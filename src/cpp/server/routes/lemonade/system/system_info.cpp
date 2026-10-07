#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/cloud_providers.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

namespace lemon {
namespace {

using json = nlohmann::json;

json get_model_storage_stats(const std::string& model_storage_path) {
    auto make_error_result = [](const fs::path& path, const std::string& error) {
        return json{
            {"path", utils::path_to_utf8(path)},
            {"used_bytes", nullptr},
            {"total_bytes", nullptr},
            {"free_bytes", nullptr},
            {"error", error}
        };
    };

    std::error_code ec;
    fs::path configured_path;

    if (!model_storage_path.empty()) {
        configured_path = utils::path_from_utf8(model_storage_path);
    }

    if (configured_path.empty()) {
        configured_path = fs::current_path(ec);
        if (ec) {
            LOG(WARNING, "Server") << "Unable to resolve current path for model storage stats: "
                                   << ec.message() << std::endl;
            return make_error_result(
                fs::path{},
                "Unable to resolve current path: " + ec.message()
            );
        }
    } else if (configured_path.is_relative()) {
        configured_path = fs::absolute(configured_path, ec);
        if (ec) {
            LOG(WARNING, "Server") << "Unable to resolve model storage path "
                                   << model_storage_path << ": " << ec.message() << std::endl;
            return make_error_result(
                configured_path,
                "Unable to resolve model storage path: " + ec.message()
            );
        }
    }

    configured_path = configured_path.lexically_normal();

    // The storage path may not exist yet, so measure the nearest existing ancestor.
    fs::path probe_path = configured_path;
    while (!probe_path.empty()) {
        std::error_code exists_ec;
        if (fs::exists(probe_path, exists_ec)) {
            break;
        }

        if (exists_ec) {
            LOG(WARNING, "Server") << "Unable to inspect model storage path "
                                   << utils::path_to_utf8(probe_path) << ": "
                                   << exists_ec.message() << std::endl;
            return make_error_result(
                configured_path,
                "Unable to inspect model storage path: " + exists_ec.message()
            );
        }

        fs::path parent_path = probe_path.parent_path();
        if (parent_path == probe_path) {
            break;
        }

        probe_path = parent_path;
    }

    auto space_info = fs::space(probe_path, ec);
    if (ec) {
        LOG(WARNING, "Server") << "Unable to read model storage stats for "
                               << utils::path_to_utf8(probe_path) << ": "
                               << ec.message() << std::endl;
        return make_error_result(
            configured_path,
            "Unable to read model storage stats: " + ec.message()
        );
    }

    const uintmax_t total_bytes = space_info.capacity;
    const uintmax_t free_bytes = std::min(space_info.available, space_info.capacity);
    const uintmax_t used_bytes = total_bytes - free_bytes;

    return json{
        {"path", utils::path_to_utf8(configured_path)},
        {"used_bytes", static_cast<uint64_t>(used_bytes)},
        {"total_bytes", static_cast<uint64_t>(total_bytes)},
        {"free_bytes", static_cast<uint64_t>(free_bytes)}
    };
}

// Adds release_url, download_filename and version from BackendManager to each backend.
void enrich_recipes(BackendManager* backend_manager, json& recipes) {
    if (!backend_manager) return;

    for (auto& [recipe_name, recipe_info] : recipes.items()) {
        if (!recipe_info.contains("backends")) continue;
        for (auto& [backend_name, backend_info] : recipe_info["backends"].items()) {
            try {
                auto enrichment = backend_manager->get_backend_enrichment(recipe_name, backend_name);
                if (!enrichment.release_url.empty()) {
                    backend_info["release_url"] = enrichment.release_url;
                }
                if (!enrichment.download_filename.empty()) {
                    backend_info["download_filename"] = enrichment.download_filename;
                }
                if (!backend_info.contains("version") || backend_info["version"].get<std::string>().empty()) {
                    if (!enrichment.version.empty()) {
                        backend_info["version"] = enrichment.version;
                    }
                }
            } catch (...) {}
        }
    }
}

json system_info_schema() {
    return json::parse(R"({
        "type": "object",
        "required": ["devices", "recipes", "unavailable_recipes", "model_storage", "cloud"],
        "properties": {
            "OS Version": {"type": "string", "description": "Operating system name and version."},
            "Processor": {"type": "string", "description": "CPU model name."},
            "Physical Memory": {"type": "string", "description": "Total RAM."},
            "OEM System": {"type": "string", "description": "Windows only: system or laptop model."},
            "BIOS Version": {"type": "string", "description": "Windows only."},
            "CPU Max Clock": {"type": "string", "description": "Windows only."},
            "Windows Power Setting": {"type": "string", "description": "Windows only: current power plan."},
            "devices": {
                "type": "object",
                "description": "Hardware detected on the system, without software support information: cpu, plus amd_gpu and nvidia_gpu arrays and amd_npu when present.",
                "properties": {
                    "cpu": {"type": "object", "description": "Name, cores and threads."},
                    "amd_gpu": {"type": "array", "description": "AMD GPUs, integrated and discrete."},
                    "nvidia_gpu": {"type": "array"},
                    "amd_npu": {"type": "object"}
                }
            },
            "recipes": {
                "type": "object",
                "description": "Each recipe's backends and their support on this system.",
                "additionalProperties": {
                    "type": "object",
                    "required": ["backends"],
                    "properties": {
                        "default_backend": {"type": "string", "description": "Backend the server prefers on this system; present when at least one backend is not unsupported."},
                        "backends": {
                            "type": "object",
                            "additionalProperties": {
                                "type": "object",
                                "required": ["devices", "state"],
                                "properties": {
                                    "devices": {"type": "array", "items": {"type": "string"}, "description": "Devices on this system that support the backend; empty when unsupported."},
                                    "state": {"enum": ["unsupported", "installable", "update_required", "installed"]},
                                    "message": {"type": "string", "description": "Status text for GUI and CLI users: required for unsupported, installable and update_required; empty for installed."},
                                    "action": {"type": "string", "description": "What the user should do, typically an exact CLI command for install and update, or a URL; may be empty."},
                                    "version": {"type": "string", "description": "Installed or configured backend version, when available."},
                                    "release_url": {"type": "string"},
                                    "download_filename": {"type": "string"}
                                }
                            }
                        }
                    }
                }
            },
            "unavailable_recipes": {"type": "array", "items": {"type": "string"},
                                    "description": "Recipes all of whose models are filtered out on this host. Reported separately so recipes stays the same on every host; dynamic-model backends (cloud, flm) are never listed."},
            "no_fetch_executables": {"type": "boolean", "description": "Whether the server is configured not to download backend executables."},
            "model_storage": {
                "type": "object",
                "description": "Drive-level storage for the configured model storage path, for storage meters. Not a recursive sum of model files.",
                "required": ["path", "used_bytes", "total_bytes", "free_bytes"],
                "properties": {
                    "path": {"type": "string"},
                    "used_bytes": {"type": ["integer", "null"]},
                    "total_bytes": {"type": ["integer", "null"]},
                    "free_bytes": {"type": ["integer", "null"], "description": "Bytes available to the server process."},
                    "error": {"type": "string", "description": "Only when the drive could not be measured."}
                }
            },
            "cloud": {
                "type": "object",
                "required": ["providers"],
                "properties": {
                    "providers": {"type": "array", "description": "One entry per installed cloud provider; the API key itself is never reported.", "items": {
                        "type": "object",
                        "properties": {
                            "name": {"type": "string", "description": "Provider name, used as the model-name prefix, e.g. fireworks."},
                            "base_url": {"type": "string", "description": "Base URL persisted in config.json."},
                            "allow_insecure_http": {"type": "boolean"},
                            "auth_header_name": {"type": "string", "description": "Header the API key is sent in; default Authorization."},
                            "auth_header_prefix": {"type": "string", "description": "Value prefix before the key; default \"Bearer \"."},
                            "wire_format": {"enum": ["openai", "anthropic"]},
                            "env_var": {"type": "string", "description": "Name of the provider's API key environment variable, e.g. LEMONADE_FIREWORKS_API_KEY."},
                            "env_var_set": {"type": "boolean", "description": "Whether that variable is set in lemond's environment."},
                            "runtime_key_set": {"type": "boolean", "description": "Whether a key was supplied with POST /v1/cloud/auth since the server started."},
                            "models_discovered": {"type": "integer", "description": "Chat-capable models in the catalog for this provider."},
                            "warnings": {"type": "array", "items": {"type": "string"}},
                            "warning": {"type": "string"}
                        }
                    }}
                }
            }
        }
    })");
}

class SystemInfoRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.system_info";
        s.methods = {"GET"};
        s.paths = {"system-info"};
        s.summary = "System information and device enumeration";
        s.description =
            "Reports hardware details, detected devices, each recipe's backend support on this "
            "system, model storage, and installed cloud providers.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = system_info_schema();
        response.example = json::object();
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        // SystemInfoCache is the single source of truth for hardware + recipes.
        // Recipes are cached until invalidated by install/uninstall.
        json system_info = SystemInfoCache::get_system_info_with_cache();

