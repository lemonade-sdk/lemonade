#include <optional>
#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "local_backend.h"
#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/server/download_manager.h"
#include "lemon/server/shared_args.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kUnsupportedBackend{
    400, "", "The backend is not supported on this system and `force` is not set"};
const RouteError kMissingCloudFields{
    400, "", "A cloud install is missing `provider` or `base_url`, or one is empty"};
const RouteError kInvalidProviderName{
    400, "", "`provider` is longer than 64 characters or not lowercase `[a-z0-9_-]`"};
const RouteError kInvalidBaseUrl{
    400, "",
    "`base_url` does not start with `https://` or `http://`, has no host, or contains "
    "whitespace"};
const RouteError kInvalidCloudOption{
    400, "",
    "`auth_header_name` is empty, over 128 characters or not a valid HTTP header name, or "
    "`auth_header_prefix` is over 128 characters or contains a control character"};

ordered_json install_schema() {
    auto local = ordered_json::parse(R"({
        "type": "object",
        "title": "Local backend",
        "description": "A local backend installed or already up to date.",
        "required": ["status", "recipe", "backend"],
        "properties": {
            "status": {"const": "success", "description": "Always `success`."},
            "recipe": {"type": "string", "description": "The recipe from the request."},
            "backend": {"type": "string", "description": "The backend from the request."}
        }
    })");
    auto manual = ordered_json::parse(R"({
        "type": "object",
        "title": "Manual setup",
        "description": "A backend that needs manual setup on this system, such as `rocm` on a Strix Halo (`gfx1151`) whose Linux kernel lacks the CWSR fix: nothing was installed.",
        "x-no-example": "Only a Strix Halo running a kernel without the CWSR fix returns it.",
        "required": ["action", "recipe", "backend"],
        "properties": {
            "action": {"type": "string", "description": "The setup guide's URL."},
            "recipe": {"type": "string", "description": "The recipe from the request."},
            "backend": {"type": "string", "description": "The backend from the request."}
        }
    })");
    auto cloud = ordered_json::parse(R"({
        "type": "object",
        "title": "Cloud provider",
        "description": "A cloud provider registered.",
        "required": ["status", "backend", "provider", "base_url", "allow_insecure_http",
                     "auth_header_name", "auth_header_prefix", "wire_format", "models_discovered",
                     "auth_state"],
        "properties": {
            "status": {"const": "success", "description": "Always `success`."},
            "backend": {"const": "cloud", "description": "Always `cloud`."},
            "provider": {"type": "string", "description": "The provider's name, which prefixes its model names."},
            "base_url": {"type": "string", "description": "The provider's saved base URL."},
            "allow_insecure_http": {"type": "boolean", "description": "Whether an API key may be sent to an `http://` base URL."},
            "auth_header_name": {"type": "string", "description": "The header that carries the API key."},
            "auth_header_prefix": {"type": "string", "description": "The text before the key in that header."},
            "wire_format": {"enum": ["openai", "anthropic"], "description": "The API the provider speaks: `openai` or `anthropic`."},
            "models_discovered": {"type": "integer", "description": "Chat models discovered with the resolved API key; `0` when no key resolves."},
            "auth_state": {
                "type": "object",
                "description": "Which API keys are set for the provider.",
                "properties": {
                    "env_var_set": {"type": "boolean", "description": "Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`."},
                    "runtime_key_set": {"type": "boolean", "description": "Whether a key is held in `lemond`'s memory."}
                }
            },
            "warnings": {"type": "array", "items": {"type": "string"}, "description": "Present when the base URL is `http://`, or when the provider's environment variable overrode `api_key`."},
            "warning": {"type": "string", "description": "The warnings joined into one string, for older clients."}
        }
    })");
    auto job = DownloadManager::job_schema();
    job["title"] = "Download job";
    job["description"] = "`stream: true` with `subscribe: false`: the server-owned download job just started.";
    return {{"oneOf", ordered_json::array({local, manual, cloud, job})}};
}

const RouteArg kForce{"force", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                      "Local backends: if `true`, installs a backend this system does not support anyway. "
                      "Defaults to `false`."};
