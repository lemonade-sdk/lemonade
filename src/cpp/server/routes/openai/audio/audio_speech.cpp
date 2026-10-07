#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class AudioSpeechRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.audio_speech";
        s.methods = {"POST"};
        s.paths = {"audio/speech"};
        s.summary = "Text to speech";
        s.description =
            "Speaks the input text and returns the audio, loading the model on first use. Which "
            "engine serves the request depends on the model.";
        s.notes = {
            "Supported models are `kokoro-v1` (fixed voices, "
            "[Kokoros](https://github.com/lucasjinreal/Kokoros) backend) and the OpenMOSS "
            "family: `OpenMOSS-TTS` and `MOSS-TTS-Local` support cloning and integrated voice "
            "design. `MOSS-VoiceGen` remains available as a legacy compatibility model.",
            "**Limitations:** Which `response_format` values are accepted depends on the model's "
            "backend: `kokoro-v1` encodes `mp3`, `wav`, `opus` and `pcm`; OpenMOSS encodes "
            "buffered `wav` or `pcm`. Streaming is narrower for both backends and uses `pcm` only, "
            "so an explicit non-PCM `response_format` on a streaming request is rejected rather "
            "than mislabeled or silently transcoded. OpenMOSS raw PCM is returned as `audio/pcm` "
            "with `X-MOSS-Sample-Rate` and `X-MOSS-Channels` headers, because its native format is "
            "model-dependent (24 kHz mono for OpenMOSS-TTS, 48 kHz stereo for MOSS-TTS-Local).",
            "A request for a model that is not a text-to-speech model answers `400` with `code` "
            "`model_not_applicable`, before any model loads.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text-to-speech model, e.g. `kokoro-v1`."},
            {"input", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The text to speak."},
            {"voice", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Partial,
             "For `kokoro-v1`, a voice name: every OpenAI voice (`alloy`, `ash`, ...) and the "
             "Kokoro voices (`af_sky`, `am_echo`, ...). Defaults to `shimmer`. For OpenMOSS "
             "models, a free-text voice or style instruction, e.g. "
             "`a calm, deep male narrator voice`."},
            {"speed", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Speaking speed. Defaults to `1.0`."},
            {"reference_wav_b64", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Lemonade extension for OpenMOSS voice cloning: a base64-encoded WAV sample of the "
             "voice to clone."},
            {"voice_design_description", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Lemonade extension for OpenMOSS voice design: a description of a voice to invent, "
             "e.g. `a warm low female voice with a British accent`. Lemonade renders a short "
             "sample in that voice and uses it as the reference, with the same effect as "
             "supplying `reference_wav_b64`. Ignored when `reference_wav_b64` is also present. "
             "Only this field triggers design; `voice` never does."},
            {"response_format", ArgIn::JsonBody,
             {{"enum", json::array({"mp3", "opus", "aac", "flac", "wav", "pcm"})}}, false, Support::Partial,
             "Container for the returned audio; which values a model accepts depends on its "
             "backend. Defaults to `mp3` when buffered and `pcm` when streaming, falling back to "
             "the backend's first supported format when it cannot encode that default."},
            {"stream_format", ArgIn::JsonBody, {{"const", "audio"}}, false, Support::Partial,
             "Set to `audio` to stream the response; no other value is supported. This selects "
             "the transport only: the container still comes from `response_format`, and an "
             "explicit one is honored on both transports."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` streams the response, like `stream_format: audio`."},
        };

        RouteResponse audio;
        audio.format = ResponseFormat::Binary;
        audio.example = {{"model", "kokoro-v1"}, {"input", "Lemonade can speak!"},
                         {"response_format", "mp3"}};

        RouteResponse stream;
        stream.format = ResponseFormat::BinaryStream;
        stream.example = {{"model", "kokoro-v1"}, {"input", "Lemonade can speak!"},
                          {"stream_format", "audio"}};

        s.responses = {audio, stream};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        if (!req.body.contains("model")) {
            return true;
        }
        req.body["model"].get<std::string>();  // a non-string model fails before any load
        if (auto info = ctx_.router->try_get_model_info(req.model);
            info && info->type != ModelType::TTS) {
            write_openai_error(res, 400, "Model '" + req.model + "' is not a text-to-speech model",
                               "invalid_request_error", "model_not_applicable");
            return false;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        json& request_json = req.body;
        if (!request_json.contains("input")) {
            write_openai_error(res, 400, "Missing 'input' field in request");
            return;
        }
        if (!request_json["input"].is_string()) {
            write_openai_error(res, 400, "'input' must be a string");
            return;
        }

        bool is_streaming = (request_json.contains("stream") && request_json["stream"].get<bool>());

        if (request_json.contains("stream_format")) {
            is_streaming = true;
            if (request_json["stream_format"] != "audio") {
                write_openai_error(res, 400, "Only 'audio' is supported for stream_format");
                return;
            }
        }

        const AudioFormat format =
            negotiate_speech_format(*ctx_.router, req.model, request_json, is_streaming);
        if (!format.error.empty()) {
            write_openai_error(res, 400, format.error);
            return;
        }
        for (const auto& [name, value] : format.headers) {
            res.set_header(name, value);
        }
        // The backend has to encode what the Content-Type promises. Without this it
        // would receive whatever the client sent (nothing, when the format came from a
        // default) and fall back to its own choice.
        request_json["response_format"] = format.format;

        LOG(INFO, "Server") << "POST /api/v1/audio/speech" << std::endl;

        Router* router = ctx_.router;
        if (is_streaming) {
            stream_response(req, res,
                [router, request_json](const std::string&, httplib::DataSink& sink) {
                    router->audio_speech(request_json, sink);
                },
                format.mime_type);
        } else {
            serve_media_or_error(res, format.mime_type,
                [router, &request_json](httplib::DataSink& sink) {
                    router->audio_speech(request_json, sink);
                });
        }
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_exception(error, res);
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in audio speech: " << error.what() << std::endl;
        write_openai_error(res, 500, error.what(), "internal_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_audio_speech_route(ServerContext& ctx) {
    return std::make_unique<AudioSpeechRoute>(ctx);
}

} // namespace lemon
