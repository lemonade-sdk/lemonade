#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

namespace {

static const char* ROCM_STABLE_RUNTIME_DIR = "rocm-stable-runtime";

std::string get_current_os() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string normalize_backend_name(const std::string& recipe, const std::string& backend) {
    if ((recipe == "llamacpp" || recipe == "sd-cpp") && backend == "rocm") {
        // Map "rocm" to the appropriate channel based on config
        std::string channel = "preview";  // default to preview for now
        if (auto* cfg = RuntimeConfig::global()) {
            channel = cfg->rocm_channel();
        }
        return "rocm-" + channel;
    }
    return backend;
}

std::string get_backend_runtime_version(const json& backend_versions,
                                        const std::string& recipe,
                                        const std::string& backend_type) {
    const std::string runtime_key = backend_type + "-runtime";

    if (backend_versions.contains(runtime_key) &&
        backend_versions[runtime_key].is_string()) {
        return backend_versions[runtime_key].get<std::string>();
    }

    if (backend_versions.contains(recipe) &&
        backend_versions[recipe].is_object() &&
        backend_versions[recipe].contains(runtime_key) &&
        backend_versions[recipe][runtime_key].is_string()) {
        return backend_versions[recipe][runtime_key].get<std::string>();
    }

    if (backend_versions.contains("llamacpp") &&
        backend_versions["llamacpp"].is_object() &&
        backend_versions["llamacpp"].contains(runtime_key) &&
        backend_versions["llamacpp"][runtime_key].is_string()) {
        return backend_versions["llamacpp"][runtime_key].get<std::string>();
    }

    throw std::runtime_error("backend_versions.json is missing runtime version for: " + recipe + ":" + runtime_key);
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string normalize_runtime_version(const std::string& version) {
    std::string normalized = trim(version);
    if (!normalized.empty() && normalized[0] == 'v') {
        normalized.erase(0, 1);
    }
    return normalized;
}

bool runtime_version_matches_expected(const std::string& discovered_version,
                                      const std::string& expected_version) {
    const std::string discovered = normalize_runtime_version(discovered_version);
    const std::string expected = normalize_runtime_version(expected_version);

    if (discovered.empty() || expected.empty()) {
        return false;
    }

    if (discovered == expected) {
        return true;
    }

    return discovered.rfind(expected + ".", 0) == 0;
}

std::string read_version_file(const fs::path& version_file) {
    if (!fs::exists(version_file)) {
        return "";
    }

    std::ifstream file(version_file);
    if (!file.is_open()) {
        return "";
    }

    std::string version;
    std::getline(file, version);
    return trim(version);
}

bool has_matching_system_rocm_runtime(const std::string& expected_runtime_version) {
    const fs::path version_file = "/opt/rocm/.info/version";
    const std::string system_version = read_version_file(version_file);
    return runtime_version_matches_expected(system_version, expected_runtime_version);
}

bool has_matching_installed_rocm_stable_runtime(const std::string& install_dir,
                                                const std::string& expected_runtime_version) {
    const fs::path info_version_file = fs::path(install_dir) / ".info" / "version";
    const std::string installed_runtime_version = read_version_file(info_version_file);
    return runtime_version_matches_expected(installed_runtime_version, expected_runtime_version);
}

std::string get_rocm_stable_runtime_asset_filename(const std::string& version) {
    std::string normalized_version = normalize_runtime_version(version);
    if (normalized_version.empty()) {
        throw std::runtime_error("Invalid ROCm stable runtime version in backend_versions.json");
    }
    return "rocm-" + normalized_version + "-runtime-libs.tar.gz";
}

void install_rocm_stable_runtime_if_needed(const std::string& os,
                                           const backends::BackendSpec& spec,
                                           const json& backend_versions,
                                           DownloadProgressCallback progress_cb) {
    // ROCm stable runtime is only needed on Linux.
    // On Windows, the llama.cpp HIP binaries are self-contained.
    if (os != "linux") {
        return;
    }
    const std::string backend_type = "rocm-stable";
    const std::string version = get_backend_runtime_version(backend_versions, spec.recipe, backend_type);
    const std::string repo = "lemonade-sdk/rocm-stable";
    const std::string filename = get_rocm_stable_runtime_asset_filename(version);
    const std::string install_dir = backends::BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
    const std::string expected_runtime_version = normalize_runtime_version(version);

    if (has_matching_system_rocm_runtime(expected_runtime_version)) {
        LOG(DEBUG, "BackendManager")
            << "Detected compatible system ROCm runtime version at /opt/rocm/.info/version: "
            << expected_runtime_version << std::endl;
        return;
    }

    if (has_matching_installed_rocm_stable_runtime(install_dir, expected_runtime_version)) {
        LOG(DEBUG, "BackendManager")
            << "Detected compatible bundled ROCm stable runtime version in "
            << install_dir << std::endl;
        return;
    }

    fs::remove_all(install_dir);
    fs::create_directories(install_dir);

    const std::string url = "https://github.com/" + repo + "/releases/download/" + version + "/" + filename;
    std::string archive_basename = filename;
    for (char& ch : archive_basename) {
        if (ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    const std::string archive_path = (fs::temp_directory_path() /
        ("llamacpp_rocm_stable_runtime_" + version + "_" + archive_basename)).string();

    // Remove any stale archive from a previous failed download. The HTTP downloader
    // supports resume-by-default, which is undesirable here because older attempts
    // may have cached an HTML error page under the same temp filename.
    std::error_code archive_ec;
    fs::remove(archive_path, archive_ec);

    utils::ProgressCallback http_progress_cb;
    if (progress_cb) {
        http_progress_cb = [&progress_cb, &filename](size_t downloaded, size_t total) -> bool {
            DownloadProgress p;
            p.file = filename;
            p.file_index = 1;
            p.total_files = 1;
            p.bytes_downloaded = downloaded;
            p.bytes_total = total;
            p.percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
            p.complete = false;
            return progress_cb(p);
        };
    } else {
        http_progress_cb = utils::create_throttled_progress_callback();
    }

    auto download_result = utils::HttpClient::download_file(url, archive_path, http_progress_cb);
    if (!download_result.success) {
        throw std::runtime_error("Failed to download ROCm stable runtime from: " + url +
                                 " - " + download_result.error_message);
    }

    if (!backends::BackendUtils::extract_archive(archive_path, install_dir, spec.log_name())) {
        fs::remove(archive_path);
        fs::remove_all(install_dir);
        throw std::runtime_error("Failed to extract ROCm stable runtime archive: " + archive_path);
    }

    fs::remove(archive_path);

    if (progress_cb) {
        DownloadProgress p;
        p.file = filename;
        p.file_index = 1;
        p.total_files = 1;
        p.bytes_downloaded = download_result.bytes_downloaded;
        p.bytes_total = download_result.total_bytes;
        p.percent = 100;
        p.complete = true;
        progress_cb(p);
    }
}

void uninstall_rocm_stable_runtime_if_needed(const std::string& os) {
    // ROCm stable runtime is only used on Linux.
    // On Windows, the llama.cpp HIP binaries are self-contained.
    if (os != "linux") {
        return;
    }
    std::string runtime_dir = backends::BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
    if (fs::exists(runtime_dir)) {
        std::error_code ec;
        fs::remove_all(runtime_dir, ec);
        if (ec && fs::exists(runtime_dir)) {
            throw std::runtime_error("Failed to remove " + runtime_dir + ": " + ec.message());
        }
    }
}

void install_therock_if_needed(const std::string& os, const json& backend_versions) {
    // TheRock is only needed on Linux for ROCm preview channel.
    if (os != "linux") {
        return;
    }

    // Check if system ROCm is available - if so, don't need TheRock
    if (backends::BackendUtils::is_rocm_installed_system_wide()) {
        LOG(DEBUG, "BackendManager")
            << "System ROCm detected, skipping TheRock installation" << std::endl;
        return;
    }

    // Get ROCm architecture
    std::string rocm_arch = SystemInfo::get_rocm_arch();
    if (rocm_arch.empty()) {
        LOG(DEBUG, "BackendManager")
            << "No ROCm architecture detected, skipping TheRock installation" << std::endl;
        return;
    }

    // Get TheRock version from backend_versions.json
    if (!backend_versions.contains("therock") || !backend_versions["therock"].contains("version")) {
        throw std::runtime_error("backend_versions.json is missing 'therock.version'");
    }
    std::string version = backend_versions["therock"]["version"].get<std::string>();

    // Check if this architecture is supported
    if (backend_versions["therock"].contains("architectures") &&
        backend_versions["therock"]["architectures"].is_array()) {
        bool arch_supported = false;
        for (const auto& arch : backend_versions["therock"]["architectures"]) {
            if (arch.is_string() && arch.get<std::string>() == rocm_arch) {
                arch_supported = true;
                break;
            }
        }
        if (!arch_supported) {
            LOG(DEBUG, "BackendManager")
                << "Architecture " << rocm_arch << " not supported by TheRock" << std::endl;
            return;
        }
    }

    // Install TheRock for this architecture
    backends::BackendUtils::install_therock(rocm_arch, version);
}

} // namespace

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
    std::string resolved_backend = normalize_backend_name(recipe, backend);

    // The "system" backend doesn't have a version in backend_versions.json
    // because it uses a pre-installed binary from the system PATH
    if (resolved_backend == "system") {
        return "";
    }

    if (!backend_versions_.contains(recipe) || !backend_versions_[recipe].is_object()) {
        throw std::runtime_error("backend_versions.json is missing '" + recipe + "' section");
    }
    const auto& recipe_config = backend_versions_[recipe];
    if (!recipe_config.contains(resolved_backend) || !recipe_config[resolved_backend].is_string()) {
        throw std::runtime_error("backend_versions.json is missing version for: " + recipe + ":" + resolved_backend);
    }
    return recipe_config[resolved_backend].get<std::string>();
}

// ============================================================================
// Core operations
// ============================================================================

// ============================================================================
// Install parameters
// ============================================================================

BackendManager::InstallParams BackendManager::get_install_params(const std::string& recipe, const std::string& backend) {
    std::string resolved_backend = normalize_backend_name(recipe, backend);

    if (recipe == "flm") {
        throw std::runtime_error("FLM uses a special installer and cannot be installed via get_install_params");
    }

    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }
    std::string version = get_version_from_config(recipe, resolved_backend);

    if (!spec->install_params_fn) {
        throw std::runtime_error("No install params function for recipe: " + recipe);
    }

    auto params = spec->install_params_fn(resolved_backend, version);
    return {params.repo, params.filename, version};
}

void BackendManager::install_backend(const std::string& recipe, const std::string& backend,
                                     bool force,
                                     DownloadProgressCallback progress_cb) {
    std::string resolved_backend = normalize_backend_name(recipe, backend);
    LOG(DEBUG, "BackendManager") << "Installing " << recipe << ":" << resolved_backend << std::endl;

    // System backend uses a pre-installed binary from PATH - nothing to install
    if (resolved_backend == "system") {
        return;
    }

    // FLM special case - uses installer exe with its own install logic
    if (recipe == "flm") {
        auto status = SystemInfoCache::get_flm_status();
        if (status.state == "installed") {
            // Already installed — nothing to do
        } else if (status.state == "unsupported" && !force) {
            throw std::runtime_error("FLM is not supported on this system: " + status.message);
        } else {
            // installable, update_required, or action_required
            backends::FastFlowLMServer flm_installer("info", nullptr, this);
            flm_installer.install(backend);
            // install() calls SystemInfoCache::invalidate_recipes()
        }
        // Re-read status after install
        status = SystemInfoCache::get_flm_status();
        if (!status.is_ready() && !force) {
            throw std::runtime_error("FLM installation incomplete: " + status.message +
                (status.action.empty() ? "" : ". " + status.action));
        }
        return;
    }

    auto params = get_install_params(recipe, resolved_backend);
    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }

