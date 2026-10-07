#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class UninstallRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.uninstall";
        s.methods = {"POST"};
        s.paths = {"uninstall"};
        s.summary = "Remove a backend or cloud provider";
        s.description =
            "Removes the backend for a recipe, or a cloud provider when `backend` is "
            "`\"cloud\"`.";
        s.notes = {
            "**Status:** the cloud-provider branch is experimental.",
            "Loaded models that use the backend are unloaded first.",
            "Removing a cloud provider deletes its record from `config.json`, drops its "
            "in-memory API key, and evicts every model discovered for it. A provider that was "
            "never installed is answered with `404`.",
            "A local uninstall without both `recipe` and `backend`, or a cloud uninstall without "
            "`provider`, is answered with `400`. Any other failure, including invalid JSON, is "
            "answered with `500` and an `error` string.",
        };
        s.args = {
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Local backends, required: recipe name."},
            {"backend", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Backend within the recipe, or `\"cloud\"` to remove a cloud provider."},
            {"provider", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers, required: the installed provider's name."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({"oneOf": [
            {
                "type": "object",
                "description": "A local backend removed.",
                "required": ["status", "recipe", "backend"],
                "properties": {
                    "status": {"const": "success"},
                    "recipe": {"type": "string"},
                    "backend": {"type": "string"}
                }
            },
            {
                "type": "object",
                "description": "A cloud provider removed.",
                "required": ["status", "backend", "provider", "models_evicted"],
                "properties": {
                    "status": {"const": "success"},
                    "backend": {"const": "cloud"},
                    "provider": {"type": "string"},
                    "models_evicted": {"type": "integer", "description": "Discovered models removed from the catalog."}
                }
            }
        ]})");
        response.setup = {{"lemonade.install", ResponseFormat::Json}};
        response.example = json::parse(R"({
            "backend": "cloud",
            "provider": "example"
        })");
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);
            Router& router = *ctx_.router;

            // Unknown providers answer 404, matching the install error shape, so a
            // script can tell the two cases apart.
            if (request_json.value("backend", "") == "cloud") {
                const std::string provider = request_json.value("provider", "");
                if (provider.empty()) {
                    write_openai_error(res, 400, "Cloud uninstall requires 'provider' string field");
                    return;
                }
                // Router::unload_model takes a model name, not a recipe filter, so the
                // provider's loaded models are found by walking the loaded list.
                auto loaded = router.get_all_loaded_models();
                for (const auto& m : loaded) {
                    if (m.value("recipe", "") == "cloud" &&
                        m.value("cloud_provider", "") == provider) {
                        router.unload_model(m.value("model_name", ""));
                    }
                }
                bool removed = ctx_.cloud_registry->uninstall(provider);
                size_t evicted = ctx_.model_manager->evict_cloud_models(provider);
                if (!removed) {
                    write_openai_error(res, 404, "Cloud provider '" + provider + "' is not installed");
                    return;
                }
                persist_cloud_providers(ctx_);
                json response = {
                    {"status", "success"},
                    {"backend", "cloud"},
                    {"provider", provider},
                    {"models_evicted", evicted}
                };
                res.set_content(response.dump(), "application/json");
                return;
            }

            std::string recipe = request_json.value("recipe", "");
            std::string backend = request_json.value("backend", "");

            if (recipe.empty() || backend.empty()) {
                write_plain_error(res, 400, "Both 'recipe' and 'backend' are required");
                return;
            }

            LOG(INFO, "Server") << "Uninstalling backend: " << recipe << ":" << backend << std::endl;

            auto loaded_models = router.get_all_loaded_models();
            std::string backend_option_key = recipe + "_backend";
            for (const auto& model : loaded_models) {
                if (model.value("recipe", "") == recipe) {
                    std::string model_backend;
                    if (model.contains("recipe_options") && model["recipe_options"].contains(backend_option_key)) {
                        model_backend = model["recipe_options"].value(backend_option_key, "");
                    }
                    if (!model_backend.empty() && model_backend != backend) {
                        continue;
                    }
                    std::string model_name = model.value("model_name", "");
                    LOG(INFO, "Server") << "Unloading model " << model_name
                              << " before uninstalling " << recipe << ":" << backend << std::endl;
                    router.unload_model(model_name);
                }
            }

            ctx_.backend_manager->uninstall_backend(recipe, backend);

            SystemInfoCache::invalidate_recipes();
            ctx_.model_manager->invalidate_models_cache();

            json response = {
                {"status", "success"},
                {"recipe", recipe},
                {"backend", backend}
            };
            res.set_content(response.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_uninstall: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_uninstall_route(ServerContext& ctx) {
    return std::make_unique<UninstallRoute>(ctx);
}

} // namespace lemon
