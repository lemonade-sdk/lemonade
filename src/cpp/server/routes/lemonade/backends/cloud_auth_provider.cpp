#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class CloudAuthProviderRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.cloud_auth_provider";
        s.methods = {"DELETE"};
        s.paths = {"cloud/auth/{provider}"};
        s.summary = "Clear the in-memory API key for a cloud provider";
        s.experimental = true;
        s.description =
            "Clear the in-memory API key for a provider. Any env-var-based key "
            "(`LEMONADE_<PROVIDER>_API_KEY`) remains in effect. If no env-var key is set, the "
            "provider's discovered models are evicted from the catalog since they are no longer "
            "authenticatable.";
        s.args = {
            {"provider", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "Installed provider name."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["provider", "cleared_runtime_key", "auth_state"],
            "properties": {
                "provider": {"type": "string"},
                "cleared_runtime_key": {"type": "boolean", "description": "`false` when no in-memory key was present, for example when the only key came from the env var."},
                "auth_state": {
                    "type": "object",
                    "properties": {
                        "env_var_set": {"type": "boolean"},
                        "runtime_key_set": {"type": "boolean"}
                    }
                }
            }
        })");
        response.setup = {{"lemonade.cloud_auth", ResponseFormat::Json}};
        response.example = {{"provider", "fireworks"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            const std::string provider = req.http.matches[1].str();
            if (provider.empty()) {
                write_openai_error(res, 400, "Missing provider in URL");
                return;
            }

            const bool cleared = ctx_.cloud_registry->clear_runtime_key(provider);
            // With the env var set, the models stay discovered because it still
            // authenticates them.
            const auto state = ctx_.cloud_registry->auth_state(provider);
            if (!state.env_var_set) {
                ctx_.model_manager->evict_cloud_models(provider);
            }

            json response = {
                {"provider", provider},
                {"cleared_runtime_key", cleared},
                {"auth_state", {
                    {"env_var_set", state.env_var_set},
                    {"runtime_key_set", state.runtime_key_set}
                }}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_cloud_auth_clear: " << e.what() << std::endl;
            write_openai_error(res, 500, e.what(), "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_cloud_auth_provider_route(ServerContext& ctx) {
    return std::make_unique<CloudAuthProviderRoute>(ctx);
}

} // namespace lemon
