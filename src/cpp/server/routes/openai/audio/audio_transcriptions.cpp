#include <lemon/utils/aixlog.hpp>

#include "lemon/audio_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class AudioTranscriptionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.audio_transcriptions";
        s.methods = {"POST"};
        s.paths = {"audio/transcriptions"};
        s.summary = "Audio Transcription";
        s.description =
            "Transcribes an audio file to text, loading the model on first use. The request is "
            "`multipart/form-data`.";
        s.notes = {
            "Transcription models such as the Whisper family are downloaded automatically when "
            "first used.",
            "**Limitations:** Only `wav` audio input is supported. On the FastFlowLM (FLM) "
            "backend, `srt` and `vtt` are rejected with a `400` because FLM returns no segment "
            "timestamps, and `verbose_json` returns the compact shape without a `segments` field.",
        };
        s.args = {
            {"file", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Partial,
             "The audio file to transcribe. Only `wav` is supported."},
            {"model", ArgIn::Form, {{"type", "string"}}, true, Support::Available,
             "Transcription model, e.g. `Whisper-Tiny`, `Whisper-Base` or `Whisper-Small`."},
            {"language", ArgIn::Form, {{"type", "string"}}, false, Support::Available,
             "Language of the audio as an ISO 639-1 code, e.g. `en`, `es` or `fr`. Defaults to "
             "`auto`, which has whisper.cpp detect the language instead of assuming English."},
            {"prompt", ArgIn::Form, {{"type", "string"}}, false, Support::Available,
             "Text to guide the transcription's style or continue a previous segment."},
            {"response_format", ArgIn::Form,
             {{"enum", json::array({"json", "verbose_json", "text", "srt", "vtt"})}}, false, Support::Available,
             "`json` (default), `verbose_json`, `text`, `srt` or `vtt`. `text`, `srt` and `vtt` "
             "answer with plain text; `srt` and `vtt` need a backend that reports segment "
             "timestamps, such as whisper.cpp."},
            {"temperature", ArgIn::Form, {{"type", "number"}}, false, Support::Available,
             "Sampling temperature."},
        };

        RouteResponse transcript;
        transcript.format = ResponseFormat::Json;
        transcript.schema = json::parse(R"({
            "type": "object",
            "required": ["text"],
            "properties": {
                "text": {"type": "string", "description": "The transcribed text."},
                "segments": {"type": "array", "description": "verbose_json only: timestamped segments."}
            }
        })");
        transcript.example = {{"model", "Whisper-Tiny"}, {"file", "@fixtures/speech.wav"}};

        RouteResponse text;
        text.format = ResponseFormat::Text;
        text.example = {{"model", "Whisper-Tiny"}, {"file", "@fixtures/speech.wav"},
                        {"response_format", "text"}};

        s.responses = {transcript, text};
        s.request_format = RequestFormat::Form;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/audio/transcriptions" << std::endl;

        const std::string response_format =
            req.body.value("response_format", lemon::audio::ResponseFormat::JSON);
        if (!lemon::audio::is_supported_response_format(response_format)) {
            write_openai_error(res, 400, "Unsupported response_format: " + response_format);
            return false;
        }

        const httplib::FormData* file = find_form_file(req.http, {"file"});
        if (!file) {
            write_openai_error(res, 400, "Missing 'file' field in request");
            return false;
        }
        req.body["file_data"] = file->content;
        req.body["filename"] = file->filename;
        LOG(INFO, "Server") << "Audio file: " << file->filename
                            << " (" << file->content.size() << " bytes)" << std::endl;
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        const std::string response_format =
            req.body.value("response_format", lemon::audio::ResponseFormat::JSON);

        auto response = ctx_.router->audio_transcriptions(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, 500);
            return;
        }

        if (lemon::audio::is_plain_text_format(response_format) &&
            response.contains("text") && response["text"].is_string()) {
            res.set_content(response["text"].get<std::string>(), "text/plain");
            return;
        }

        res.set_content(response.dump(), "application/json");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in audio transcriptions: " << error.what() << std::endl;
        write_openai_error(res, 500, error.what(), "internal_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_audio_transcriptions_route(ServerContext& ctx) {
    return std::make_unique<AudioTranscriptionsRoute>(ctx);
}

} // namespace lemon
