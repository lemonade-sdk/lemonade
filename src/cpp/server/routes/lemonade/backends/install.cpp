#include <optional>
#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/server/download_manager.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json install_schema() {
    auto local = json::parse(R"({
        "type": "object",
        "description": "A local backend installed or already up to date.",
        "required": ["status", "recipe", "backend"],
        "properties": {
            "status": {"const": "success"},
            "recipe": {"type": "string"},
            "backend": {"type": "string"}
        }
    })");
    auto manual = json::parse(R"({
        "type": "object",
        "description": "A backend that needs manual setup on this system, such as FLM on Linux: nothing was installed and action is the setup guide's URL.",
        "required": ["action", "recipe", "backend"],
        "properties": {
            "action": {"type": "string"},
            "recipe": {"type": "string"},
            "backend": {"type": "string"}
        }
    })");
    auto cloud = json::parse(R"({
        "type": "object",
        "description": "A cloud provider registered.",
        "required": ["status", "backend", "provider", "base_url", "allow_insecure_http",
                     "auth_header_name", "auth_header_prefix", "wire_format", "models_discovered",
                     "auth_state"],
        "properties": {
            "status": {"const": "success"},
            "backend": {"const": "cloud"},
            "provider": {"type": "string"},
            "base_url": {"type": "string"},
            "allow_insecure_http": {"type": "boolean"},
            "auth_header_name": {"type": "string"},
            "auth_header_prefix": {"type": "string"},
            "wire_format": {"enum": ["openai", "anthropic"]},
            "models_discovered": {"type": "integer", "description": "Chat models discovered with the resolved API key; 0 when no key resolves."},
            "auth_state": {
                "type": "object",
                "properties": {
                    "env_var_set": {"type": "boolean", "description": "Whether LEMONADE_<PROVIDER>_API_KEY is set for lemond."},
                    "runtime_key_set": {"type": "boolean", "description": "Whether a key is held in lemond's memory."}
                }
            },
            "warnings": {"type": "array", "items": {"type": "string"}},
            "warning": {"type": "string", "description": "The warnings joined into one string, for older clients."}
        }
    })");
    auto job = DownloadManager::job_schema();
    job["description"] = "stream=true with subscribe=false: the server-owned download job just started.";
    return {{"oneOf", json::array({local, manual, cloud, job})}};
}

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
            "Installs or updates the backend for a recipe, or registers a cloud provider when "
            "`backend` is `\"cloud\"`.";
        s.notes = {
            "**Status:** the cloud-provider branch is experimental.",
            "A local backend that is already installed but outdated is updated to the configured "
            "version. A backend this system does not support is refused with `400` unless "
            "`force` is set, and one that needs manual setup answers with the setup guide's URL "
            "instead of installing. See [Install a Cloud Provider](#install-a-cloud-provider) for "
            "the cloud branch.",
            "`stream: true` sends progress as server-sent events; with `subscribe: false` as well, "
            "the server owns the download, which [`GET /v1/downloads`](#get-v1downloads) and "
            "[`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control.",
            "A local install without both `recipe` and `backend`, or a backend this system does "
            "not support, is answered with `400` and an `error` string; an invalid cloud "
            "provider field is answered with `400` and an `error` object. Any other failure, "
            "including invalid JSON, is answered with `500` and an `error` string.",
        };
        s.args = {
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Local backends, required: recipe name, e.g. `llamacpp`, `flm`, `whispercpp`, "
             "`sd-cpp` or `ryzenai-llm`."},
            {"backend", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Backend within the recipe, e.g. `vulkan`, `rocm`, `cpu` or `default`, or "
             "`\"cloud\"` to register a cloud provider."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Local backends: send progress as server-sent events. Defaults to `false`."},
            {"subscribe", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Local backends with `stream: true`: `false` starts a server-owned download and "
             "answers with its snapshot at once. Defaults to `true`."},
            {"force", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Local backends: install even when this system does not support the backend. "
             "Defaults to `false`."},
            {"provider", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers, required: short name, e.g. `fireworks`, matching `[a-z0-9_-]+`. "
             "It prefixes the provider's model names."},
            {"base_url", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers, required: base URL, usually ending in `/v1`, saved to "
             "`config.json`."},
            {"api_key", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers: API key, held in lemond's memory only. An environment variable "
             "for the provider takes precedence; see [`POST /v1/cloud/auth`](#post-v1cloudauth)."},
            {"allow_insecure_http", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Cloud providers: must be `true` to send an API key to an `http://` base URL. "
             "Defaults to `false`."},
            {"auth_header_name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers: header that carries the API key. Defaults to `Authorization`."},
            {"auth_header_prefix", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Cloud providers: text before the key in that header. Defaults to `\"Bearer \"`; "
             "pass `\"\"` for gateways that expect the bare key."},
            {"wire_format", ArgIn::JsonBody, {{"enum", json::array({"openai", "anthropic"})}},
             false, Support::Available,
             "Cloud providers: `openai` (default), or `anthropic` for a provider served only "
             "from `POST /v1/messages`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = install_schema();
        response.example = json::parse(R"({
            "backend": "cloud",
            "provider": "example",
            "base_url": "https://api.example.com/v1"
        })");

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = DownloadManager::event_schema();
        stream.example = json::parse(R"({
            "recipe": "llamacpp",
            "backend": "vulkan",
            "stream": true
        })");

        s.responses = {response, stream};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);

            if (request_json.value("backend", "") == "cloud") {
                install_cloud_provider(request_json, res);
                return;
            }

            std::string recipe = request_json.value("recipe", "");
            std::string backend = request_json.value("backend", "");
            bool stream = request_json.value("stream", false);
            bool subscribe = request_json.value("subscribe", true);
            bool force = request_json.value("force", false);

            if (recipe.empty() || backend.empty()) {
                write_plain_error(res, 400, "Both 'recipe' and 'backend' are required");
                return;
            }

            LOG(INFO, "Server") << "Installing backend: " << recipe << ":" << backend << std::endl;

            // Get fresh state before any checks
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
                    res.status = 400;
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
            write_plain_error(res, 500, e.what());
        }
    }

