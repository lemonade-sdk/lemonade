#include "lemon/server/model_options.h"

#include <optional>

#include <lemon/utils/aixlog.hpp>

#include "lemon/alias_manager.h"
#include "lemon/auto_tune.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {

using json = nlohmann::json;

namespace {

// Fill in every option the recipe accepts, resolving unset keys through the
// default chain, so a client can render a complete form from one response.
// `pinned` is live-process state that /v1/load and /internal/pin own, so it is left out.
json resolve_all_recipe_options(const RecipeOptions& options) {
    json resolved = options.to_resolved_json();
    resolved.erase("pinned");
    return resolved;
}

} // namespace

json model_options_schema() {
    return json::parse(R"({
        "type": "object",
        "required": ["model_name", "recipe", "saved", "effective", "defaults", "resolved_ctx_size"],
        "properties": {
            "model_name": {"type": "string", "description": "The id from the URL. It appears again inside effective and defaults, so each is a complete /v1/load body."},
            "recipe": {"type": "string", "description": "The recipe the option names belong to."},
            "saved": {"type": "object", "description": "The model's own entry in recipe_options.json: only what was explicitly saved, or {} when nothing is. It can hold keys this endpoint does not accept, such as pinned written by /v1/load, so replay effective rather than saved."},
            "effective": {"type": "object", "description": "The exact /v1/load body for this model right now, with every option the recipe accepts resolved through the priority chain. Posting it back whole saves every resolved value as an override, so send only the options the user changed."},
            "defaults": {"type": "object", "description": "What effective becomes if saved is erased, in the same shape. A ctx_size of -1 means the server picks the context size automatically."},
            "resolved_ctx_size": {"type": "integer", "description": "The context size a load right now would use: the effective ctx_size, or the automatically computed size when that is -1."}
        }
    })");
}

void respond_with_model_options(ServerContext& ctx, const httplib::Request& http,
                                httplib::Response& res, const ModelOptionsMutation& mutation) {
    ModelManager& model_manager = *ctx.model_manager;
    const std::string model_id = utils::normalize_model_name(http.matches[1]);
    std::string model_key = model_id;

    if (!model_manager.model_exists(model_key)) {
        std::optional<std::string> target;
        if (ctx.alias_manager->has_alias(model_id)) {
            target = ctx.alias_manager->resolve_alias(model_id);
        }
        std::string canonical_target = target ? model_manager.resolve_model_name(*target) : "";
        if (target && model_manager.model_exists(canonical_target)) {
            model_key = canonical_target;
        } else if (target && model_manager.model_exists(*target)) {
            model_key = *target;
        } else {
            ctx.model_loader->write_load_error(res, model_id, "Model not found");
            return;
        }
    }

    // Work in canonical names from here on: a user model's requested id is its
    // bare public name, which not every downstream lookup resolves for itself.
    model_key = model_manager.resolve_model_name(model_key);

    try {
        ModelInfo info = model_manager.get_model_info(model_key);
        if (mutation && !mutation(model_key, info, res)) return;

        const RecipeOptions no_request_options(info.recipe, json::object());
        RecipeOptions effective = ctx.router->resolve_effective_options(info, no_request_options);

        ModelInfo without_saved = info;
        without_saved.recipe_options = model_manager.get_model_default_options(info);
        RecipeOptions defaults = ctx.router->resolve_effective_options(without_saved, no_request_options);

        // `effective` and `defaults` double as replayable /v1/load bodies.
        json effective_json = resolve_all_recipe_options(effective);
        json defaults_json = resolve_all_recipe_options(defaults);
        effective_json["model_name"] = model_id;
        defaults_json["model_name"] = model_id;

        const int64_t auto_ctx = resolve_auto_ctx_size(effective, info);
        const json effective_ctx = effective.get_option("ctx_size");
        const int64_t resolved_ctx = auto_ctx != -2 ? auto_ctx
            : (effective_ctx.is_number() ? effective_ctx.get<int64_t>() : -1);

        json response = {
            {"model_name", model_id},
            {"recipe", info.recipe},
            {"saved", model_manager.get_saved_model_options(model_key)},
            {"effective", effective_json},
            {"defaults", defaults_json},
            {"resolved_ctx_size", resolved_ctx}
        };
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to handle options for '" << model_id
                             << "': " << e.what() << std::endl;
        if (!model_manager.model_exists(model_key)) {
            // The model went away between the existence check and the read.
            ctx.model_loader->write_load_error(res, model_key, e.what());
            return;
        }
        // Anything else is a server-side failure, most often the options file
        // being unwritable. Reporting it as a model error would call it a load
        // failure, which this endpoint never performs.
        write_openai_error(res, 500,
                           "Failed to read or update options for '" + model_id + "': " + e.what(),
                           "server_error", "internal_error");
    }
}

} // namespace lemon
