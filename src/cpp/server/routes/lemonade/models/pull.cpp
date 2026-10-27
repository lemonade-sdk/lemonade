#include <lemon/utils/aixlog.hpp>

#include "lemon/server/model_sync.h"
#include "lemon/model_manager.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"
#include "lemon/server/model_registration.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidSource{
    400, "",
    "`source` or `registry_source` names an unsupported source, or the "
    "checkpoint URLs disagree with it or with each other"};
const RouteError kUnknownModel{
    400, "unknown_model",
    "Without `stream`, the model is not registered and its name has no `user.` prefix"};
const RouteError kIllegalLabels{
    400, "",
    "Labels or `embedding`/`reranking` flags name a deployment mode the recipe cannot serve, "
    "or name two"};
const RouteError kRegistryUnavailable{
    503, "registry_unavailable",
    "Without `stream`, the model registry rate-limits or is temporarily unavailable; the "
    "`Retry-After` header and the body's `upstream_status_code` carry the details"};
const RouteError kPullFailed{
    500, "",
    "Another failure occurs, for example a failed download, a model this system cannot run, a "
    "new `user.*` model without `recipe` or `checkpoint`, or a GGUF `checkpoint` without a "
    "variant; a new definition stays registered"};

ordered_json pull_schema() {
    ordered_json success = ordered_json::parse(R"({
        "type": "object",
        "title": "Installed",
        "description": "Without `stream`, or with `local_import`: the model is installed or registered.",
        "required": ["status", "model_name"],
        "properties": {
            "status": {"const": "success", "description": "Always `success`."},
            "model_name": {"type": "string", "description": "The installed model."},
            "message": {"type": "string", "description": "Only with `local_import`: a confirmation that the files were registered."}
        }
    })");
    ordered_json job = DownloadManager::job_schema();
    job["title"] = "Download job";
    job["description"] = "With `stream: true` and `subscribe: false`: the server-owned download job.";
    return {{"oneOf", ordered_json::array({success, job})}};
}

