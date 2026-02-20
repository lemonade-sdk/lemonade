#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/whisper_server.h"
#include "lemon/backends/sd_server.h"
#include "lemon/backends/kokoro_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace lemon {

BackendManager::BackendManager() {
    try {
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        backend_versions_ = utils::JsonUtils::load_from_file(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[BackendManager] Warning: Could not load backend_versions.json: " << e.what() << std::endl;
        backend_versions_ = json::object();
    }
}

std::string BackendManager::get_version_from_config(const std::string& recipe, const std::string& backend) {
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

static const backends::BackendSpec& get_spec_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") return backends::LlamaCppServer::SPEC;
    if (recipe == "whispercpp") return backends::WhisperServer::SPEC;
    if (recipe == "sd-cpp") return backends::SDServer::SPEC;
    if (recipe == "kokoro") return backends::KokoroServer::SPEC;
    if (recipe == "ryzenai-llm") return RyzenAIServer::SPEC;

    throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
}

// ============================================================================
// Install parameters
// ============================================================================

BackendManager::InstallParams BackendManager::get_install_params(const std::string& recipe, const std::string& backend) {
    if (recipe == "flm") {
        throw std::runtime_error("FLM uses a special installer and cannot be installed via get_install_params");
    }

    const auto& spec = get_spec_for_recipe(recipe);
    std::string version;

    if (recipe == "ryzenai-llm") {
        // ryzenai-server version is at top level in backend_versions.json (string, not nested object)
        if (backend_versions_.contains("ryzenai-server") && backend_versions_["ryzenai-server"].is_string()) {
            version = backend_versions_["ryzenai-server"].get<std::string>();
        } else {
            throw std::runtime_error("backend_versions.json is missing 'ryzenai-server' version");
        }
    } else {
        version = get_version_from_config(recipe, backend);
    }

    if (!spec.install_params_fn) {
        throw std::runtime_error("No install params function for recipe: " + recipe);
    }

    auto params = spec.install_params_fn(backend, version);
    return {params.repo, params.filename, version};
}

void BackendManager::install_backend(const std::string& recipe, const std::string& backend,
                                     DownloadProgressCallback progress_cb) {
    std::cout << "[BackendManager] Installing " << recipe << ":" << backend << std::endl;

    // FLM special case - uses installer exe
    if (recipe == "flm") {
        install_flm(backend);
        if (progress_cb) {
            DownloadProgress p;
            p.file = "flm-setup.exe";
            p.file_index = 1;
            p.total_files = 1;
            p.percent = 100;
            p.complete = true;
            progress_cb(p);
        }
        update_recipes_cache_entry(recipe, backend, true);
        return;
    }

    auto params = get_install_params(recipe, backend);
    const auto& spec = get_spec_for_recipe(recipe);

    // For ryzenai-llm, the backend field in install_from_github is "default" (no backend variants)
    std::string backend_dir = (recipe == "ryzenai-llm") ? "" : backend;

    backends::BackendUtils::install_from_github(
        spec, params.version, params.repo, params.filename, backend_dir, progress_cb);

    update_recipes_cache_entry(recipe, backend, true);
}

void BackendManager::install_flm(const std::string& backend) {
    // FLM install is complex (driver checks, installer exe, etc.)
    // We delegate to FastFlowLMServer's static install logic
    // For now, create a temporary FastFlowLMServer just to call install()
    // This is a stopgap - ideally we'd extract the FLM install logic to a shared place
    backends::FastFlowLMServer flm_server("info", nullptr);
    flm_server.install(backend);
}

void BackendManager::uninstall_backend(const std::string& recipe, const std::string& backend) {
    std::cout << "[BackendManager] Uninstalling " << recipe << ":" << backend << std::endl;

    if (recipe == "flm") {
        throw std::runtime_error("FLM cannot be uninstalled via Backend Manager (system installation)");
    }

    const auto& spec = get_spec_for_recipe(recipe);
    // For ryzenai-llm, backend is empty (no variants)
    std::string backend_dir = (recipe == "ryzenai-llm") ? "" : backend;

    std::string install_dir = backends::BackendUtils::get_install_directory(spec.recipe, backend_dir);

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
        std::cout << "[BackendManager] Removed: " << install_dir << std::endl;
    } else {
        std::cout << "[BackendManager] Nothing to uninstall at: " << install_dir << std::endl;
    }

    update_recipes_cache_entry(recipe, backend, false);
}

