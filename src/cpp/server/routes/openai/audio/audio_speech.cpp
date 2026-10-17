#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;

const RouteError kNotTtsModel{
    400, "model_not_applicable",
    "`model` is not a text-to-speech model, which is checked before it loads"};

const RouteArg kInput{"input", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The text to speak."};
const RouteArg kSpeed{"speed", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                      "Speaking speed. Defaults to `1.0`.", {}, true};
const RouteArg kVoice{"voice", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Partial,
                      "For `kokoro-v1`, a voice name: OpenAI's voices (`alloy`, `ash`, ...) or Kokoro's own "
                      "(`af_sky`, `am_echo`, ...), defaulting to `shimmer`. For OpenMOSS models, a free-text "
                      "voice or style instruction, e.g. `a calm, deep male narrator voice`.", {}, true};
const RouteArg kReferenceWavB64{"reference_wav_b64", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                                "Lemonade extension (OpenMOSS voice cloning): base64-encoded WAV sample of the "
                                "voice to clone."};
const RouteArg kVoiceDesignDescription{"voice_design_description", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                                       "Lemonade extension (OpenMOSS voice design): a description of the voice to invent, "
                                       "e.g. `a warm low female voice with a British accent`. Lemonade renders a short "
                                       "sample in that voice and uses it as the reference, so the effect is the same as "
                                       "supplying `reference_wav_b64` yourself. Ignored when `reference_wav_b64` is also "
                                       "present. Only this field triggers design; `voice` never does."};
const RouteArg kResponseFormat{"response_format", ArgIn::JsonBody,
                               {{"enum", json::array({"mp3", "opus", "aac", "flac", "wav", "pcm"})}}, false, Support::Partial,
                               "Container for the returned audio. Defaults to `mp3` when buffered and `pcm` when "
                               "streaming, or to the backend's first supported format when it cannot encode those."};
const RouteArg kStreamFormat{"stream_format", ArgIn::JsonBody, {{"const", "audio"}}, false, Support::Partial,
                             "`audio` streams the response. It selects the transport only: the container still "
                             "comes from `response_format`."};
const RouteArg kStream{"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` streams the response, like `stream_format: audio`."};

class AudioSpeechRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.audio_speech";
        s.methods = {"POST"};
        s.paths = {"audio/speech"};
        s.summary = "Generate speech from text";
        s.modality = "text -> audio";
        s.description =
            "Generates speech audio from text. The model selects the engine: `kokoro-v1` speaks "
            "with fixed voices ([Kokoros](https://github.com/lucasjinreal/Kokoros)), while "
            "`OpenMOSS-TTS` and `MOSS-TTS-Local` can also clone a voice or design one from a "
            "description. `MOSS-VoiceGen` is kept for compatibility.";
        s.notes = {
            "Accepted `response_format` values depend on the backend: `kokoro-v1` encodes `mp3`, "
            "`wav`, `opus` and `pcm`, and OpenMOSS encodes `wav` or `pcm`. Streaming produces "
            "`pcm` only. OpenMOSS PCM is returned as `audio/pcm` with `X-MOSS-Sample-Rate` and "
            "`X-MOSS-Channels` headers, since its format depends on the model (24 kHz mono for "
            "`OpenMOSS-TTS`, 48 kHz stereo for `MOSS-TTS-Local`).",
        };
        s.args = {
            kInput, model_loading::kModel, kSpeed, kVoice, kReferenceWavB64,
            kVoiceDesignDescription, kResponseFormat, kStreamFormat, kStream
        };

        RouteResponse audio;
        audio.format = ResponseFormat::Binary;

        RouteResponse stream;
        stream.format = ResponseFormat::BinaryStream;

        s.responses = {audio, stream};

        RouteExample audio_example;
        audio_example.name = "non-streaming";
        audio_example.format = ResponseFormat::Binary;
        audio_example.args = {{"model", "kokoro-v1"}, {"input", "Lemonade can speak!"}, {"speed", 1.0},
                         {"response_format", "mp3"}};

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::BinaryStream;
        stream_example.args = {{"model", "kokoro-v1"}, {"input", "Lemonade can speak!"}, {"speed", 1.0},
                          {"stream_format", "audio"}};

        s.examples = {audio_example, stream_example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kUnexpectedException,
                                 kNotTtsModel, kUnsupportedAudioFormat, kNoOutputError, kBackendError},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        if (auto info = ctx_.router->try_get_model_info(req.model);
            info && info->type != ModelType::TTS) {
            write_openai_error(res, kNotTtsModel,
                               "Model '" + req.model + "' is not a text-to-speech model");
            return false;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        json& request_json = req.body;
        bool is_streaming = req.value(kStream, false);

        if (req.has(kStreamFormat) && req.arg(kStreamFormat) == "audio") {
            is_streaming = true;
        }

        const AudioFormat format =
            negotiate_speech_format(*ctx_.router, req.model, req.arg(kResponseFormat), is_streaming);
        if (!apply_audio_format(format, res)) {
            return;
        }
        // The backend must encode what the Content-Type promises, including a default
        // format the client never sent.
        request_json[kResponseFormat.name] = format.format;

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

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in audio speech: " << error.what() << std::endl;
        write_openai_error(res, kUnexpectedException, error.what(), "internal_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_audio_speech_route(ServerContext& ctx) {
    return std::make_unique<AudioSpeechRoute>(ctx);
}

} // namespace lemon