const RouteArg kProvider{"provider", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                         "Cloud providers, which take `backend: \"cloud\"`, required: a short identifier "
                         "matching `[a-z0-9_-]+`, such as `fireworks`, used as the provider's model-name "
                         "prefix."};
const RouteArg kBaseUrl{"base_url", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                        "Cloud providers, required: the provider's base URL, such as one ending in `/v1`."};
const RouteArg kApiKey{"api_key", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Cloud providers: an API key, held in `lemond`'s memory. The provider's environment "
                       "variable takes precedence; see [`POST /v1/cloud/auth`](#post-v1cloudauth)."};
const RouteArg kAuthHeaderName{"auth_header_name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                               "Cloud providers: the header that carries the API key. Defaults to "
                               "`\"Authorization\"`."};
const RouteArg kAuthHeaderPrefix{"auth_header_prefix", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                                 "Cloud providers: the text before the key in that header. Defaults to `\"Bearer \"`; "
                                 "pass `\"\"` for gateways that expect the bare key."};
const RouteArg kWireFormat{"wire_format", ArgIn::JsonBody, {{"enum", json::array({"openai", "anthropic"})}},
                           false, Support::Available,
                           "Cloud providers: `\"openai\"` or `\"anthropic\"`. An `\"anthropic\"` provider is "
                           "served only through [`POST /v1/messages`](./anthropic.md#post-v1messages). Defaults "
                           "to `\"openai\"`."};

class InstallRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.install";
        s.methods = {"POST"};
        s.paths = {"install"};
        s.summary = "Install or update a backend, or register a cloud provider";
        s.description =
            "Installs or updates a recipe's backend, or registers a cloud provider.";
        s.notes = {
            "Errors carry an `error` string, except a cloud provider's field that has the right type "
            "but fails the provider checks, which carries an `error` object: a missing or empty "
            "`provider` or `base_url`, an invalid `provider`, `base_url`, `auth_header_name` or "
            "`auth_header_prefix`, or an `http://` base URL without `allow_insecure_http`.",
            "### Install a Cloud Provider\n"
            "<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>\n"
            "\n"
            "The base URL is saved to `config.json`. "
            "See the [Cloud Offload guide](../guide/configuration/cloud.md) for "
            "the full workflow.\n"
            "\n"
            "Optional fields are applied only when present in the request body. Re-installing a "
            "provider without them keeps its stored values, so updating just the `base_url` never "
            "resets a custom auth header or the `allow_insecure_http` opt-in.",
        };
        s.args = {
            backend_routes::kRecipe, backend_routes::kBackend, common::kStream, kForce,
            kProvider, kBaseUrl, kApiKey, backend_routes::kAllowInsecureHttp,
            kAuthHeaderName, kAuthHeaderPrefix, kWireFormat, downloads::kSubscribe
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = install_schema();

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = DownloadManager::event_schema();

        s.responses = {response, stream};

        RouteExample local_example;
        local_example.name = "local";
        local_example.format = ResponseFormat::Json;
        local_example.args = ordered_json::parse(R"({
            "recipe": "llamacpp",
            "backend": "cpu"
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::EventStream;
        stream_example.args = ordered_json::parse(R"({
            "recipe": "llamacpp",
            "backend": "vulkan",
            "stream": true
        })");

        RouteExample job_example;
        job_example.name = "job";
        job_example.format = ResponseFormat::Json;
        job_example.args = ordered_json::parse(R"({
            "recipe": "llamacpp",
            "backend": "vulkan",
            "stream": true,
            "subscribe": false
        })");

        RouteExample cloud_example;
        cloud_example.name = "cloud";
        cloud_example.format = ResponseFormat::Json;
        cloud_example.args = ordered_json::parse(R"({
            "backend": "cloud",
            "provider": "fireworks",
            "base_url": "https://api.fireworks.ai/inference/v1"
        })");

        s.examples = {local_example, stream_example, job_example, cloud_example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kMissingRecipe,
                    kUnsupportedBackend, kMissingCloudFields, kInvalidProviderName,
                    kInvalidBaseUrl, kInvalidCloudOption, kInsecureHttpError, kUnexpectedException};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            if (req.value(backend_routes::kBackend, "") == "cloud") {
                install_cloud_provider(req, res);
                return;
            }

            const std::string recipe = req.value(backend_routes::kRecipe, "");
            const std::string backend = req.value(backend_routes::kBackend, "");
            if (!require_local_recipe(recipe, res)) {
                return;
            }
            bool stream = req.value(common::kStream, false);
            bool subscribe = req.value(downloads::kSubscribe, true);
            bool force = req.value(kForce, false);

            LOG(INFO, "Server") << "Installing backend: " << recipe << ":" << backend << std::endl;

            SystemInfoCache::invalidate_recipes();

            // A backend that needs manual setup (e.g. FLM on Linux) answers with the
            // setup guide instead of attempting an install.
            json system_info = SystemInfoCache::get_system_info_with_cache();
            if (system_info.contains("recipes") &&
                system_info["recipes"].contains(recipe) &&
                system_info["recipes"][recipe].contains("backends") &&
                system_info["recipes"][recipe]["backends"].contains(backend)) {
                const auto& backend_info = system_info["recipes"][recipe]["backends"][backend];
                std::string state = backend_info.value("state", "unsupported");
                std::string message = backend_info.value("message", "Backend is not supported on this system.");
                std::string action = backend_info.value("action", "");

                if (state == "unsupported" && !force) {
                    res.status = kUnsupportedBackend.status;
                    json error = {
                        {"error", "Cannot install " + recipe + ":" + backend + " on this system: " + message},
                        {"recipe", recipe},
                        {"backend", backend}
                    };
                    res.set_content(error.dump(), "application/json");
                    return;
                }

                if (action.find(".html") != std::string::npos) {
                    auto url_pos = action.find("https://");
                    if (url_pos != std::string::npos) {
                        json response = {
                            {"action", action.substr(url_pos)},
                            {"recipe", recipe},
                            {"backend", backend}
                        };
                        res.set_content(response.dump(), "application/json");
                        return;
                    }
                }
            }

            BackendManager* backend_manager = ctx_.backend_manager;
            ModelManager* model_manager = ctx_.model_manager;
            if (stream) {
                DownloadManager::Operation operation =
                    [backend_manager, model_manager, recipe, backend, force](
                        DownloadProgressCallback progress_cb) {
                        backend_manager->install_backend(recipe, backend, force, progress_cb);
                        SystemInfoCache::invalidate_recipes();
                        model_manager->invalidate_models_cache();
                    };

                if (!subscribe) {
                    const std::string display_name = recipe + ":" + backend;
                    res.set_content(ctx_.downloads->start("backend:" + display_name, "backend",
                                                          display_name, operation).dump(),
                                    "application/json");
                    return;
                }

                stream_response(req, res, [operation](const std::string&, httplib::DataSink& sink) {
                    DownloadManager::stream(operation, sink);
                });
            } else {
                backend_manager->install_backend(recipe, backend, force);
                SystemInfoCache::invalidate_recipes();
                model_manager->invalidate_models_cache();
                json response = {
                    {"status", "success"},
                    {"recipe", recipe},
                    {"backend", backend}
                };
                res.set_content(response.dump(), "application/json");
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_install: " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, e.what());
        }
    }

