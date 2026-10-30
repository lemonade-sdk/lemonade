#include <string>

#include <lemon/utils/aixlog.hpp>

#include "local_backend.h"
#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/server/shared_args.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kMissingProvider{400, "", "A cloud uninstall has no `provider`, or an empty one"};

const RouteArg kProvider{"provider", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                         "The installed cloud provider to remove."};


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
            "Removes a recipe's backend, unloading the models that use it first, or a cloud "
            "provider when `backend` is `\"cloud\"`.";
        s.notes = {
            "### Uninstall a Cloud Provider\n"
            "<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>\n"
            "\n"
            "Removes the provider record from `config.json`, drops its in-memory API key, if any, "
            "and evicts every discovered model for that provider from the catalog.",
        };
        s.args = {backend_routes::kRecipe, backend_routes::kBackend, kProvider};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({"oneOf": [
            {
                "type": "object",
                "title": "Local backend",
                "description": "A local backend removed.",
                "required": ["status", "recipe", "backend"],
                "properties": {
                    "status": {"const": "success", "description": "Always `success`."},
                    "recipe": {"type": "string", "description": "The recipe from the request."},
                    "backend": {"type": "string", "description": "The backend from the request."}
                }
            },
            {
                "type": "object",
                "title": "Cloud provider",
                "description": "A cloud provider removed.",
                "required": ["status", "backend", "provider", "models_evicted"],
                "properties": {
                    "status": {"const": "success", "description": "Always `success`."},
                    "backend": {"const": "cloud", "description": "Always `cloud`."},
                    "provider": {"type": "string", "description": "The provider that was removed."},
                    "models_evicted": {"type": "integer", "description": "Discovered models removed from the catalog."}
                }
            }
        ]})");
        s.responses = {response};

        RouteExample local_example;
        local_example.name = "local";
        local_example.format = ResponseFormat::Json;
        local_example.setup = {{"lemonade.install", "local"}};
        local_example.args = ordered_json::parse(R"({
            "recipe": "llamacpp",
            "backend": "cpu"
        })");

        RouteExample cloud_example;
        cloud_example.name = "cloud";
        cloud_example.format = ResponseFormat::Json;
        cloud_example.setup = {{"lemonade.install", "cloud"}};
        cloud_example.args = ordered_json::parse(R"({
            "backend": "cloud",
            "provider": "fireworks"
        })");
        s.examples = {local_example, cloud_example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kMissingProvider,
                    kProviderNotInstalled, kMissingRecipe, kUnexpectedException};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            Router& router = *ctx_.router;

            if (req.value(backend_routes::kBackend, "") == "cloud") {
                const std::string provider = req.value(kProvider, "");
                if (provider.empty()) {
                    write_openai_error(res, kMissingProvider, "Cloud uninstall requires 'provider' string field");
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
                    write_provider_not_installed(res, provider);
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

            const std::string recipe = req.value(backend_routes::kRecipe, "");
            const std::string backend = req.value(backend_routes::kBackend, "");
            if (!require_local_recipe(recipe, res)) {
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
            write_plain_error(res, kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_uninstall_route(ServerContext& ctx) {
    return std::make_unique<UninstallRoute>(ctx);
}

} // namespace lemon
