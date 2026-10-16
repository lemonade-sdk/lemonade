#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNotAudioModel{
    400, "model_not_applicable", "`model` is not an audio-generation model"};
const RouteError kUnsupportedFormat{
    400, "", "`response_format` is not a format the model's backend produces"};
const RouteError kGenerationException{500, "", "Generation fails unexpectedly"};

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
            "Generates an audio clip from a text prompt. The model decides the kind of audio: music "
            "with ACE-Step models such as `ACE-Step-Music`, sound effects with ThinkSound models "
            "such as `ThinkSound-SFX`. This is a Lemonade extension; OpenAI's audio endpoints cover "
            "only speech and transcription.";
        s.notes = {
            "Generation runs on the GPU (Vulkan, ROCm or CUDA) and takes from seconds for a short "
            "sound effect to minutes for full-length music, depending on duration and hardware.",
            "Errors carry a JSON `error` object.",
            "### Lyrics\n\n"
            "ACE-Step vocals are a two-stage pipeline inside the backend: a language model first "
            "turns the style description and lyrics into audio codes, then the diffusion "
            "synthesizer renders those codes into audio. The instrumental path skips the "
            "language-model stage entirely, which also means lyrics embedded in the `prompt` field "
            "are treated as style text and never sung. Vocal generations take noticeably "
            "longer than instrumental ones of the same duration because of the extra "
            "language-model pass.\n\n"
            "Format the `lyrics` value the way the ACE-Step authors recommend:\n\n"
            "- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, "
            "`[bridge]`, `[intro]`, `[outro]`.\n"
            "- Write one sung phrase per line and separate sections with a blank line.\n"
            "- Describe the voice (\"gentle female vocals\", \"raspy male baritone\") in `prompt`, "
            "not in the lyrics.\n"
            "- Lyrics may be in any supported language; set `vocal_language` to match.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The audio-generation model."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text description of the music or sound effect to generate. For music, this is the "
             "style description: genre, mood, tempo, instruments, and voice."},
            {"lyrics", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Lyrics to sing (ACE-Step only). When present and not empty, the track is generated "
             "with vocals singing these lyrics. Omitting the field, an empty string, or the "
             "sentinel `[Instrumental]` (any case) produces an instrumental track. See "
             "[Lyrics](#lyrics) below for the expected format."},
            {"vocal_language", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Defaults "
             "to `en`."},
            {"duration", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Length of the clip in seconds. Defaults to the backend's own default."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of inference steps. Lower is faster, higher can improve quality."},
            {"cfg", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance strength (ThinkSound only)."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducibility."},
            {"response_format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Output encoding. Only `wav`, the format the backend produces, is accepted. "
             "Defaults to `wav`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        s.responses = {response};

        RouteExample sound;
        sound.name = "sound effect";
        sound.format = ResponseFormat::Binary;
        sound.args = ordered_json::parse(R"({
            "model": "ThinkSound-SFX",
            "prompt": "glass shattering on a stone floor",
            "duration": 5,
            "seed": 42
        })");

        RouteExample song;
        song.name = "music with vocals";
        song.format = ResponseFormat::Binary;
        song.args = ordered_json::parse(R"({
            "model": "ACE-Step-Music",
            "prompt": "warm acoustic folk ballad, fingerpicked guitar, gentle female vocals",
            "lyrics": "[verse]\nMoonlight spills across the floor\nShadows dancing by the door\n\n[chorus]\nWe sing until the morning light\nCarried on the wind tonight",
            "duration": 30,
            "seed": 42
        })");

        s.examples = {sound, song};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNotAudioModel,
                                 kUnsupportedFormat, kNoOutputError, kBackendError,
                                 kGenerationException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        if (auto info = ctx_.router->try_get_model_info(req.model);
            info && info->type != ModelType::AUDIO_GENERATION) {
            write_openai_error(res, kNotAudioModel,
                               "Model '" + req.model + "' is not an audio-generation model");
            return false;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        AudioFormat format = negotiate_audio_generation_format(*ctx_.router, req.model, req.body);
        if (!format.error.empty()) {
            write_openai_error(res, kUnsupportedFormat, format.error);
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

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in handle_audio_generations: " << error.what() << std::endl;
        write_openai_error(res, kGenerationException, error.what(), "internal_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_audio_generations_route(ServerContext& ctx) {
    return std::make_unique<AudioGenerationsRoute>(ctx);
}

} // namespace lemon
