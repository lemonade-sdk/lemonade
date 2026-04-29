#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <atomic>
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

    // Only fall back to llamacpp runtime version if the recipe is llamacpp
    if (recipe == "llamacpp" &&
        backend_versions.contains("llamacpp") &&
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
            p.file_index = 2;  // ROCm runtime is the second file
            p.total_files = 2;  // Backend binary + ROCm runtime
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
        p.file_index = 2;  // ROCm runtime is the second file
        p.total_files = 2;  // Backend binary + ROCm runtime
        p.bytes_downloaded = download_result.bytes_downloaded;
        p.bytes_total = download_result.total_bytes;
        p.percent = 100;
        p.complete = true;  // Now we can send the completion event
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

bool will_install_therock(const std::string& os, const json& backend_versions) {
    // TheRock is needed on Linux and Windows for ROCm preview channel.
    if (os != "linux" && os != "windows") {
        return false;
    }

    // Get TheRock version from backend_versions.json
    if (!backend_versions.contains("therock") || !backend_versions["therock"].contains("version")) {
        return false;
    }
    std::string therock_version = backend_versions["therock"]["version"].get<std::string>();
    std::string expected_rocm_version = normalize_runtime_version(therock_version);

    // Check if system ROCm matches TheRock version - if so, don't need TheRock
    if (backends::BackendUtils::is_rocm_installed_system_wide() &&
        has_matching_system_rocm_runtime(expected_rocm_version)) {
        LOG(DEBUG, "BackendManager")
            << "System ROCm " << expected_rocm_version
            << " matches TheRock version, skipping TheRock installation" << std::endl;
        return false;
    }

    // Get ROCm architecture
    std::string rocm_arch = SystemInfo::get_rocm_arch();
    if (rocm_arch.empty()) {
        return false;
    }

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
            return false;
        }
    }

    return true;
}

