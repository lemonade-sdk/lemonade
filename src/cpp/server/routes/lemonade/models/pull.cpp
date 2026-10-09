#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"
#include "lemon/server/model_registration.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json pull_schema() {
    json success = json::parse(R"({
        "type": "object",
        "required": ["status", "model_name"],
        "properties": {
            "status": {"const": "success"},
            "model_name": {"type": "string"},
            "message": {"type": "string", "description": "Only for local_import."}
        }
    })");
    json job = DownloadManager::job_schema();
    job["description"] = "With stream=true and subscribe=false: the server-owned download job.";
    return {{"oneOf", json::array({success, job})}};
}

class PullRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.pull";
        s.methods = {"POST"};
        s.paths = {"pull"};
        s.summary = "Install a model";
        s.description =
            "Installs a model from the built-in registry, or registers and installs any model "
            "from Hugging Face or ModelScope.";
        s.notes = {
            "A model is registered first, through the same path as "
            "[`POST /v1/models/register`](#post-v1modelsregister): a new definition needs a "
            "`user.` name, a `recipe`, and a `checkpoint` or a `checkpoints` object with a "
            "`main` key. Registration adds the model to `user_models.json` in the Lemonade "
            "config directory (default `~/.config/lemonade`), after which `/v1/models` lists it "
            "like a built-in model.",
            "Only this endpoint checks a downloaded model's registry for updates, unless "
            "`do_not_upgrade` is `true`; loading a model on first use never does.",
            "**Progress:** `stream: true` sends server-sent events: `progress` during each file's "
            "download, `complete` when every file is done, and `error`, whose data carries "
            "`error`, on failure. With `subscribe: false` as well, the server owns the download "
            "instead and answers at once with a job snapshot; clients poll "
            "[`GET /v1/downloads`](#get-v1downloads) to restore progress after a reload, tab "
            "close or reconnect, and use [`POST /v1/downloads/control`](#post-v1downloadscontrol) "
            "to pause, cancel or remove the job.",
            "A `400` answers an invalid definition, an unknown model (with `code: "
            "\"unknown_model\"`), or offline mode (with `code: \"lemond_offline\"`), and nothing is "
            "registered.",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "[Lemonade model name](https://lemonade-server.ai/models.html) to install, or a "
             "`user.`-namespaced name to register and install. `model` is accepted as an alias."},
            {"checkpoint", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registering only: main checkpoint, such as `unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M`. "
             "A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its "
             "registry."},
            {"checkpoints", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Registering only: checkpoints by role (`main`, `mmproj`, `draft`, `text_encoder`, "
             "`vae`, ...), for multi-checkpoint models."},
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registering only: recipe that loads the model."},
            {"mmproj", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registering only: multimodal projector file for vision models."},
            {"labels", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Registering only: model labels. A model deploys in exactly one "
             "[deployment mode](./openai.md#model-labels): naming a mode the recipe cannot "
             "serve, or naming two, is rejected with `400`. Omitting the deployment label "
             "applies the recipe's default."},
            {"reasoning", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Registering only: adds the `reasoning` label. Defaults to `false`."},
            {"vision", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Registering only: adds the `vision` label. Defaults to `false`."},
            {"embedding", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Registering only: adds the `embeddings` label. Defaults to `false`."},
            {"reranking", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Registering only: adds the `reranking` label. Defaults to `false`."},
            {"components", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Collections only: ordered, non-empty component model names. Components that are "
             "not downloaded yet are pulled by the same call; deleting the collection later "
             "removes only its entry, not the components."},
            {"models", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Collections only: full model definitions, one per `components` entry. Components "
             "not yet registered are registered from them; existing names keep their local "
             "definition."},
            {"source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registry to download from: `huggingface` or `modelscope`. Defaults to the server's "
             "`default_model_source`."},
            {"registry_source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Same as `source`; when both are given they must name the same registry."},
            {"do_not_upgrade", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` skips checking an already-downloaded model's registry for updates. Defaults "
             "to `false`."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Send download progress as server-sent events. Defaults to `false`."},
            {"subscribe", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Only with `stream: true`: `false` starts a server-owned download job and answers "
             "with its snapshot instead of streaming. Defaults to `true`."},
            {"local_import", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Register model files a client already copied into the Hugging Face cache, without "
             "downloading. Defaults to `false`."},
        };

        RouteResponse job;
        job.format = ResponseFormat::Json;
        job.schema = pull_schema();
        job.example = json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
            "do_not_upgrade": true,
            "stream": true,
            "subscribe": false
        })");

        RouteResponse events;
        events.format = ResponseFormat::EventStream;
        events.schema = DownloadManager::event_schema();
        events.example = json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
            "do_not_upgrade": true,
            "stream": true
        })");

        s.responses = {job, events};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        json& request_json = req.body;
        try {
            // Accept both "model" and "model_name" for compatibility
            std::string model_name = request_json.contains("model") ?
                request_json["model"].get<std::string>() :
                request_json["model_name"].get<std::string>();

            std::string checkpoint = request_json.value("checkpoint", "");
            std::string recipe = request_json.value("recipe", "");
            bool do_not_upgrade = request_json.value("do_not_upgrade", false);
            bool stream = request_json.value("stream", false);
            bool subscribe = request_json.value("subscribe", true);
            bool local_import = request_json.value("local_import", false);

            // Resolve the pull's registry provenance once, up front, so download,
            // cache layout, and later refresh stay consistent. A registry-backed
            // checkpoint that names no source inherits the configured default; a
            // provider URL is normalized to owner/repo and its registry adopted.
            // Self-managed backends (flm/cloud), non-registry checkpoints, explicit
            // sources, and bare `pull <registered-name>` refreshes are left as-is.
            try {
                lemon::apply_default_pull_source(request_json, ctx_.config->default_model_source());
            } catch (const std::invalid_argument& e) {
                write_plain_error(res, 400, e.what());
                return;
            }
            // The helper may have rewritten a provider URL into an owner/repo id.
            checkpoint = request_json.value("checkpoint", "");

            LOG(INFO, "Server") << "Pulling model: " << model_name << std::endl;
            if (!checkpoint.empty()) {
                LOG(INFO, "Server") << "   checkpoint: " << checkpoint << std::endl;
            }
            if (!recipe.empty()) {
                LOG(INFO, "Server") << "   recipe: " << recipe << std::endl;
            }

            const bool collection_file_import =
                request_json.contains("models") && request_json["models"].is_array();

            try {
                register_model_definition(
                    ctx_,
                    model_name,
                    request_json,
                    /*require_definition=*/false,
                    /*allow_embedded_models=*/true,
                    local_import);
            } catch (const std::invalid_argument& e) {
                write_plain_error(res, 400, e.what());
                return;
            }

            if (local_import) {
                json response = {
                    {"status", "success"},
                    {"model_name", model_name},
                    {"message", "Model imported and registered successfully"}
                };
                res.set_content(response.dump(), "application/json");
                return;
            }

            if (ctx_.config->offline()) {
                res.status = 400;
                json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            json download_request = collection_file_import ? request_json : json::object();
            for (const char* field : {"source", "registry_source"}) {
                if (request_json.contains(field)) {
                    download_request[field] = request_json[field];
                }
            }

            if (stream) {
                ModelManager* model_manager = ctx_.model_manager;
                DownloadManager::Operation operation =
                    [model_manager, model_name, download_request, do_not_upgrade](DownloadProgressCallback progress_cb) {
                        model_manager->download_model(model_name, download_request, do_not_upgrade, progress_cb);
                    };

                if (!subscribe) {
                    json response = ctx_.downloads->start("model:" + model_name, "model", model_name, operation);
                    res.set_content(response.dump(), "application/json");
                    return;
                }

                stream_response(req, res, [operation](const std::string&, httplib::DataSink& sink) {
                    DownloadManager::stream(operation, sink);
                });
            } else {
                ctx_.model_manager->download_model(model_name, download_request, do_not_upgrade);

                json response = {{"status", "success"}, {"model_name", model_name}};
                res.set_content(response.dump(), "application/json");
            }

        } catch (const lemon::UnknownModelError& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
            res.status = 400;
            json error = {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}};
            res.set_content(error.dump(), "application/json");
        } catch (const lemon::InvalidModelDefinitionError& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
            write_plain_error(res, 400, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_pull_route(ServerContext& ctx) {
    return std::make_unique<PullRoute>(ctx);
}

} // namespace lemon
