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
using ordered_json = nlohmann::ordered_json;

ordered_json pull_schema() {
    ordered_json success = ordered_json::parse(R"({
        "type": "object",
        "required": ["status", "model_name"],
        "properties": {
            "status": {"const": "success"},
            "model_name": {"type": "string"},
            "message": {"type": "string", "description": "Only for local_import."}
        }
    })");
    ordered_json job = DownloadManager::job_schema();
    job["description"] = "With stream=true and subscribe=false: the server-owned download job.";
    return {{"oneOf", ordered_json::array({success, job})}};
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
            "Register and install models for use with Lemonade Server.\n\n"
            "The Lemonade Server built-in model registry has a collection of model names that can "
            "be pulled and loaded. The `pull` endpoint can install any registered model, and it "
            "can also register-then-install any model available on Hugging Face.";
        s.notes = {
            "**Install a Model that is Already Registered**\n\n"
            "Example request:\n\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "    \"model_name\": \"Qwen3-0.6B-GGUF\"\n"
            "  }'\n"
            "```\n\n"
            "Response format:\n\n"
            "```json\n"
            "{\n"
            "  \"status\":\"success\",\n"
            "  \"model_name\":\"Qwen3-0.6B-GGUF\"\n"
            "}\n"
            "```\n\n"
            "In case of an error, the `error` field will contain the error message.",
            "**Register and Install a Model**\n\n"
            "Registration will place an entry for that model in the `user_models.json` file, "
            "which is located in the user's Lemonade config directory (default: "
            "`~/.config/lemonade`). Then, the model will be installed. Once the model is "
            "registered and installed, it will show up in the `models` endpoint alongside the "
            "built-in models and can be loaded.\n\n"
            "The `recipe` field defines which software framework and device will be used to load "
            "and run the model.\n\n"
            "> Note: the `model_name` for registering a new model must use the `user` namespace, "
            "to prevent collisions with built-in models. For example, `user.Phi-4-Mini-GGUF`.\n\n"
            "A model definition requires at least a `main` checkpoint. This can be either\n"
            "be specified with the `checkpoint` parameter, or a `main` key in the\n"
            "`checkpoints` dict.\n\n"
            "Each backend serves a fixed set of [deployment modes](openai.md#model-labels),\n"
            "and a model deploys in exactly one of them. Naming a mode the recipe cannot\n"
            "serve, or naming two — whether through `labels` or through the `embedding` /\n"
            "`reranking` parameters — is rejected with `400` and nothing is registered:\n\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/api/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\"model_name\": \"user.Clf\", \"recipe\": \"llamacpp\",\n"
            "       \"checkpoint\": \"example/model:Q4_K_M\", \"labels\": [\"classification\"]}'\n"
            "```\n\n"
            "```json\n"
            "{\"error\": \"Model 'user.Clf': recipe 'llamacpp' cannot serve 'classification'. It "
            "serves 'chat', 'embeddings', 'reranking'. Omit the label to deploy as 'chat'.\"}\n"
            "```\n\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/api/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\"model_name\": \"user.Both\", \"recipe\": \"llamacpp\",\n"
            "       \"checkpoint\": \"example/model:Q4_K_M\", \"labels\": [\"chat\", "
            "\"embeddings\"]}'\n"
            "```\n\n"
            "```json\n"
            "{\"error\": \"Model 'user.Both': a model deploys in exactly one mode, but these "
            "labels name two: 'chat' and 'embeddings'. Register one model per mode.\"}\n"
            "```\n\n"
            "Omitting the deployment label entirely is always valid — the recipe's default is\n"
            "applied.\n\n"
            "Other checkpoint types may also be specified depending on the model type.\n"
            "This list is not exhaustive, and may change or grow over time as models\n"
            "and backends evolve:\n"
            "* `mmproj` - used by vision models, if not already embedded in `main`\n"
            "* `draft` - used by dflash, eagle, and multitoken-prediction, if not already "
            "embedded in `main`\n"
            "* `text_encoder` - text-to-token encoder used by image generation\n"
            "* `vae` - variational autoencoder used by image generation\n\n"
            "Example request:\n\n"
            "```bash\n"
            "# Single checkpoint\n"
            "curl -X POST http://localhost:13305/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "    \"model_name\": \"user.Phi-4-Mini-GGUF\",\n"
            "    \"checkpoint\": \"unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M\",\n"
            "    \"recipe\": \"llamacpp\"\n"
            "  }'\n"
            "```\n\n"
            "Instead of defining a model by `checkpoint` and `mmproj`, a model can also\n"
            "be defined with a dict of checkpoint types and paths. These requests do the\n"
            "same thing, but the syntax for pulling the mmproj differs.\n\n"
            "```bash\n"
            "# Multi-checkpoint\n"
            "curl -X POST http://localhost:13305/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "    \"model_name\": \"user.My-Gemma3\",\n"
            "    \"checkpoint\": \"ggml-org/gemma-3-4b-it-GGUF:Q4_K_M\",\n"
            "    \"mmproj\": \"mmproj-model-f16.gguf\",\n"
            "\t\"vision\": true,\n"
            "    \"recipe\": \"llamacpp\"\n"
            "  }'\n"
            "```\n\n"
            "```bash\n"
            "# Multi-checkpoint\n"
            "curl -X POST http://localhost:13305/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "    \"model_name\": \"user.My-Gemma3\",\n"
            "    \"checkpoints\": {\n"
            "        \"main\": \"ggml-org/gemma-3-4b-it-GGUF:Q4_K_M\",\n"
            "        \"mmproj\": \"ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf\"\n"
            "    },\n"
            "\t\"vision\": true,\n"
            "    \"recipe\": \"llamacpp\"\n"
            "  }'\n"
            "```\n\n"
            "Response format:\n\n"
            "```json\n"
            "{\n"
            "  \"status\":\"success\",\n"
            "  \"model_name\":\"user.Phi-4-Mini-GGUF\"\n"
            "}\n"
            "```\n\n"
            "In case of an error, the `error` field will contain the error message.",
            "**Register an Omni-Model**\n\n"
            "An omni collection is a collection type that bundles several models into a single "
            "entry that can be loaded, pulled, or deleted as a unit. Use `recipe: "
            "\"collection.omni\"` with a `components` array instead of `checkpoint`.\n\n"
            "Components do not need to be downloaded already — any not-yet-downloaded components "
            "are pulled by the same call. Deleting the collection removes only the collection "
            "entry; components stay on disk.\n\n"
            "Example request:\n\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/v1/pull \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "    \"model_name\": \"user.MyKit\",\n"
            "    \"recipe\": \"collection.omni\",\n"
            "    \"components\": [\"Qwen3-0.6B-GGUF\", \"Whisper-Tiny\", \"SD-Turbo\"]\n"
            "  }'\n"
            "```",
            "Only this endpoint checks a downloaded model's registry for updates, unless "
            "`do_not_upgrade` is `true`; loading a model on first use never does.",
            "A `400` answers an invalid definition, an unknown model (with `code: "
            "\"unknown_model\"`), or offline mode (with `code: \"lemond_offline\"`), and nothing "
            "is registered.",
            "### Import an Exported Model File\n\n"
            "Files written by `lemonade export` (and the desktop app's Export button) are "
            "import-ready\n"
            "`/v1/pull` request bodies — POST the file contents verbatim to register and install "
            "the model.\n"
            "This works for regular models and collections alike; exported collection files "
            "additionally\n"
            "carry `components` plus a `models` array embedding each component's definition (see "
            "the\n"
            "`models` parameter above). For the file format and the export/import/Hugging Face "
            "workflows,\n"
            "see [Share a "
            "collection](../guide/configuration/custom-models.md#share-a-collection-between-machines).",
        };
        s.args = {
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If `true`, returns Server-Sent Events (SSE) with download progress. Defaults to "
             "`false`."},
            {"subscribe", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Only applies when `stream=true`. If `false`, the server starts a background model "
             "download job and returns a JSON snapshot immediately instead of keeping the HTTP "
             "response subscribed to SSE progress. Defaults to `true` for backwards compatibility."},
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Already registered: [Lemonade Server model name](https://lemonade-server.ai/models.html) "
             "to install. Register and install: Namespaced [Lemonade Server model "
             "name](https://lemonade-server.ai/models.html) to register and install. Omni-model: "
             "Namespaced model name, e.g. `user.MyKit`. `model` is accepted as an alias."},
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Register and install, required: Lemonade API recipe to load the model with. "
             "Omni-model, required: Must be `\"collection.omni\"`."},
            {"checkpoint", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "HuggingFace \"main\" checkpoint to install. A Hugging Face or ModelScope URL is "
             "normalized to `owner/repo` and selects its registry."},
            {"checkpoints", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "HuggingFace checkpoints to install, for multi-checkpoint models."},
            {"reasoning", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Whether the model is a reasoning model, like DeepSeek (default: false). Adds "
             "'reasoning' label."},
            {"vision", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Whether the model has vision capabilities for processing images (default: false). "
             "Adds 'vision' label."},
            {"embedding", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Whether the model is an embedding model (default: false). Adds 'embeddings' label."},
            {"reranking", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Whether the model is a reranking model (default: false). Adds 'reranking' label."},
            {"mmproj", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Multimodal Projector (mmproj) file to use for vision models."},
            {"components", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Omni-model, required: Ordered, non-empty array of model names. Each component must "
             "be a regular model."},
            {"models", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Ordered array of full model definitions, one per `components` entry (the same fields "
             "as single-model registration, keyed by `model_name`). When present, component names "
             "that are not yet registered are registered from these definitions; names that "
             "already exist keep their local definition. When absent, every `components` entry "
             "must already exist in the registry (built-in or a previously registered `user.*` "
             "model)."},
            {"labels", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Registering only: model labels. A model deploys in exactly one "
             "[deployment mode](./openai.md#model-labels): naming a mode the recipe cannot "
             "serve, or naming two, is rejected with `400`. Omitting the deployment label "
             "applies the recipe's default."},
            {"source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registry to download from: `huggingface` or `modelscope`. Defaults to the server's "
             "`default_model_source`."},
            {"registry_source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Same as `source`; when both are given they must name the same registry."},
            {"do_not_upgrade", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` skips checking an already-downloaded model's registry for updates. Defaults "
             "to `false`."},
            {"local_import", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Register model files a client already copied into the Hugging Face cache, without "
             "downloading. Defaults to `false`."},
        };

        RouteResponse job;
        job.format = ResponseFormat::Json;
        job.schema = pull_schema();
        job.example = ordered_json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
            "stream": true,
            "subscribe": false
        })");

        RouteResponse events;
        events.format = ResponseFormat::EventStream;
        events.schema = DownloadManager::event_schema();
        events.example = ordered_json::parse(R"({
            "model_name": "Qwen3-0.6B-GGUF",
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

            // The registry is resolved once, up front, so download, cache layout and
            // later refresh agree on it.
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
