#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kAuthConflict{
    409, "",
    "`LEMONADE_<PROVIDER>_API_KEY` is set and takes precedence, so the key is not stored; "
    "`error.type` is `auth_conflict` and `error.env_var` names the variable"};

const RouteArg kProvider{"provider", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                         "The installed provider's name."};
const RouteArg kApiKey{"api_key", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                       "The API key."};

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
            "Sets an API key for an installed cloud provider and refreshes the provider's "
            "discovered models. The key is held in `lemond`'s memory only, never written to disk, "
            "and is gone after a restart; for a key that persists, set "
            "`LEMONADE_<PROVIDER>_API_KEY` in `lemond`'s environment instead.";
        s.args = {kProvider, kApiKey, backend_routes::kAllowInsecureHttp};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["provider", "allow_insecure_http", "auth_state", "models_discovered"],
            "properties": {
                "provider": {"type": "string", "description": "The provider the key was set for."},
                "allow_insecure_http": {"type": "boolean", "description": "Whether the provider may receive keys over `http://`, after this request."},
                "auth_state": {
                    "type": "object",
                    "description": "Which API keys are set for the provider.",
                    "properties": {
                        "env_var_set": {"type": "boolean", "description": "Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`."},
                        "runtime_key_set": {"type": "boolean", "description": "Whether a key is held in `lemond`'s memory."}
                    }
                },
                "models_discovered": {"type": "integer", "description": "Chat models discovered with the key; `0` when discovery fails."},
                "warnings": {"type": "array", "items": {"type": "string"}, "description": "Present when the provider's base URL is `http://`."},
                "warning": {"type": "string", "description": "The warnings joined into one string, for older clients."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.install", "cloud"}};
        example.args = ordered_json::parse(R"({
            "provider": "fireworks",
            "api_key": "fw-XXXXX"
        })");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid,
                    kProviderNotInstalled, kInsecureHttpError, kAuthConflict, kUnexpectedException};
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        CloudProviderRegistry& registry = *ctx_.cloud_registry;
        try {
            const auto provider = req.arg(kProvider).get<std::string>();
            const auto api_key = req.arg(kApiKey).get<std::string>();
            const bool allow_insecure_http = req.value(backend_routes::kAllowInsecureHttp, false);

            if (!registry.is_installed(provider)) {
                write_provider_not_installed(res, provider);
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
                res.status = kAuthConflict.status;
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
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_cloud_auth_set: " << e.what() << std::endl;
            write_openai_error(res, kUnexpectedException, e.what(), "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_cloud_auth_route(ServerContext& ctx) {
    return std::make_unique<CloudAuthRoute>(ctx);
}

} // namespace lemon
