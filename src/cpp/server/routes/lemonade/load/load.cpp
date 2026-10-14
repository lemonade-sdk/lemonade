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
using ordered_json = nlohmann::ordered_json;

ordered_json load_schema() {
    return ordered_json::parse(R"({
        "oneOf": [
            {
                "type": "object",
                "description": "A model with a backend of its own.",
                "required": ["status", "model_name", "checkpoint", "recipe"],
                "properties": {
                    "status": {"const": "success", "description": "Always `success`."},
                    "model_name": {"type": "string", "description": "The loaded model."},
                    "checkpoint": {"type": "string", "description": "The model's main checkpoint."},
                    "recipe": {"type": "string", "description": "The recipe that loaded the model, such as `llamacpp`."}
                }
            },
            {
                "type": "object",
                "description": "A collection, which has no checkpoint of its own.",
                "required": ["status", "model_name", "recipe"],
                "properties": {
                    "status": {"const": "success", "description": "Always `success`."},
                    "model_name": {"type": "string", "description": "The loaded collection."},
                    "recipe": {"enum": ["collection.omni", "collection.router"], "description": "The collection's recipe: `collection.omni` or `collection.router`."}
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
            "Loads a registered model into memory, downloading it first when needed, so it is "
            "ready before the first request.";
        s.notes = {
            "Loading a collection (`recipe: \"collection.omni\"`) loads each component in turn "
            "with the component's own saved options; options in the request are not forwarded. A "
            "`collection.router` model loads nothing until a request routes to a candidate.",
            "Loading a model that is already loaded with the same options does nothing; different "
            "options reload it.",
            "### Option Priority\n\n"
            "Each recipe option comes from the first of these layers that sets it:\n\n"
            "1. The `load` request\n"
            "2. The model's own options: its registry and image defaults, with its saved `recipe_options.json` entry on top\n"
            "3. Server-wide settings in `config.json`, or the environment variables that override "
            "them (see [Server Configuration](../guide/configuration/README.md))\n"
            "4. Lemonade's built-in defaults\n\n"
            "An option sent as `null` skips the saved value for this load without changing "
            "`recipe_options.json`. A `*_args` in the request replaces the model's saved args for "
            "this load.",
            "Every option the model's recipe accepts can be passed; the [Backend "
            "Reference](../dev/backends-reference.md#recipe-options) lists them per recipe.",
            "Load failures answer with the same error object as inference requests: `404` for an "
            "unknown or unsupported model, `409` when pinned models fill every slot, and `500` "
            "when the backend fails to start.",
            "### Saved Options\n\n"
            "`recipe_options.json`, in the Lemonade config directory (`~/.config/lemonade` by "
            "default), holds each model's saved options, keyed by the full model name including "
            "any `user.` or `extra.` prefix:\n\n"
            "```json\n"
            "{\n"
            "  \"user.Qwen2.5-Coder-1.5B-Instruct\": {\n"
            "    \"ctx_size\": 16384,\n"
            "    \"llamacpp_backend\": \"vulkan\",\n"
            "    \"llamacpp_args\": \"-np 2 -kvu\"\n"
            "  },\n"
            "  \"whisper-large-v3-turbo-q8_0.bin\": {\n"
            "    \"whispercpp_backend\": \"npu\",\n"
            "    \"whispercpp_args\": \"--convert\"\n"
            "  }\n"
            "}\n"
            "```",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "[Lemonade Server model name](https://lemonade-server.ai/models.html) to load."},
            {"pinned", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If `true`, pins the model so least-recently-used eviction skips it; `false` unpins it. "
             "When omitted, a loaded model keeps its pin state and a new load uses the model's "
             "saved `pinned` option, else `false`."},
            {"save_options", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If `true`, saves the options this load uses as the model's `recipe_options.json` "
             "entry, replacing the stored one; an option sent as `null` keeps its stored value. To "
             "save options without loading, or to change one option without resending the rest, "
             "use [`POST /v1/models/{id}/options`](openai.md#post-v1modelsidoptions). Defaults to "
             "`false`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`llamacpp`, `flm`, `ryzenai-llm`: context size, or `-1` to size it automatically."},
            {"llamacpp_backend", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`llamacpp`: the llama.cpp backend: `auto`, `vulkan`, `rocm`, `cuda`, `metal`, `cpu`, "
             "or `system` for a `llama-server` already installed where supported."},
            {"llamacpp_args", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`llamacpp`: extra arguments for `llama-server`. Flags Lemonade sets itself are rejected, "
             "with their aliases: `-m`, `--port`, `--ctx-size`, `--jinja`, `--device`, `--mmproj`, "
             "`--model-draft`, `--embeddings` and `--reranking`."},
            {"whispercpp_backend", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`whispercpp`: the whisper.cpp backend, such as `cpu`, `npu`, `vulkan`, `rocm` or "
             "`metal`. [`/v1/system-info`](#get-v1system-info) lists the ones this system supports."},
            {"whispercpp_args", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "`whispercpp`: extra arguments for `whisper-server`, such as `--convert`. `-m`, "
             "`--model` and `--port` are rejected."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: inference steps per image. Defaults to the model's own, else `20`."},
            {"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "`sd-cpp`: classifier-free guidance scale. Defaults to the model's own, else `7.0`."},
            {"width", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: image width in pixels. Defaults to the model's own, else `512`."},
            {"height", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "`sd-cpp`: image height in pixels. Defaults to the model's own, else `512`."},
            {"merge_args", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If `true` (the default), server-wide `*_args` still apply when the request sends "
             "`*_args`. If `false`, the request's `*_args` replace every server-wide argument "
             "instead of adding to them."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = load_schema();
        s.responses = {response};

        RouteExample example;
        example.name = "model";
        example.format = ResponseFormat::Json;
        example.args = {{"model_name", "Qwen3-0.6B-GGUF"}};

        RouteExample collection_example;
        collection_example.name = "collection";
        collection_example.format = ResponseFormat::Json;
        collection_example.args = {{"model_name", "LMX-Omni-5.5B-Lite"}};
        s.examples = {example, collection_example};
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

            // Cloud models are registered at cache build, cloud auth and install, so an
            // unknown cloud model here is as missing as any other.
            if (!model_manager.model_exists(model_name)) {
                LOG(ERROR, "Server") << "Model not found: " << model_name << std::endl;
                ctx_.model_loader->write_load_error(res, model_name, "Model not found");
                return;
            }

            auto info = model_manager.get_model_info(model_name);

            // Omitted options keep saved values; a null masks its saved key for this
            // load only.
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

            // Saving keeps the stored value of a nulled key.
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

            // The nulled keys leave this load's options, not recipe_options.json.
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