void install_therock_if_needed(const std::string& os, const json& backend_versions,
                              DownloadProgressCallback progress_cb = nullptr) {
    if (!will_install_therock(os, backend_versions)) {
        return;
    }

    std::string rocm_arch = SystemInfo::get_rocm_arch();
    std::string version = backend_versions["therock"]["version"].get<std::string>();

    // Install TheRock for this architecture
    backends::BackendUtils::install_therock(rocm_arch, version, progress_cb);
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

// Global accessor — mirrors RuntimeConfig::global(). Set once from Server's
// constructor; null when invoked from CLI tools that don't run a server.
static std::atomic<BackendManager*> s_global_backend_manager{nullptr};

void BackendManager::set_global(BackendManager* instance) {
    s_global_backend_manager.store(instance, std::memory_order_release);
}

BackendManager* BackendManager::global() {
    return s_global_backend_manager.load(std::memory_order_acquire);
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

std::string BackendManager::fetch_latest_github_tag(const std::string& repo,
                                                     bool throw_on_failure) {
    {
        std::lock_guard<std::mutex> lock(latest_version_cache_mutex_);
        auto it = latest_version_cache_.find(repo);
        if (it != latest_version_cache_.end()) {
            return it->second;
        }
    }

    auto* cfg = RuntimeConfig::global();
    if (cfg && cfg->no_fetch_executables()) {
        if (throw_on_failure) {
            throw std::runtime_error(
                "Cannot resolve 'latest' release for " + repo
                + ": no_fetch_executables is true");
        }
        return "";
    }
    if (cfg && cfg->offline()) {
        if (throw_on_failure) {
            throw std::runtime_error(
                "Cannot resolve 'latest' release for " + repo + ": offline mode");
        }
        return "";
    }

    const std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
    std::map<std::string, std::string> headers = {
        {"User-Agent", "lemonade"},
        {"Accept", "application/vnd.github+json"},
    };

    LOG(DEBUG, "BackendManager") << "Resolving 'latest' for " << repo << " via " << url << std::endl;
    utils::HttpResponse resp;
    try {
        resp = utils::HttpClient::get(url, headers);
    } catch (const std::exception& e) {
        if (throw_on_failure) {
            throw std::runtime_error(
                "Failed to query GitHub for latest release of " + repo + ": " + e.what());
        }
        LOG(WARNING, "BackendManager") << "GitHub query for " << repo << " failed: " << e.what() << std::endl;
        return "";
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (throw_on_failure) {
            throw std::runtime_error(
                "GitHub returned HTTP " + std::to_string(resp.status_code)
                + " when querying latest release of " + repo);
        }
        LOG(WARNING, "BackendManager") << "GitHub returned HTTP " << resp.status_code
                                       << " for " << repo << std::endl;
        return "";
    }

    std::string tag;
    try {
        auto body = json::parse(resp.body);
        tag = body.at("tag_name").get<std::string>();
    } catch (const std::exception& e) {
        if (throw_on_failure) {
            throw std::runtime_error(
                "Failed to parse GitHub release response for " + repo + ": " + e.what());
        }
        LOG(WARNING, "BackendManager") << "Failed to parse GitHub response for " << repo
                                       << ": " << e.what() << std::endl;
        return "";
    }

    {
        std::lock_guard<std::mutex> lock(latest_version_cache_mutex_);
        latest_version_cache_[repo] = tag;
    }
    LOG(INFO, "BackendManager") << "Resolved 'latest' for " << repo << " -> " << tag << std::endl;
    return tag;
}

std::string BackendManager::resolve_user_version(const std::string& recipe,
                                                  const std::string& resolved_backend,
                                                  const std::string& pinned_version,
                                                  const std::string& repo) {
    auto* cfg = RuntimeConfig::global();
    if (!cfg) return pinned_version;

    std::string section, bin_key;
    backends::BackendUtils::build_bin_config_key(recipe, resolved_backend, section, bin_key);
    std::string raw = cfg->backend_string(section, bin_key);

    // "" / "builtin" → use lemonade's pinned version.
    if (raw.empty() || raw == "builtin") return pinned_version;

    // Path overrides skip the install flow entirely (find_external_backend_binary
    // returns the path), so the version doesn't matter — fall back to pinned for
    // any UI/metadata callers that still touch this code path.
    if (utils::looks_like_path(raw)) return pinned_version;

    // "latest" → ask GitHub. If offline and a previously-installed version is
    // recorded, reuse it instead of failing.
    if (raw == "latest") {
        if (cfg->offline()) {
            std::string install_dir = backends::BackendUtils::get_install_directory(
                recipe, resolved_backend);
            std::string cached = read_version_file(fs::path(install_dir) / "version.txt");
            if (!cached.empty()) {
                LOG(WARNING, "BackendManager") << "offline: reusing installed " << recipe
                                               << ":" << resolved_backend << " version "
                                               << cached << " for 'latest'" << std::endl;
                return cached;
            }
            throw std::runtime_error(
                "Cannot resolve 'latest' for " + recipe + ":" + resolved_backend
                + ": offline mode and no installed version found");
        }
        return fetch_latest_github_tag(repo, /*throw_on_failure=*/true);
    }

    // Otherwise: a bare upstream release tag (e.g. "b8664"). Pass through.
    return raw;
}

std::string BackendManager::get_or_resolve_latest_tag(const std::string& recipe,
                                                       const std::string& backend) {
    try {
        std::string resolved_backend = normalize_backend_name(recipe, backend);
        auto* spec = backends::try_get_spec_for_recipe(recipe);
        if (!spec || !spec->install_params_fn) return "";
        std::string pinned = get_version_from_config(recipe, resolved_backend);
        // First call extracts the repo for this (recipe, backend); filename
        // templating is discarded.
        auto pinned_params = spec->install_params_fn(resolved_backend, pinned);
        return fetch_latest_github_tag(pinned_params.repo, /*throw_on_failure=*/false);
    } catch (const std::exception& e) {
        LOG(WARNING, "BackendManager") << "get_or_resolve_latest_tag(" << recipe
                                       << ":" << backend << ") failed: " << e.what() << std::endl;
        return "";
    }
}

BackendManager::InstallParams BackendManager::get_install_params(const std::string& recipe, const std::string& backend) {
    std::string resolved_backend = normalize_backend_name(recipe, backend);

    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }
    std::string pinned = get_version_from_config(recipe, resolved_backend);

    if (!spec->install_params_fn) {
        throw std::runtime_error("No install params function for recipe: " + recipe);
    }

    // Two-pass: first call gets the repo for resolve_user_version (filename
    // discarded); second call templates the resolved tag into the real filename.
    auto pinned_params = spec->install_params_fn(resolved_backend, pinned);
    std::string resolved_version = resolve_user_version(
        recipe, resolved_backend, pinned, pinned_params.repo);
    auto final_params = spec->install_params_fn(resolved_backend, resolved_version);
    return {final_params.repo, final_params.filename, resolved_version};
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

    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->no_fetch_executables()) {
            throw std::runtime_error(
                "Fetching executable artifacts is disabled");
        }
    }

    auto params = get_install_params(recipe, resolved_backend);
    auto* spec = backends::try_get_spec_for_recipe(recipe);
    if (!spec) {
        throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
    }

    // Check if we need to install additional runtime components after the main backend
    bool needs_rocm_stable_runtime = (recipe == "llamacpp" || recipe == "sd-cpp") &&
                                      resolved_backend == "rocm-stable" &&
                                      get_current_os() == "linux";

    bool needs_therock = (recipe == "llamacpp" || recipe == "sd-cpp") &&
                         resolved_backend == "rocm-preview" &&
                         will_install_therock(get_current_os(), backend_versions_);

    const bool runtime_install_may_follow = needs_rocm_stable_runtime || needs_therock;

    // Roll back only brand-new backend dirs if a required runtime fails.
    const std::string backend_install_dir =
        backends::BackendUtils::get_install_directory(spec->recipe, resolved_backend);
    const bool backend_install_dir_existed_before = fs::exists(backend_install_dir);

    // Track whether callers already received a terminal completion event.
    bool completion_reported = false;
    bool have_last_progress = false;
    DownloadProgress last_progress;

    // Report backend progress as file 1/2 when a runtime may follow.
    DownloadProgressCallback wrapped_progress_cb;
    if (progress_cb) {
        wrapped_progress_cb = [progress_cb,
                               runtime_install_may_follow,
                               &completion_reported,
                               &have_last_progress,
                               &last_progress](const DownloadProgress& p) -> bool {
            DownloadProgress adjusted = p;
            if (runtime_install_may_follow) {
                // Final completion belongs to the runtime path.
                adjusted.file_index = 1;
                adjusted.total_files = 2;
                if (p.complete) {
                    adjusted.complete = false;
                }
            }
            if (adjusted.complete) {
                completion_reported = true;
            }
            last_progress = adjusted;
            have_last_progress = true;
            return progress_cb(adjusted);
        };
    }

    try {
        backends::BackendUtils::install_from_github(
            *spec, params.version, params.repo, params.filename, resolved_backend, wrapped_progress_cb);

        DownloadProgressCallback runtime_progress_cb;
        if (progress_cb) {
            runtime_progress_cb = [progress_cb,
                                   &completion_reported,
                                   &have_last_progress,
                                   &last_progress](const DownloadProgress& p) -> bool {
                if (p.complete) {
                    completion_reported = true;
                }
                last_progress = p;
                have_last_progress = true;
                return progress_cb(p);
            };
        }

        if (needs_rocm_stable_runtime) {
            install_rocm_stable_runtime_if_needed(get_current_os(), *spec, backend_versions_, runtime_progress_cb);
        }

        if (needs_therock) {
            install_therock_if_needed(get_current_os(), backend_versions_, runtime_progress_cb);
        }

        if (progress_cb && runtime_install_may_follow && !completion_reported) {
            DownloadProgress complete_progress;
            if (have_last_progress) {
                complete_progress = last_progress;
            }
            // Runtime emitted no progress; send a zero-byte final event.
            complete_progress.file = "runtime";
            complete_progress.file_index = 2;
            complete_progress.total_files = 2;
            complete_progress.bytes_downloaded = 0;
            complete_progress.bytes_total = 0;
            complete_progress.percent = 100;
            complete_progress.complete = true;
            progress_cb(complete_progress);
        }
    } catch (...) {
        if (!backend_install_dir_existed_before) {
            std::error_code cleanup_ec;
            fs::remove_all(backend_install_dir, cleanup_ec);
        }
        throw;
    }
}