// ============================================================================
// Query operations
// ============================================================================

bool BackendManager::is_installed(const std::string& recipe, const std::string& backend) {
    if (recipe == "flm") {
        return SystemInfo::get_flm_version() != "";
    }

    try {
        const auto& spec = get_spec_for_recipe(recipe);
        // For ryzenai-llm, backend is empty (no variants)
        std::string backend_dir = (recipe == "ryzenai-llm") ? "" : backend;
        std::string path = backends::BackendUtils::get_backend_binary_path(spec, backend_dir);
        return !path.empty();
    } catch (...) {
        return false;
    }
}

std::string BackendManager::get_installed_version(const std::string& recipe, const std::string& backend) {
    if (recipe == "flm") {
        return SystemInfo::get_flm_version();
    }

    try {
        const auto& spec = get_spec_for_recipe(recipe);
        // For ryzenai-llm, backend is empty (no variants)
        std::string backend_dir = (recipe == "ryzenai-llm") ? "" : backend;
        std::string version_file = backends::BackendUtils::get_installed_version_file(spec, backend_dir);
        if (fs::exists(version_file)) {
            std::ifstream vf(version_file);
            std::string version;
            std::getline(vf, version);
            return version;
        }
    } catch (...) {}

    return "";
}

std::string BackendManager::get_latest_version(const std::string& recipe, const std::string& backend) {
    try {
        if (recipe == "ryzenai-llm") {
            if (backend_versions_.contains("ryzenai-server") && backend_versions_["ryzenai-server"].is_string()) {
                return backend_versions_["ryzenai-server"].get<std::string>();
            }
            return "";
        }

        if (recipe == "flm") {
            if (backend_versions_.contains("flm") && backend_versions_["flm"].contains("version")) {
                return backend_versions_["flm"]["version"].get<std::string>();
            }
            return "";
        }

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
        recipe_json["supported"] = recipe.supported;
        recipe_json["available"] = recipe.available;
        if (!recipe.error.empty()) {
            recipe_json["error"] = recipe.error;
        }

        json backends_json = json::array();
        for (const auto& backend : recipe.backends) {
            json b;
            b["name"] = backend.name;
            b["supported"] = backend.supported;
            b["available"] = backend.available;
            if (!backend.version.empty()) {
                b["version"] = backend.version;
            }
            if (!backend.error.empty()) {
                b["error"] = backend.error;
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

// ============================================================================
// Recipes cache
// ============================================================================

void BackendManager::set_recipes_cache(const json& recipes) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_recipes_ = recipes;
}

json BackendManager::get_recipes_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cached_recipes_;
}

void BackendManager::update_recipes_cache_entry(const std::string& recipe, const std::string& backend, bool installed) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (cached_recipes_.empty()) return;

    if (!cached_recipes_.contains(recipe) ||
        !cached_recipes_[recipe].contains("backends") ||
        !cached_recipes_[recipe]["backends"].contains(backend)) {
        return;
    }

    auto& info = cached_recipes_[recipe]["backends"][backend];
    info["available"] = installed;

    if (installed) {
        // Update version and enrichment from config (no disk I/O — uses cached backend_versions_)
        auto enrichment = get_backend_enrichment(recipe, backend);
        if (!enrichment.version.empty()) info["version"] = enrichment.version;
        if (!enrichment.release_url.empty()) info["release_url"] = enrichment.release_url;
        if (!enrichment.download_filename.empty()) info["download_filename"] = enrichment.download_filename;
    } else {
        info.erase("version");
    }
}

} // namespace lemon
