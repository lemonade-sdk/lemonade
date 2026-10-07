#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class PullRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.pull";
        s.methods = {"POST"};
        s.paths = {"/api/pull"};
        s.prefixes = Prefixes::Root;
        s.summary = "Download a model, with progress";
        s.description =
            "Downloads a registered model, or updates it when its registry has a newer "
            "version.";
        s.notes = {
            "Only models already in Lemonade's registry can be pulled; use "
            "[`POST /v1/pull`](./lemonade.md#post-v1pull) to register a new one. An unknown model answers "
            "`404` with `model '<name>' not found`.",
            "Ollama streams by default: without `\"stream\": false` the response is "
            "newline-delimited JSON progress. A failed download ends the stream with an "
            "`error` line.",
            "In offline mode the request answers `400` with code `lemond_offline`.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model to download. A `:latest` tag is ignored."},
            {"name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Older name for `model`; wins when both are given."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Stream newline-delimited JSON progress. Defaults to `true`."},
        };

        RouteResponse stream;
        stream.format = ResponseFormat::JsonLines;
        stream.schema = json::parse(R"({
            "type": "object",
            "description": "The first line is pulling manifest, then one line per progress report, then success.",
            "properties": {
                "status": {"type": "string", "description": "pulling manifest, downloading <file>, or success."},
                "digest": {"type": "string", "description": "Download lines: sha256: followed by the file name."},
                "completed": {"type": "integer", "description": "Download lines: bytes of the file downloaded so far."},
                "total": {"type": "integer", "description": "Download lines: the file's size in bytes."},
                "error": {"type": "string", "description": "Only on a failed download's last line."}
            }
        })");
        stream.example = json::parse(R"({"model": "Qwen3-0.6B-GGUF", "stream": true})");

        RouteResponse full;
        full.format = ResponseFormat::Json;
        full.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "success"}}
        })");
        full.example = json::parse(R"({"model": "RealESRGAN-x4plus-anime", "stream": false})");

        s.responses = {stream, full};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);
            std::string name = strip_latest_tag(request_json.value("name", request_json.value("model", "")));

            if (name.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"name is required"})", "application/json");
                return;
            }

            bool stream = request_json.value("stream", true);

            if (!ctx_.model_manager->model_exists(name)) {
                res.status = 404;
                json error = {{"error", "model '" + name + "' not found"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            if (ctx_.config->offline()) {
                res.status = 400;
                json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            LOG(INFO, "OllamaApi") << "POST /api/pull - Pulling model: " << name << std::endl;

            if (stream) {
                ModelManager* model_manager = ctx_.model_manager;
                stream_response(req, res, [model_manager, name](const std::string&, httplib::DataSink& sink) {
                    try {
                        std::string init = json({{"status", "pulling manifest"}}).dump() + "\n";
                        sink.write(init.c_str(), init.size());

                        auto info = model_manager->get_model_info(name);

                        DownloadProgressCallback progress_cb = [&sink](const DownloadProgress& p) -> bool {
                            json progress;
                            if (p.complete) {
                                progress["status"] = "success";
                            } else {
                                progress["status"] = "downloading " + p.file;
                                progress["digest"] = "sha256:" + p.file;
                                progress["completed"] = static_cast<uint64_t>(p.bytes_downloaded);
                                progress["total"] = static_cast<uint64_t>(p.bytes_total);
                            }

                            std::string ndjson = progress.dump() + "\n";
                            if (!sink.write(ndjson.c_str(), ndjson.size())) {
                                return false;
                            }
                            return true;
                        };

                        model_manager->download_model(name, json::object(), false, progress_cb);

                        std::string success = json({{"status", "success"}}).dump() + "\n";
                        sink.write(success.c_str(), success.size());

                    } catch (const std::exception& e) {
                        std::string error_msg = e.what();
                        if (error_msg != "Download cancelled") {
                            json error = {{"error", error_msg}};
                            std::string ndjson = error.dump() + "\n";
                            sink.write(ndjson.c_str(), ndjson.size());
                        }
                    }

                    sink.done();
                }, "application/x-ndjson");
            } else {
                ctx_.model_manager->download_model(name, json::object());
                json response = {{"status", "success"}};
                res.set_content(response.dump(), "application/json");
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/pull: " << e.what() << std::endl;
            res.status = 500;
            json error = {{"error", std::string(e.what())}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_pull_route(ServerContext& ctx) {
    return std::make_unique<PullRoute>(ctx);
}

} // namespace lemon
