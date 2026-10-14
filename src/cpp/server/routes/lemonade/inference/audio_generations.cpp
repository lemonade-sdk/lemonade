#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class AudioGenerationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.audio_generations";
        s.methods = {"POST"};
        s.paths = {"audio/generations"};
        s.summary = "Generate audio (music or sound effects) from a text prompt";
        s.description =
            "Audio Generation API. You provide a text prompt and receive a generated audio clip. "
            "The loaded model decides the kind of audio: music with ACE-Step models (e.g. "
            "`ACE-Step-Music`), sound effects with ThinkSound models (e.g. `ThinkSound-SFX`).\n\n"
            "This endpoint is not part of the OpenAI API (OpenAI's audio endpoints cover speech "
            "and transcription only), so it is a Lemonade-specific extension.\n\n"
            "> **Performance:** generation runs on the GPU (Vulkan, ROCm, or CUDA) and takes from "
            "seconds (short sound effects) to minutes (full-length music) depending on duration "
            "and hardware.";
        s.notes = {
            "### Lyrics\n\n"
            "ACE-Step vocals are a two-stage pipeline inside the backend: a language model first "
            "turns the style description and lyrics into audio codes, then the diffusion "
            "synthesizer renders those codes into audio. The instrumental path skips the "
            "language-model stage entirely, which also means lyrics embedded in the `prompt` field "
            "are treated as style text — they are never sung. Vocal generations take noticeably "
            "longer than instrumental ones of the same duration because of the extra "
            "language-model pass.\n\n"
            "Format the `lyrics` value the way the ACE-Step authors recommend:\n\n"
            "- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, "
            "`[bridge]`, `[intro]`, `[outro]`.\n"
            "- Write one sung phrase per line and separate sections with a blank line.\n"
            "- Describe the voice (\"gentle female vocals\", \"raspy male baritone\") in `prompt`, "
            "not in the lyrics.\n"
            "- Lyrics may be in any supported language; set `vocal_language` to match.",
            "### Response\n\n"
            "On success the raw audio bytes are returned with the matching content type "
            "(`audio/wav`). On failure the response is JSON with an `error` object: `400` for "
            "invalid requests, `404` for unknown models, `500` when the backend reports an error, "
            "and `502` when the backend produces no output.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The audio-generation model to use (e.g., `ThinkSound-SFX`, `ACE-Step-Music`)."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text description of the music or sound effect to generate. For music, this is the "
             "style description: genre, mood, tempo, instruments, and voice."},
            {"lyrics", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Lyrics to sing (ACE-Step only). When present and not empty, the track is generated "
             "with vocals singing these lyrics. Omitting the field, an empty string, or the "
             "sentinel `[Instrumental]` (any case) produces an instrumental track. See "
             "[Lyrics](#lyrics) below for the expected format."},
            {"vocal_language", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Default: "
             "`en`."},
            {"duration", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Length of the clip in seconds. Defaults to the backend's native default."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of inference steps. Lower is faster, higher can improve quality."},
            {"cfg", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance strength (ThinkSound only)."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducibility."},
            {"response_format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Output encoding. Only formats the backend natively produces are accepted (currently "
             "`wav`); other values are rejected with `400 Bad Request`. Default: `wav`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        response.example = ordered_json::parse(R"({
            "model": "ThinkSound-SFX",
            "prompt": "glass shattering on a stone floor",
            "duration": 5,
            "seed": 42
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        // A missing model is reported before a missing prompt.
        if (!req.body.contains("model")) {
            return true;
        }
        if (!req.body.contains("prompt")) {
            write_openai_error(res, 400, "Missing 'prompt' field in request");
            return false;
        }
        for (const auto* field : {"prompt", "lyrics", "vocal_language"}) {
            if (req.body.contains(field) && !req.body[field].is_string()) {
                write_openai_error(res, 400, "'" + std::string(field) + "' must be a string");
                return false;
            }
        }

        req.body["model"].get<std::string>();  // a non-string model fails before any load
        if (auto info = ctx_.router->try_get_model_info(req.model);
            info && info->type != ModelType::AUDIO_GENERATION) {
            write_openai_error(res, 400,
                               "Model '" + req.model + "' is not an audio-generation model",
                               "invalid_request_error", "model_not_applicable");
            return false;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        AudioFormat format = negotiate_audio_generation_format(*ctx_.router, req.model, req.body);
        if (!format.error.empty()) {
            write_openai_error(res, 400, format.error);
            return;
        }
        for (const auto& [name, value] : format.headers) {
            res.set_header(name, value);
        }

        LOG(INFO, "Server") << "POST /api/v1/audio/generations" << std::endl;

        Router* router = ctx_.router;
        serve_media_or_error(res, format.mime_type, [router, &req](httplib::DataSink& sink) {
            router->audio_generations(req.body, sink);
        });
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_openai_error(res, 500, error.what(), "internal_error");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in handle_audio_generations: " << error.what() << std::endl;
        write_openai_error(res, 500, error.what(), "internal_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_audio_generations_route(ServerContext& ctx) {
    return std::make_unique<AudioGenerationsRoute>(ctx);
}

} // namespace lemon