    backends::BackendUtils::install_from_github(
        *spec, params.version, params.repo, params.filename, resolved_backend, progress_cb);

    if ((recipe == "llamacpp" || recipe == "sd-cpp") && resolved_backend == "rocm-stable") {
        install_rocm_stable_runtime_if_needed(get_current_os(), *spec, backend_versions_, progress_cb);
    }

    if (recipe == "sd-cpp" && resolved_backend == "rocm-preview") {
        install_therock_if_needed(get_current_os(), backend_versions_);
    }
}

void BackendManager::uninstall_backend(const std::string& recipe, const std::string& backend) {
    std::string resolved_backend = normalize_backend_name(recipe, backend);
    LOG(DEBUG, "BackendManager") << "Uninstalling " << recipe << ":" << resolved_backend << std::endl;

    if (recipe == "flm") {
        throw std::runtime_error("Uninstall FastFlowLM using their Windows uninstaller.");
    }

    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }

    std::string install_dir = backends::BackendUtils::get_install_directory(spec->recipe, resolved_backend);

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

    if (recipe == "llamacpp" && resolved_backend == "rocm-stable") {
        uninstall_rocm_stable_runtime_if_needed(get_current_os());
    }

}

// ============================================================================
// Query operations
// ============================================================================

std::string BackendManager::get_latest_version(const std::string& recipe, const std::string& backend) {
    try {
        return get_version_from_config(recipe, normalize_backend_name(recipe, backend));
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
        std::string resolved_backend = normalize_backend_name(recipe, backend);

        if (recipe == "flm") {
            std::string version = get_latest_version(recipe, resolved_backend);
            if (!version.empty()) {
                return "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + version;
            }
            return "";
        }

        auto params = get_install_params(recipe, resolved_backend);
        return "https://github.com/" + params.repo + "/releases/tag/" + params.version;
    } catch (...) {
        return "";
    }
}

std::string BackendManager::get_download_filename(const std::string& recipe, const std::string& backend) {
    try {
        auto params = get_install_params(recipe, normalize_backend_name(recipe, backend));
        return params.filename;
    } catch (...) {
        return "";
    }
}

BackendManager::BackendEnrichment BackendManager::get_backend_enrichment(const std::string& recipe, const std::string& backend) {
    BackendEnrichment result;
    try {
        std::string resolved_backend = normalize_backend_name(recipe, backend);

        if (recipe == "flm") {
            result.version = get_latest_version(recipe, resolved_backend);
            if (!result.version.empty()) {
                result.release_url = "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + result.version;
            }
            // FLM installer artifact used by install_flm_if_needed().
            result.download_filename = "flm-setup.exe";
            return result;
        }

        // All standard recipes (including ryzenai-llm): one get_install_params() call gives us everything
        auto params = get_install_params(recipe, resolved_backend);
        result.release_url = "https://github.com/" + params.repo + "/releases/tag/" + params.version;
        result.download_filename = params.filename;
        result.version = params.version;
    } catch (...) {}
    return result;
}

} // namespace lemon