const RouteArg kModelName{"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                          "A [registered model](https://lemonade-server.ai/models.html) to install, or a new "
                          "`user.*` name to register, such as `user.Phi-4-Mini-GGUF`. The `user.` namespace keeps "
                          "registered models from colliding with built-in ones.", {"model"}};
const RouteArg kRecipe{"recipe", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Required when registering: the recipe that loads and runs the model, such as `llamacpp`, "
                       "or `collection.omni` for an omni collection."};
const RouteArg kCheckpoint{"checkpoint", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                           "The `main` checkpoint to install. A Hugging Face or ModelScope URL is normalized to "
                           "`owner/repo` and selects its registry."};
const RouteArg kCheckpoints{"checkpoints", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
                            "Checkpoints by role, for multi-checkpoint models: `main`, plus `mmproj` for vision, "
                            "`draft` for dflash, eagle and multi-token prediction, and `text_encoder` and `vae` for "
                            "image generation, each when `main` does not embed it."};
const RouteArg kReasoning{"reasoning", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "`true` adds the `reasoning` label, for a reasoning model such as DeepSeek. Defaults "
                          "to `false`."};
const RouteArg kVision{"vision", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` adds the `vision` label, for a model that accepts images. Defaults to `false`."};
const RouteArg kEmbedding{"embedding", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "`true` adds the `embeddings` label. Defaults to `false`."};
const RouteArg kReranking{"reranking", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "`true` adds the `reranking` label. Defaults to `false`."};
const RouteArg kMmproj{"mmproj", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Multimodal projector file for a vision model, as a filename in the "
                       "`checkpoint` repository, such as `mmproj-model-f16.gguf`. "
                       "`checkpoints.mmproj` takes the same file as `repository:filename`."};
const RouteArg kComponents{"components", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
                           "Required for an omni collection registered without a `checkpoint`: the component "
                           "model names, in order; each must be a regular model, not a collection."};
const RouteArg kModels{"models", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
                       "Full model definitions, one per `components` entry, with the same fields as a single "
                       "registration. Components not yet registered are registered from them; registered "
                       "ones keep their local definition. Without `models`, every component must already be "
                       "registered."};
const RouteArg kLabels{"labels", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
                       "Registering only: model labels; see [Model Labels](./openai.md#model-labels). "
                       "Omitting the deployment label applies the recipe's default."};
const RouteArg kSource{"source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Registry to download from: `huggingface` or `modelscope`. Defaults to the server's "
                       "`default_model_source`."};
const RouteArg kRegistrySource{"registry_source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                               "Same as `source`; when both are given they must name the same registry."};
const RouteArg kDoNotUpgrade{"do_not_upgrade", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                             "`true` skips checking an already-downloaded model's registry for updates. Defaults "
                             "to `false`."};
const RouteArg kLocalImport{"local_import", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                            "Register model files a client already copied into the Hugging Face cache, without "
                            "downloading. Defaults to `false`."};

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
            "Installs a model from the built-in registry, or registers a new `user.*` model from "
            "a Hugging Face or ModelScope checkpoint and installs it.";
        s.notes = {
            "Pulling an already-downloaded model downloads any newer commit from its registry. "
            "Loading a model never checks for updates, and "
            "[`POST /v1/models/check-updates`](#post-v1modelscheck-updates) only reports them.",
            "With `stream`, the error event or the job reports an unknown model.",
            "In offline mode a new definition is still registered, but nothing is downloaded.",
            "### Register and Install a Model\n\n"
            "Registering writes the model to `user_models.json` in the Lemonade config directory "
            "(`~/.config/lemonade` by default), after which [`GET /v1/models`](./openai.md#get-v1models) lists it beside the "
            "built-in models. The definition needs a `main` checkpoint, given as `checkpoint` or "
            "as the `main` key of `checkpoints`.",
            "### Register an Omni Collection\n\n"
            "An omni collection bundles several models into one entry that is loaded and pulled as "
            "a unit. Register it with `recipe: \"collection.omni\"` and `components` "
            "instead of a checkpoint. The same call pulls each component.",
            "### Import an Exported Model File\n\n"
            "Files written by `lemonade export` and the desktop app's Export button are `/v1/pull` "
            "request bodies: POST a file's contents as is to register and install its model. See "
            "[Share a collection](../guide/configuration/custom-models.md#share-a-collection-between-machines) "
            "for the file format and workflows.",
        };
        s.args = {
            common::kStream, downloads::kSubscribe, kModelName, kRecipe, kCheckpoint, kCheckpoints,
            kReasoning, kVision, kEmbedding, kReranking, kMmproj, kComponents, kModels, kLabels,
            kSource, kRegistrySource, kDoNotUpgrade, kLocalImport
        };

        RouteResponse job;
        job.format = ResponseFormat::Json;
        job.schema = pull_schema();

        RouteResponse events;
        events.format = ResponseFormat::EventStream;
        events.schema = DownloadManager::event_schema();

        s.responses = {job, events};

        RouteExample job_example;
        job_example.name = "job";
        job_example.format = ResponseFormat::Json;
        job_example.args = ordered_json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
            "stream": true,
            "subscribe": false
        })");

        RouteExample events_example;
        events_example.name = "streaming";
        events_example.format = ResponseFormat::EventStream;
        events_example.args = ordered_json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
            "stream": true
        })");

        RouteExample success_example;
        success_example.name = "non-streaming";
        success_example.format = ResponseFormat::Json;
        success_example.args = ordered_json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF"
        })");

        RouteExample register_example;
        register_example.name = "register";
        register_example.description =
            "Registers `user.Qwen3-0.6B-Q4` from a Hugging Face checkpoint and downloads it.";
        register_example.format = ResponseFormat::Json;
        register_example.args = ordered_json::parse(R"({
            "model_name": "user.Qwen3-0.6B-Q4",
            "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
            "recipe": "llamacpp"
        })");

        RouteExample collection_example;
        collection_example.name = "collection";
        collection_example.description =
            "Registers `user.MyKit`, an omni collection of three built-in models, and pulls each "
            "of them.";
        collection_example.format = ResponseFormat::Json;
        collection_example.args = ordered_json::parse(R"({
            "model_name": "user.MyKit",
            "recipe": "collection.omni",
            "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
        })");

        s.examples = {job_example, success_example, events_example, register_example,
                      collection_example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kInvalidSource, kInvalidModelDefinition,
                    kLemondOffline, kUnknownModel, kIllegalLabels, kRegistryUnavailable, kPullFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model_name = req.arg(kModelName).get<std::string>();

            std::string checkpoint = req.value(kCheckpoint, "");
            std::string recipe = req.value(kRecipe, "");
            bool do_not_upgrade = req.value(kDoNotUpgrade, false);
            bool stream = req.value(common::kStream, false);
            bool subscribe = req.value(downloads::kSubscribe, true);
            bool local_import = req.value(kLocalImport, false);

            // The registry is resolved once, up front, so download, cache layout and
            // later refresh agree on it.
            try {
                lemon::apply_default_pull_source(req.body, ctx_.config->default_model_source());
            } catch (const std::invalid_argument& e) {
                write_plain_error(res, kInvalidSource, e.what());
                return;
            }
            // The helper may have rewritten a provider URL into an owner/repo id.
            checkpoint = req.value(kCheckpoint, "");

            LOG(INFO, "Server") << "Pulling model: " << model_name << std::endl;
            if (!checkpoint.empty()) {
                LOG(INFO, "Server") << "   checkpoint: " << checkpoint << std::endl;
            }
            if (!recipe.empty()) {
                LOG(INFO, "Server") << "   recipe: " << recipe << std::endl;
            }

            const bool collection_file_import = req.arg(kModels).is_array();

            if (!try_register_model_definition(
                    ctx_,
                    model_name,
                    req.body,
                    /*require_definition=*/false,
                    /*allow_embedded_models=*/true,
                    local_import,
                    res)) {
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

            if (refuse_offline_download(ctx_, res)) {
                return;
            }

            json download_request = collection_file_import ? req.body : json::object();
            for (const RouteArg* field : {&kSource, &kRegistrySource}) {
                if (req.has(*field)) {
                    download_request[field->name] = req.arg(*field);
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
            res.status = kUnknownModel.status;
            json error = {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}};
            res.set_content(error.dump(), "application/json");
        } catch (const lemon::InvalidModelDefinitionError& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
            write_plain_error(res, kIllegalLabels, e.what());
        } catch (const lemon::RegistryRateLimitError& e) {
            LOG(WARNING, "Server") << "Registry unavailable in handle_pull: " << e.what() << std::endl;
            res.status = kRegistryUnavailable.status;
            res.set_header("Retry-After", std::to_string(e.retry_after_seconds()));
            json error = {{"error", e.what()},
                          {"code", lemon::kRegistryUnavailableErrorCode},
                          {"upstream_status_code", e.status_code()}};
            res.set_content(error.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull: " << e.what() << std::endl;
            write_plain_error(res, kPullFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_pull_route(ServerContext& ctx) {
    return std::make_unique<PullRoute>(ctx);
}

} // namespace lemon
