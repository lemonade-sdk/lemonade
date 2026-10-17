#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/model_sync.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kPullFailed{500, "", "A non-streaming download fails"};


class PullRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.pull";
        s.methods = {"POST"};
        s.paths = {"/api/pull"};
        s.prefixes = Prefixes::Root;
        s.summary = "Download a model";
        s.description =
            "Downloads a registered model, or updates it when its registry has a newer "
            "version.";
        s.notes = {
            "Only models already in Lemonade's registry can be pulled; use "
            "[`POST /v1/pull`](./lemonade.md#post-v1pull) to register a new one.",
        };
        s.args = {ollama::management::kModel, ollama::kStream};

        RouteResponse stream;
        stream.format = ResponseFormat::JsonLines;
        stream.schema = ordered_json::parse(R"({
            "type": "object",
            "description": "The first line is `pulling manifest`, then one line per progress report, then two `success` lines.",
            "properties": {
                "status": {"type": "string", "description": "`pulling manifest`, `downloading <file>`, or `success`."},
                "digest": {"type": "string", "description": "Download lines: `sha256:` followed by the file name."},
                "completed": {"type": "integer", "description": "Download lines: bytes of the file downloaded so far."},
                "total": {"type": "integer", "description": "Download lines: the file's size in bytes."},
                "error": {"type": "string", "description": "Only on a failed download's last line."}
            }
        })");

        RouteResponse full;
        full.format = ResponseFormat::Json;
        full.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "success", "description": "Always `success`."}}
        })");

        s.responses = {stream, full};

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::JsonLines;
        stream_example.args = ordered_json::parse(R"({"model": "Qwen3-0.6B-GGUF", "stream": true})");

        RouteExample full_example;
        full_example.name = "non-streaming";
        full_example.format = ResponseFormat::Json;
        full_example.args = ordered_json::parse(R"({"model": "RealESRGAN-x4plus-anime", "stream": false})");

        s.examples = {stream_example, full_example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kOllamaModelNotFound, kLemondOffline, kPullFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string name = strip_latest_tag(req.arg(ollama::management::kModel).get<std::string>());
            if (!ollama_model_registered(*ctx_.model_manager, name, res)) {
                return;
            }

            bool stream = req.value(ollama::kStream, true);

            if (refuse_offline_download(ctx_, res)) {
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
            write_plain_error(res, kPullFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_pull_route(ServerContext& ctx) {
    return std::make_unique<PullRoute>(ctx);
}

} // namespace lemon