private:
    // A cloud provider has no binary to fetch: installing one registers its base URL so
    // ModelManager can discover its catalog once an API key resolves. Optional fields
    // apply only when present, so a re-install that omits one keeps the stored value.
    void install_cloud_provider(const json& request_json, httplib::Response& res) {
        CloudProviderRegistry& registry = *ctx_.cloud_registry;
        const std::string provider = request_json.value("provider", "");
        const std::string base_url = request_json.value("base_url", "");
        const std::string api_key = request_json.value("api_key", "");

        auto reject = [&](const std::string& message) {
            write_openai_error(res, 400, message);
        };
        if (provider.empty() || base_url.empty()) {
            reject("Cloud install requires 'provider' and 'base_url' string fields");
            return;
        }
        if (auto err = CloudProviderRegistry::validate_provider_name(provider); !err.empty()) {
            reject(err);
            return;
        }
        if (auto err = CloudProviderRegistry::validate_base_url(base_url); !err.empty()) {
            reject(err);
            return;
        }

        CloudProviderRegistry::InstallOptions install_options;
        auto read_validated_field = [&](const char* field,
                                        std::string (*validate)(const std::string&),
                                        std::optional<std::string>& out) {
            if (!request_json.contains(field)) return true;
            if (!request_json[field].is_string()) {
                reject(std::string(field) + " must be a string when provided");
                return false;
            }
            auto value = request_json[field].get<std::string>();
            if (auto err = validate(value); !err.empty()) {
                reject(err);
                return false;
            }
            out = std::move(value);
            return true;
        };
        if (!read_validated_field("auth_header_name",
                                  CloudProviderRegistry::validate_auth_header_name,
                                  install_options.auth_header_name) ||
            !read_validated_field("auth_header_prefix",
                                  CloudProviderRegistry::validate_auth_header_prefix,
                                  install_options.auth_header_prefix) ||
            !read_validated_field("wire_format",
                                  CloudProviderRegistry::validate_wire_format,
                                  install_options.wire_format)) {
            return;
        }
        if (request_json.contains("allow_insecure_http")) {
            if (!request_json["allow_insecure_http"].is_boolean()) {
                reject("allow_insecure_http must be a boolean when provided");
                return;
            }
            install_options.allow_insecure_http =
                request_json["allow_insecure_http"].get<bool>();
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
