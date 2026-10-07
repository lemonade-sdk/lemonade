#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class CloudAuthRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.cloud_auth";
        s.methods = {"POST"};
        s.paths = {"cloud/auth"};
        s.summary = "Set an in-memory API key for a cloud provider";
        s.experimental = true;
        s.description =
            "Stores an API key for an installed cloud provider in lemond's memory and refreshes "
            "the provider's discovered models.";
        s.notes = {
            "The key is never written to disk and is cleared when lemond restarts; for a key "
            "that persists, set `LEMONADE_<PROVIDER>_API_KEY` in lemond's environment instead.",
            "**Precedence:** when `LEMONADE_<PROVIDER>_API_KEY` is set, it wins: the supplied key "
            "is not stored and the answer is `409` with `{\"error\": {\"type\": "
            "\"auth_conflict\", \"env_var\": \"LEMONADE_<PROVIDER>_API_KEY\", \"message\": "
            "...}}`. An operator can provision a house key this way without a client overriding "
            "it.",
            "A missing or empty `provider` or `api_key` is answered with `400`, as is an "
            "`http://` provider without `allow_insecure_http` (code "
            "`insecure_http_requires_opt_in`). A provider that is not installed is answered with "
            "`404`; install it with [`POST /v1/install`](#post-v1install) and `backend: \"cloud\"` "
            "first.",
        };
        s.args = {
            {"provider", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Installed provider name."},
            {"api_key", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "API key to hold in lemond's memory."},
            {"allow_insecure_http", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Opt in to sending the key to a provider whose base URL is `http://`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["provider", "allow_insecure_http", "auth_state", "models_discovered"],
            "properties": {
                "provider": {"type": "string"},
                "allow_insecure_http": {"type": "boolean"},
                "auth_state": {
                    "type": "object",
                    "properties": {
                        "env_var_set": {"type": "boolean"},
                        "runtime_key_set": {"type": "boolean"}
                    }
                },
                "models_discovered": {"type": "integer", "description": "Chat models discovered with the key; 0 when discovery fails."},
                "warnings": {"type": "array", "items": {"type": "string"}},
                "warning": {"type": "string", "description": "The warnings joined into one string, for older clients."}
            }
        })");
        response.setup = {{"lemonade.install", ResponseFormat::Json}};
        response.example = json::parse(R"({
            "provider": "example",
            "api_key": "example-key"
        })");
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        CloudProviderRegistry& registry = *ctx_.cloud_registry;
        try {
            const auto body = json::parse(req.http.body);
            if (!body.contains("provider") || !body["provider"].is_string() ||
                !body.contains("api_key") || !body["api_key"].is_string()) {
                write_openai_error(res, 400, "Body must contain string fields: provider, api_key");
                return;
            }
            const auto provider = body["provider"].get<std::string>();
            const auto api_key = body["api_key"].get<std::string>();
            bool allow_insecure_http = false;
            if (body.contains("allow_insecure_http")) {
                if (!body["allow_insecure_http"].is_boolean()) {
                    write_openai_error(res, 400, "allow_insecure_http must be a boolean when provided");
                    return;
                }
                allow_insecure_http = body["allow_insecure_http"].get<bool>();
            }
            if (provider.empty() || api_key.empty()) {
                write_openai_error(res, 400, "provider and api_key must be non-empty");
                return;
            }

            if (!registry.is_installed(provider)) {
                write_openai_error(res, 404,
                    "Cloud provider '" + provider + "' is not installed. "
                    "Call POST /v1/install with backend=cloud, provider, "
                    "and base_url first.");
                return;
            }

            const std::string base_url = registry.base_url_for(provider);
            if (CloudProviderRegistry::is_http_base_url(base_url)) {
                const bool already_allowed = registry.allow_insecure_http_for(provider);
                if (!allow_insecure_http && !already_allowed) {
                    write_insecure_http_error(res, provider);
                    return;
                }
                if (allow_insecure_http && !already_allowed) {
                    registry.set_allow_insecure_http(provider, true);
                    persist_cloud_providers(ctx_);
                }
            }

            // The environment variable wins over a runtime key, so refusing with 409
            // tells the caller its POST had no effect.
            if (!registry.set_runtime_key(provider, api_key)) {
                res.status = 409;
                const auto env_name = CloudProviderRegistry::env_var_name(provider);
                json error = {{"error", {
                    {"message", env_name + " is set in the lemond process; the env var "
                                "takes precedence and the supplied API key was not stored."},
                    {"type", "auth_conflict"},
                    {"env_var", env_name}}}};
                res.set_content(error.dump(), "application/json");
                return;
            }

            // Best-effort: a failed refresh logs and counts 0, and the key stays stored.
            size_t models_after = ctx_.model_manager->refresh_cloud_models(provider);

            const auto state = registry.auth_state(provider);
            json response = {
                {"provider", provider},
                {"allow_insecure_http", registry.allow_insecure_http_for(provider)},
                {"auth_state", {
                    {"env_var_set", state.env_var_set},
                    {"runtime_key_set", state.runtime_key_set}
                }},
                {"models_discovered", models_after}
            };
            attach_warnings(
                response,
                CloudProviderRegistry::base_url_warnings(base_url, /*api_key_available=*/true));
            res.set_content(response.dump(), "application/json");
        } catch (const json::parse_error& e) {
            write_openai_error(res, 400, "Invalid JSON: " + std::string(e.what()));
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_cloud_auth_set: " << e.what() << std::endl;
            write_openai_error(res, 500, e.what(), "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_cloud_auth_route(ServerContext& ctx) {
    return std::make_unique<CloudAuthRoute>(ctx);
}

} // namespace lemon
