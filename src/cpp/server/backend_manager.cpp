#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

BackendManager::BackendManager() {
    try {
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        backend_versions_ = utils::JsonUtils::load_from_file(config_path);
    } catch (const std::exception& e) {
        LOG(WARNING, "BackendManager") << "Could not load backend_versions.json: " << e.what() << std::endl;
        backend_versions_ = json::object();
    }
}

std::string BackendManager::get_version_from_config(const std::string& recipe, const std::string& backend) {
    // The "system" backend doesn't have a version in backend_versions.json
    // because it uses a pre-installed binary from the system PATH
    if (backend == "system") {
        return "";
    }

    if (!backend_versions_.contains(recipe) || !backend_versions_[recipe].is_object()) {
        throw std::runtime_error("backend_versions.json is missing '" + recipe + "' section");
    }
    const auto& recipe_config = backend_versions_[recipe];
    if (!recipe_config.contains(backend) || !recipe_config[backend].is_string()) {
        throw std::runtime_error("backend_versions.json is missing version for: " + recipe + ":" + backend);
    }
    return recipe_config[backend].get<std::string>();
}

// ============================================================================
// Core operations
// ============================================================================

// ============================================================================
// Install parameters
// ============================================================================

BackendManager::InstallParams BackendManager::get_install_params(const std::string& recipe, const std::string& backend) {
    if (recipe == "flm") {
        throw std::runtime_error("FLM uses a special installer and cannot be installed via get_install_params");
    }

    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }
    std::string version = get_version_from_config(recipe, backend);

    if (!spec->install_params_fn) {
        throw std::runtime_error("No install params function for recipe: " + recipe);
    }

    auto params = spec->install_params_fn(backend, version);
    return {params.repo, params.filename, version};
}

void BackendManager::install_backend(const std::string& recipe, const std::string& backend,
                                     DownloadProgressCallback progress_cb) {
    LOG(DEBUG, "BackendManager") << "Installing " << recipe << ":" << backend << std::endl;

    // System backend uses a pre-installed binary from PATH - nothing to install
    if (backend == "system") {
        return;
    }

    // FLM special case - uses installer exe with its own install logic
    if (recipe == "flm") {
        auto status = SystemInfoCache::get_flm_status();
        if (status.state == "installed") {
            // Already installed — nothing to do
        } else if (status.state == "unsupported") {
            throw std::runtime_error("FLM is not supported on this system: " + status.message);
        } else {
            // installable, update_required, or action_required
            backends::FastFlowLMServer flm_installer("info", nullptr, this);
            flm_installer.install(backend);
            // install() calls SystemInfoCache::invalidate_recipes()
        }
        // Re-read status after install
        status = SystemInfoCache::get_flm_status();
        if (!status.is_ready()) {
            throw std::runtime_error("FLM installation incomplete: " + status.message +
                (status.action.empty() ? "" : ". " + status.action));
        }
        return;
    }

    auto params = get_install_params(recipe, backend);
    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }

    backends::BackendUtils::install_from_github(
        *spec, params.version, params.repo, params.filename, backend, progress_cb);
}

void BackendManager::uninstall_backend(const std::string& recipe, const std::string& backend) {
    LOG(DEBUG, "BackendManager") << "Uninstalling " << recipe << ":" << backend << std::endl;

    if (recipe == "flm") {
        throw std::runtime_error("Uninstall FastFlowLM using their Windows uninstaller.");
    }

    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }

    std::string install_dir = backends::BackendUtils::get_install_directory(spec->recipe, backend);

    if (fs::exists(install_dir)) {
        // On Windows, antivirus scanning or indexing can briefly lock files after extraction.
        // Retry a few times with a short delay to handle transient locks.
        std::error_code ec;
        for (int attempt = 0; attempt < 5; ++attempt) {
            fs::remove_all(install_dir, ec);
            if (!ec || !fs::exists(install_dir)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (ec && fs::exists(install_dir)) {
            throw std::runtime_error("Failed to remove " + install_dir + ": " + ec.message());
        }
        LOG(DEBUG, "BackendManager") << "Removed: " << install_dir << std::endl;
    } else {
        LOG(DEBUG, "BackendManager") << "Nothing to uninstall at: " << install_dir << std::endl;
    }

}

// ============================================================================
// Query operations
// ============================================================================

std::string BackendManager::get_latest_version(const std::string& recipe, const std::string& backend) {
    try {
        return get_version_from_config(recipe, backend);
    } catch (...) {
        return "";
    }
}

json BackendManager::get_all_backends_status() {
    auto statuses = SystemInfo::get_all_recipe_statuses();
    json result = json::array();

    for (const auto& recipe : statuses) {
        json recipe_json;
        recipe_json["recipe"] = recipe.name;

        json backends_json = json::array();
        for (const auto& backend : recipe.backends) {
            json b;
            b["name"] = backend.name;
            b["state"] = backend.state;
            b["message"] = backend.message;
            b["action"] = backend.action;
            if (!backend.version.empty()) {
                b["version"] = backend.version;
            }

            // Add release URL
            std::string release_url = get_release_url(recipe.name, backend.name);
            if (!release_url.empty()) {
                b["release_url"] = release_url;
            }

            backends_json.push_back(b);
        }
        recipe_json["backends"] = backends_json;
        result.push_back(recipe_json);
    }

    return result;
}

std::string BackendManager::get_release_url(const std::string& recipe, const std::string& backend) {
    try {
        if (recipe == "flm") {
            std::string version = get_latest_version(recipe, backend);
            if (!version.empty()) {
                return "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + version;
            }
            return "";
        }

        auto params = get_install_params(recipe, backend);
        return "https://github.com/" + params.repo + "/releases/tag/" + params.version;
    } catch (...) {
        return "";
    }
}

std::string BackendManager::get_download_filename(const std::string& recipe, const std::string& backend) {
    try {
        auto params = get_install_params(recipe, backend);
        return params.filename;
    } catch (...) {
        return "";
    }
}

BackendManager::BackendEnrichment BackendManager::get_backend_enrichment(const std::string& recipe, const std::string& backend) {
    BackendEnrichment result;
    try {
        if (recipe == "flm") {
            result.version = get_latest_version(recipe, backend);
            if (!result.version.empty()) {
                result.release_url = "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + result.version;
            }
            // FLM installer artifact used by install_flm_if_needed().
            result.download_filename = "flm-setup.exe";
            return result;
        }

        // All standard recipes (including ryzenai-llm): one get_install_params() call gives us everything
        auto params = get_install_params(recipe, backend);
        result.release_url = "https://github.com/" + params.repo + "/releases/tag/" + params.version;
        result.download_filename = params.filename;
        result.version = params.version;
    } catch (...) {}
    return result;
}

} // namespace lemon