private:
    // A cloud provider has no binary to fetch: installing one registers its base URL so
    // ModelManager can discover its catalog once an API key resolves. Optional fields
    // apply only when present, so a re-install that omits one keeps the stored value.
    void install_cloud_provider(const RouteRequest& req, httplib::Response& res) {
        CloudProviderRegistry& registry = *ctx_.cloud_registry;
        const std::string provider = req.value(kProvider, "");
        const std::string base_url = req.value(kBaseUrl, "");
        const std::string api_key = req.value(kApiKey, "");

        auto reject = [&](const RouteError& error, const std::string& message) {
            write_openai_error(res, error, message);
        };
        if (provider.empty() || base_url.empty()) {
            reject(kMissingCloudFields,
                   "Cloud install requires 'provider' and 'base_url' string fields");
            return;
        }
        if (auto err = CloudProviderRegistry::validate_provider_name(provider); !err.empty()) {
            reject(kInvalidProviderName, err);
            return;
        }
        if (auto err = CloudProviderRegistry::validate_base_url(base_url); !err.empty()) {
            reject(kInvalidBaseUrl, err);
            return;
        }

        CloudProviderRegistry::InstallOptions install_options;
        auto read_validated_field = [&](const RouteArg& field,
                                        std::string (*validate)(const std::string&),
                                        std::optional<std::string>& out) {
            if (!req.has(field)) return true;
            auto value = req.arg(field).get<std::string>();
            if (auto err = validate(value); !err.empty()) {
                reject(kInvalidCloudOption, err);
                return false;
            }
            out = std::move(value);
            return true;
        };
        if (!read_validated_field(kAuthHeaderName,
                                  CloudProviderRegistry::validate_auth_header_name,
                                  install_options.auth_header_name) ||
            !read_validated_field(kAuthHeaderPrefix,
                                  CloudProviderRegistry::validate_auth_header_prefix,
                                  install_options.auth_header_prefix)) {
            return;
        }
        if (req.has(kWireFormat)) {
            install_options.wire_format = req.arg(kWireFormat).get<std::string>();
        }
        if (req.has(backend_routes::kAllowInsecureHttp)) {
            install_options.allow_insecure_http =
                req.arg(backend_routes::kAllowInsecureHttp).get<bool>();
        }
        // The http:// opt-in gate below reflects the effective state: an
        // omitted flag on a re-install keeps whatever was stored.
        const bool allow_insecure_http =
            install_options.allow_insecure_http.value_or(registry.allow_insecure_http_for(provider));

        const auto env_state = registry.auth_state(provider);
        if (CloudProviderRegistry::is_http_base_url(base_url) &&
            !allow_insecure_http &&
            (!api_key.empty() || env_state.env_var_set)) {
            write_insecure_http_error(res, provider);
            return;
        }
        LOG(INFO, "Server") << "Installing cloud provider '" << provider
                            << "' with base_url " << base_url << std::endl;
        registry.install(provider, base_url, install_options);
        persist_cloud_providers(ctx_);

        // An api_key makes this install and auth in one call. The provider's environment
        // variable still wins: the key is not stored, and the install succeeds anyway.
        bool runtime_key_stored = false;
        if (!api_key.empty()) {
            runtime_key_stored = registry.set_runtime_key(provider, api_key);
        }

        // Install means registered, not verified: discovery may find nothing, and
        // /v1/system-info reports the count later.
        size_t models_after = ctx_.model_manager->refresh_cloud_models(provider);
        const auto state = registry.auth_state(provider);
        const auto auth_header = registry.auth_header_for(provider);

        json response = {
            {"status", "success"},
            {"backend", "cloud"},
            {"provider", provider},
            {"base_url", registry.base_url_for(provider)},
            {"allow_insecure_http", registry.allow_insecure_http_for(provider)},
            {"auth_header_name", auth_header.name},
            {"auth_header_prefix", auth_header.prefix},
            {"wire_format", registry.wire_format_for(provider)},
            {"models_discovered", models_after},
            {"auth_state", {
                {"env_var_set", state.env_var_set},
                {"runtime_key_set", state.runtime_key_set}
            }}
        };
        std::vector<std::string> warnings = CloudProviderRegistry::base_url_warnings(
            registry.base_url_for(provider),
            state.env_var_set || state.runtime_key_set);
        if (!api_key.empty() && !runtime_key_stored) {
            warnings.push_back(CloudProviderRegistry::env_var_name(provider) +
                " is set; supplied api_key was ignored.");
        }
        attach_warnings(response, warnings);
        res.set_content(response.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_install_route(ServerContext& ctx) {
    return std::make_unique<InstallRoute>(ctx);
}

} // namespace lemon