void BackendManager::uninstall_backend(const std::string& recipe, const std::string& backend) {
    std::string resolved_backend = normalize_backend_name(recipe, backend);
    LOG(DEBUG, "BackendManager") << "Uninstalling " << recipe << ":" << resolved_backend << std::endl;

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
        auto params = get_install_params(recipe, resolved_backend);
        return "https://github.com/" + params.repo + "/releases/tag/" + params.version;
    } catch (...) {
        return "";
    }
}

std::string BackendManager::get_download_filename(const std::string& recipe, const std::string& backend) {
    try {
        std::string resolved_backend = normalize_backend_name(recipe, backend);
        auto params = get_install_params(recipe, resolved_backend);
        return params.filename;
    } catch (...) {
        return "";
    }
}

BackendManager::BackendEnrichment BackendManager::get_backend_enrichment(const std::string& recipe, const std::string& backend) {
    BackendEnrichment result;
    try {
        std::string resolved_backend = normalize_backend_name(recipe, backend);
        // All standard recipes (including ryzenai-llm): one get_install_params() call gives us everything
        auto params = get_install_params(recipe, resolved_backend);
        result.release_url = "https://github.com/" + params.repo + "/releases/tag/" + params.version;
        result.download_filename = params.filename;
        result.version = params.version;
    } catch (...) {}
    return result;
}

} // namespace lemon
