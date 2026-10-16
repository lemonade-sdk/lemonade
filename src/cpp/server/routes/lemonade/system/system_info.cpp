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
using ordered_json = nlohmann::ordered_json;

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

ordered_json system_info_schema() {
    return ordered_json::parse(R"json({
        "type": "object",
        "required": ["devices", "recipes", "unavailable_recipes", "model_storage", "cloud"],
        "properties": {
            "OS Version": {"type": "string", "description": "Operating system name and version"},
            "Processor": {"type": "string", "description": "CPU model name"},
            "Physical Memory": {"type": "string", "description": "Total RAM"},
            "model_storage": {
                "type": "object",
                "description": "Drive-level storage information for the active configured model storage path. Values are reported in bytes for storage meters; this is not a recursive sum of Lemonade model files.",
                "required": ["path", "used_bytes", "total_bytes", "free_bytes"],
                "properties": {
                    "path": {"type": "string", "description": "Active model storage path: the `models_dir` setting, or the Hugging Face cache directory when it is `auto`."},
                    "used_bytes": {"type": ["integer", "null"], "description": "Used bytes on the model-storage drive"},
                    "total_bytes": {"type": ["integer", "null"], "description": "Total capacity of the model-storage drive"},
                    "free_bytes": {"type": ["integer", "null"], "description": "Free bytes available to the Lemonade Server process on the model-storage drive"},
                    "error": {"type": "string", "description": "Only when the drive could not be measured."}
                }
            },
            "devices": {
                "type": "object",
                "description": "Hardware devices detected on the system (no software/support information)",
                "properties": {
                    "cpu": {"type": "object", "description": "CPU information (name, cores, threads)"},
                    "amd_gpu": {"type": "array", "description": "Usable AMD GPUs, both integrated and discrete; empty when there are none."},
                    "nvidia_gpu": {"type": "array", "description": "NVIDIA GPUs. When none is found, it holds one entry whose `available` is `false` and whose `error` says why."},
                    "amd_npu": {"type": "object", "description": "The AMD NPU. Always present; `available` is `false` when none is found, and `error` says why."}
                }
            },
            "recipes": {
                "type": "object",
                "description": "Software recipes and their backend support status",
                "additionalProperties": {
                    "type": "object",
                    "description": "One entry per recipe, such as `llamacpp` or `flm`.",
                    "required": ["experimental", "backends"],
                    "properties": {
                        "default_backend": {"type": "string", "description": "Preferred backend for this system: the one set in `config.json`, else the first in preference order that this system's devices can run, favoring an installed one. Absent when none is set and none can run here."},
                        "experimental": {"type": "boolean", "description": "`true` when the `default_backend`'s tier is `experimental` or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats)."},
                        "order": {"type": "integer", "description": "The recipe's position in Lemonade's backend registry, from `0`."},
                        "display_name": {"type": "string", "description": "The recipe's name for people, such as `Llama.cpp GPU`."},
                        "web_display_name": {"type": "string", "description": "The recipe's name on the Lemonade website."},
                        "modality": {"type": "string", "description": "The kind of model the recipe mainly serves, such as `Text generation`."},
                        "selectable_backend": {"type": "boolean", "description": "Whether a `<recipe>_backend` option chooses the recipe's backend."},
                        "uses_ctx_size": {"type": "boolean", "description": "Whether the recipe accepts the shared `ctx_size` option."},
                        "slot_policy": {"enum": ["standard", "exclusive_npu", "coexist_by_type", "unmetered"], "description": "How the recipe's models count against loaded-model limits: `standard`, `exclusive_npu` (loading one evicts every NPU model), `coexist_by_type` (one per model type) or `unmetered` (never counted or evicted)."},
                        "support": {"type": "array", "description": "Every backend's operating systems, devices and tier, the same on every host."},
                        "options": {"type": "array", "description": "The recipe's load options, with each one's CLI flag, default and help text."},
                        "backends": {
                            "type": "object",
                            "description": "Available backends for this recipe",
                            "additionalProperties": {
                                "type": "object",
                                "description": "One entry per backend.",
                                "required": ["devices", "state", "tier", "format"],
                                "properties": {
                                    "devices": {"type": "array", "items": {"type": "string"}, "description": "Devices **on this system** that support this backend; empty when none do."},
                                    "state": {"enum": ["unsupported", "installable", "update_required", "update_available", "installed"], "description": "Backend lifecycle state: `unsupported`, `installable`, `update_required`, `update_available` or `installed`."},
                                    "message": {"type": "string", "description": "Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`."},
                                    "action": {"type": "string", "description": "An instruction for the user: for install and update, usually the exact CLI command; for other states it may be empty or another action, such as a URL."},
                                    "version": {"type": "string", "description": "Installed or configured backend version (when available)"},
                                    "tier": {"enum": ["core", "experimental", "guest"], "description": "`core`, `experimental`, or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats)."},
                                    "format": {"enum": ["native", "python", "container"], "description": "`native`, `python`, or `container`. See [Backend Tiers and Formats](#backend-tiers-and-formats)."},
                                    "release_url": {"type": "string", "description": "Where the backend's release asset downloads from."},
                                    "download_filename": {"type": "string", "description": "The release asset's filename."},
                                    "can_uninstall": {"type": "boolean", "description": "Whether the backend can be uninstalled. Lemonade sets it only on a backend that does not run on this operating system, always to `true`, so a missing value also means `true`."}
                                }
                            }
                        }
                    }
                }
            },
            "cloud": {
                "type": "object",
                "description": "Cloud providers installed on this server.",
                "required": ["providers"],
                "properties": {
                    "providers": {
                        "type": "array",
                        "description": "One entry per installed provider.",
                        "items": {
                            "type": "object",
                            "properties": {
                                "name": {"type": "string", "description": "Provider name used as the model-name prefix (e.g. `fireworks`)."},
                                "base_url": {"type": "string", "description": "Persisted base URL from `config.json`."},
                                "auth_header_name": {"type": "string", "description": "Header this provider's API key is sent in (default `Authorization`)."},
                                "auth_header_prefix": {"type": "string", "description": "Value prefix placed before the key (default `Bearer `)."},
                                "wire_format": {"enum": ["openai", "anthropic"], "description": "Request/response shape this provider speaks: `openai` (default) or `anthropic`."},
                                "env_var": {"type": "string", "description": "Canonical environment variable name for this provider's API key (e.g. `LEMONADE_FIREWORKS_API_KEY`). The variable's *name* is reported, never its value."},
                                "env_var_set": {"type": "boolean", "description": "`true` if the env var is set in `lemond`'s environment."},
                                "runtime_key_set": {"type": "boolean", "description": "`true` if an in-memory key has been supplied through [`POST /v1/cloud/auth`](#post-v1cloudauth) since `lemond` started."},
                                "models_discovered": {"type": "integer", "description": "Number of chat-capable models currently in the catalog for this provider."},
                                "allow_insecure_http": {"type": "boolean", "description": "Whether the provider may be reached over `http://`."},
                                "warnings": {"type": "array", "items": {"type": "string"}, "description": "Problems found with the provider's configuration."},
                                "warning": {"type": "string", "description": "`warnings` joined into one string, for older clients."}
                            }
                        }
                    }
                }
            },
            "unavailable_recipes": {"type": "array", "items": {"type": "string"}, "description": "Recipes all of whose models are filtered out on this host. Reported separately so `recipes` stays the same on every host; dynamic-model backends (`cloud`, `flm`) are never listed."},
            "no_fetch_executables": {"type": "boolean", "description": "Whether the server is configured not to download backend executables."}
        }
    })json");
}

class SystemInfoRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.system_info";
        s.methods = {"GET"};
        s.paths = {"system-info"};
        s.summary = "Read the host's hardware and backend support";
        s.description =
            "Reports the host's hardware, the devices Lemonade can use, and which backends each "
            "recipe supports on this system.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
            "### Backend Tiers and Formats\n"
            "\n"
            "| Tier | Meaning |\n"
            "|------|---------|\n"
            "| `core` | Supported by Lemonade's maintainers for production use. |\n"
            "| `experimental` | Provided by Lemonade's maintainers, not supported for production use. |\n"
            "| `guest` | Developed and published outside Lemonade's maintainers. Use at your own risk. The models of a recipe whose only backends on this system are guest backends stay hidden from [`GET /v1/models`](./openai.md#get-v1models) with `show_all=true` until one of those backends is installed. |\n"
            "\n"
            "| Format | Meaning |\n"
            "|--------|---------|\n"
            "| `native` | A compiled executable. |\n"
            "| `python` | Python, packaged with its own interpreter. |\n"
            "| `container` | An OCI image, run by a pre-installed Podman or Docker. |\n"
            "\n"
            "The same backend can carry different labels on different operating systems. Each entry "
            "in a recipe's `backends` object reports the labels for this system's operating system.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = system_info_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

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
