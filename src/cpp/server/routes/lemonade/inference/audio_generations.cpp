#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Generates an audio clip from a text prompt. The model decides the kind of audio: "
            "music with ACE-Step models (e.g. `ACE-Step-Music`), sound effects with ThinkSound "
            "models (e.g. `ThinkSound-SFX`).";
        s.notes = {
            "This is a Lemonade extension: OpenAI's audio endpoints cover only speech and "
            "transcription.",
            "**Performance:** generation runs on the GPU (Vulkan, ROCm or CUDA) and takes from "
            "seconds for short sound effects to minutes for full-length music.",
            "A failure is answered with a JSON `error` object instead of audio: `400` for an "
            "invalid request, `404` for an unknown model, `500` when the backend reports an "
            "error, and `502` when the backend produces no output.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Audio-generation model, e.g. `ThinkSound-SFX` or `ACE-Step-Music`; loaded on "
             "first use."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Description of the music or sound effect. For music, this is the style: genre, "
             "mood, tempo, instruments and voice."},
            {"lyrics", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "ACE-Step only: lyrics to sing. Omitted, empty, or `[Instrumental]` (any case) "
             "produces an instrumental track; see [Lyrics](#lyrics) for the format."},
            {"vocal_language", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "ACE-Step only: BCP-47 language code of the lyrics, e.g. `en`, `fr` or `ja`. "
             "Defaults to `en`."},
            {"duration", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Length of the clip in seconds. Defaults to the backend's own default."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Inference steps. Fewer is faster; more can improve quality."},
            {"cfg", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "ThinkSound only: classifier-free guidance strength."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed, for reproducible output."},
            {"response_format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Output encoding. Only formats the backend produces natively are accepted "
             "(currently `wav`); any other is answered with `400`. Defaults to `wav`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        response.example = json::parse(R"({
            "model": "ThinkSound-SFX",
            "prompt": "glass shattering on a stone floor",
            "duration": 2,
            "steps": 8,
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