        if (system_info.contains("recipes")) {
            enrich_recipes(ctx_.backend_manager, system_info["recipes"]);
        }

        // Surfaced as a separate host-specific field rather than by pruning
        // `recipes`, which must stay canonical: the docs generator renders
        // README/models.js from it and would otherwise drift with the generating
        // machine's memory. Dynamic-model backends (cloud, flm) are never listed.
        json unavailable = json::array();
        for (const std::string& recipe : ctx_.model_manager->recipes_with_all_models_filtered()) {
            const BackendDescriptor* desc = backends::descriptor_for(recipe);
            if (desc && desc->dynamic_models) {
                continue;
            }
            unavailable.push_back(recipe);
        }
        system_info["unavailable_recipes"] = std::move(unavailable);

        // Surface runtime config flags that affect client-side install/download UX.
        system_info["no_fetch_executables"] = ctx_.config->no_fetch_executables();

        system_info["model_storage"] = get_model_storage_stats(utils::get_hf_cache_dir());

        // Clients use auth state to decide whether to prompt for an API key or show a
        // "configured by env var" badge.
        CloudProviderRegistry& registry = *ctx_.cloud_registry;
        json providers = json::array();
        for (const auto& rec : registry.list_installed()) {
            auto state = registry.auth_state(rec.name);
            json provider = {
                {"name", rec.name},
                {"base_url", rec.base_url},
                {"allow_insecure_http", rec.allow_insecure_http},
                {"auth_header_name", rec.auth_header_name},
                {"auth_header_prefix", rec.auth_header_prefix},
                {"wire_format", rec.wire_format},
                {"env_var", CloudProviderRegistry::env_var_name(rec.name)},
                {"env_var_set", state.env_var_set},
                {"runtime_key_set", state.runtime_key_set},
                {"models_discovered", ctx_.model_manager->count_cloud_models(rec.name)}
            };
            attach_warnings(
                provider,
                CloudProviderRegistry::base_url_warnings(
                    rec.base_url,
                    state.env_var_set || state.runtime_key_set));
            providers.push_back(std::move(provider));
        }
        system_info["cloud"] = {{"providers", providers}};

        res.set_content(system_info.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_system_info_route(ServerContext& ctx) {
    return std::make_unique<SystemInfoRoute>(ctx);
}

} // namespace lemon
