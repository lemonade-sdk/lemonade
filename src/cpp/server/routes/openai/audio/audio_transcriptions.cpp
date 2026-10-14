#include <lemon/utils/aixlog.hpp>

#include "lemon/audio_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class AudioTranscriptionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.audio_transcriptions";
        s.methods = {"POST"};
        s.paths = {"audio/transcriptions"};
        s.summary = "Transcribe an audio file to text";
        s.modality = "audio file -> text";
        s.description =
            "Transcribes an audio file to text, loading the model on first use. Whisper "
            "(whisper.cpp), Moonshine and FastFlowLM models serve this endpoint.";
        s.notes = {
            "On FastFlowLM, which reports no segment timestamps, `srt` and `vtt` answer `400` and "
            "`verbose_json` has no `segments`.",
        };
        s.args = {
            {"file", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Partial,
             "The audio file to transcribe, as WAV."},
            {"model", ArgIn::Form, {{"type", "string"}}, true, Support::Available,
             "The transcription model to use (e.g., `Whisper-Tiny`)."},
            {"language", ArgIn::Form, {{"type", "string"}}, false, Support::Available,
             "The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). Defaults to the "
             "backend's own."},
            {"response_format", ArgIn::Form,
             {{"enum", json::array({"json", "verbose_json", "text", "srt", "vtt"})}}, false, Support::Available,
             "`json`, `verbose_json`, `text`, `srt` or `vtt`. Defaults to `json`."},
            {"prompt", ArgIn::Form, {{"type", "string"}}, false, Support::Available,
             "Text to guide the transcription's style or continue a previous segment."},
            {"temperature", ArgIn::Form, {{"type", "number"}}, false, Support::Available,
             "Sampling temperature."},
        };

        RouteResponse transcript;
        transcript.format = ResponseFormat::Json;
        transcript.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["text"],
            "properties": {
                "text": {"type": "string", "description": "The transcribed text."},
                "segments": {"type": "array", "description": "`verbose_json` only: timestamped segments."}
            }
        })");

        RouteResponse text;
        text.format = ResponseFormat::Text;

        s.responses = {transcript, text};

        RouteExample transcript_example;
        transcript_example.name = "transcript";
        transcript_example.format = ResponseFormat::Json;
        transcript_example.args = {{"file", "@fixtures/speech.wav"}, {"model", "Whisper-Tiny"}};

        RouteExample text_example;
        text_example.name = "plain text";
        text_example.format = ResponseFormat::Text;
        text_example.args = {{"file", "@fixtures/speech.wav"}, {"model", "Whisper-Tiny"},
                        {"response_format", "text"}};

        s.examples = {transcript_example, text_example};
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
