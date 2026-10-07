#include <optional>
#include <set>
#include <thread>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json load_schema() {
    return json::parse(R"({
        "oneOf": [
            {
                "type": "object",
                "description": "A model with a backend of its own.",
                "required": ["status", "model_name", "checkpoint", "recipe"],
                "properties": {
                    "status": {"const": "success"},
                    "model_name": {"type": "string"},
                    "checkpoint": {"type": "string"},
                    "recipe": {"type": "string"}
                }
            },
            {
                "type": "object",
                "description": "A collection: an Omni collection loads each component; a router collection loads nothing until a request routes to a candidate.",
                "required": ["status", "model_name", "recipe"],
                "properties": {
                    "status": {"const": "success"},
                    "model_name": {"type": "string"},
                    "recipe": {"enum": ["collection.omni", "collection.router"]}
                },
                "not": {"required": ["checkpoint"]}
            }
        ]
    })");
}

class LoadRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.load";
        s.methods = {"POST"};
        s.paths = {"load"};
        s.summary = "Load a model";
        s.description =
            "Loads a registered model into memory ahead of the requests that use it, downloading "
            "it first if needed.";
        s.notes = {
            "Recipe options have three states. Omitting one keeps the model's saved value. "
            "`null` ignores only that saved value for this load, falling through to the lower "
            "default layers without changing `recipe_options.json`. A concrete value overrides "
            "the saved one; for `*_args` it replaces the model and architecture args for this "
            "load, while backend and machine args remain only when `merge_args` is true. "
            "`ctx_size: -1` is a concrete value meaning automatic sizing, not a tombstone. With "
            "`save_options: true`, concrete values are persisted and a `null` keeps the existing "
            "saved value for its key.",
            "Loading a model that is already loaded with the same options does nothing; "
            "different options reload it.",
            "Loading a collection (`recipe: \"collection.omni\"`) loads each component in turn. "
            "Per-model options such as `ctx_size` or `llamacpp_backend` are not forwarded to "
            "components; set them on each component's own `recipe_options.json` entry instead. "
            "A `collection.router` model loads nothing until a request routes to a candidate.",
            "Every option the model's recipe accepts can be passed; the "
            "[Backend Reference](../dev/backends-reference.md#recipe-options) lists them per "
            "recipe. Load failures answer with the same error object as inference requests: "
            "`404` for an unknown or unsupported model, `409` when pinned models fill every slot, "
            "and `500` when the backend fails to start.",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "[Lemonade model name](https://lemonade-server.ai/models.html) to load."},
            {"pinned", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Pin the model so the LRU never evicts it. Defaults to `false`."},
            {"save_options", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Save this request's recipe options to `recipe_options.json`, replacing the model's "
             "saved values. To save options without loading, or to change one option without "
             "resending the rest, use [`POST /v1/models/{id}/options`](./openai.md#post-v1modelsidoptions)."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`llamacpp`, `flm` and `ryzenai-llm`: context size. `-1` sizes it automatically "
             "instead of using a saved value."},
            {"llamacpp_backend", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`llamacpp`: backend to use, such as `vulkan`, `rocm`, `metal` or `cpu`."},
            {"llamacpp_args", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`llamacpp`: extra llama-server arguments. `-m`, `--port`, `--ctx-size`, `-ngl`, "
             "`--jinja`, `--mmproj`, `--embeddings` and `--reranking` are not allowed."},
            {"whispercpp_backend", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`whispercpp`: `npu` or `cpu` on Windows, `cpu` or `vulkan` on Linux. Defaults to "
             "`npu` where supported."},
            {"whispercpp_args", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`whispercpp`: extra whisper-server arguments, such as `--convert`. `-m`, `--model` "
             "and `--port` are not allowed."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: inference steps for image generation. Defaults to 20."},
            {"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "`sd-cpp`: classifier-free guidance scale. Defaults to 7.0."},
            {"width", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: image width in pixels. Defaults to 512."},
            {"height", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: image height in pixels. Defaults to 512."},
            {"merge_args", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` (default) inherits backend and machine `*_args`; a request's `*_args` then "
             "replace only the model and architecture args. `false` applies no inherited custom "
             "args or overridable runtime defaults."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = load_schema();
        // The llama.cpp slot examples use this load as their setup, and llama-server
        // answers slot actions only when started with a slot save path.
        response.example = {{"model_name", "Qwen3-0.6B-GGUF"},
                            {"llamacpp_args", "--slot-save-path ."}};
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        auto thread_id = std::this_thread::get_id();
        LOG(DEBUG, "Server") << "===== LOAD ENDPOINT ENTERED (Thread: " << thread_id << ") =====" << std::endl;

        ModelManager& model_manager = *ctx_.model_manager;
        // Declared outside the try so the catch can tell a load failure from a bad request.
        std::string model_name;

        json& request_json = req.body;
        resolve_model_name(ctx_, request_json, /*strip_latest=*/false);

        try {
            model_name = request_json["model_name"];

            // Cloud models are registered automatically at cache build / cloud-auth
            // / install time, so a cloud model reaching /load is already in the
            // cache. /load carries no creds payload; CloudServer reads the
            // resolved key (env var or runtime POST) from CloudProviderRegistry
            // when a request is actually forwarded.
            if (!model_manager.model_exists(model_name)) {
                LOG(ERROR, "Server") << "Model not found: " << model_name << std::endl;
                ctx_.model_loader->write_load_error(res, model_name, "Model not found");
                return;
            }

            auto info = model_manager.get_model_info(model_name);

            // Omitted options keep saved values; null masks only its saved key for this
            // load. Concrete *_args scope is resolved later by Router. ctx_size=-1
            // remains an explicit auto value.
            RecipeOptions options = RecipeOptions(info.recipe, request_json);
            std::set<std::string> transient_saved_option_tombstones;
            for (const auto& key : RecipeOptions::keys_for_recipe(info.recipe)) {
                if (request_json.contains(key) && request_json[key].is_null()) {
                    transient_saved_option_tombstones.insert(key);
                }
            }

            bool save_options = request_json.value("save_options", false);
            std::optional<bool> pinned_opt = std::nullopt;
            if (request_json.contains("pinned") && request_json["pinned"].is_boolean()) {
                pinned_opt = request_json["pinned"].get<bool>();
            }

            LOG(INFO, "Server") << "Ensuring model loaded: " << model_name;
            LOG(INFO, "Server") << " " << options.to_log_string(false);
            LOG(INFO, "Server") << std::endl;

            // A null tombstone is load-scoped, so saving keeps the existing saved
            // value for that key.
            if (save_options) {
                json saved_for_write = options.to_json();
                if (!transient_saved_option_tombstones.empty()) {
                    const json existing_saved = model_manager.get_saved_model_options(model_name);
                    for (const auto& key : transient_saved_option_tombstones) {
                        auto it = existing_saved.find(key);
                        if (it != existing_saved.end()) {
                            saved_for_write[key] = *it;
                        }
                    }
                }
                model_manager.set_saved_model_options(model_name, saved_for_write);
                info = model_manager.get_model_info(model_name);
            }

            if (!model_manager.is_model_downloaded(model_name) && !is_model_collection_recipe(info.recipe)) {
                LOG(INFO, "Server") << "Model not downloaded, downloading..." << std::endl;
                model_manager.download_registered_model(info);
                info = model_manager.get_model_info(model_name);
            }

            // Null tombstones are load-local: remove only those saved keys from the
            // local model layer without changing recipe_options.json.
            if (!transient_saved_option_tombstones.empty()) {
                json per_model_options = model_manager.get_model_default_options(info).to_json();
                const json saved = model_manager.get_saved_model_options(model_name);
                if (saved.is_object()) {
                    for (const auto& [key, value] : saved.items()) {
                        if (transient_saved_option_tombstones.count(key) == 0) {
                            per_model_options[key] = value;
                        }
                    }
                }
                info.recipe_options = RecipeOptions(info.recipe, per_model_options);
            }

            if (is_omni_collection_recipe(info.recipe) && !info.components.empty()) {
                ctx_.model_loader->ensure_collection_loaded(info);

                json response = {
                    {"status", "success"},
                    {"model_name", model_name},
                    {"recipe", info.recipe}
                };
                res.set_content(response.dump(), "application/json");
            } else if (is_router_collection_recipe(info.recipe)) {
                // Router collections are virtual: each request is dispatched to one
                // candidate at request time, which lazy-loads it. Eagerly loading every
                // candidate would thrash the model LRU, so acknowledge without loading.
                json response = {
                    {"status", "success"},
                    {"model_name", model_name},
                    {"recipe", info.recipe}
                };
                res.set_content(response.dump(), "application/json");
            } else {
                // Declarative: a no-op when already loaded with matching options; a
                // reload only when the options differ.
                ctx_.router->load_model(model_name, info, options, true,
                                        /*allow_reload_on_option_change=*/true,
                                        pinned_opt);

                json response = {
                    {"status", "success"},
                    {"model_name", model_name},
                    {"checkpoint", info.checkpoint()},
                    {"recipe", info.recipe}
                };
                res.set_content(response.dump(), "application/json");
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;

            if (!model_name.empty()) {
                ctx_.model_loader->write_load_error(res, model_name, e.what());
            } else {
                write_openai_error(res, 400, std::string("Invalid request: ") + e.what(),
                                   "invalid_request_error", "invalid_request");
            }
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_load_route(ServerContext& ctx) {
    return std::make_unique<LoadRoute>(ctx);
}

} // namespace lemon
