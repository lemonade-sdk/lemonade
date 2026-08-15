#include <lemon/utils/path_utils.h>
#include <lemon/utils/path_platform.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/process_manager.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon::utils {

// ---------------------------------------------------------------------------
// Lemonade cache dir and models dir — set once at startup before any
// concurrent access, then read-only from that point on.
// ---------------------------------------------------------------------------

static std::string g_cache_dir;
static std::string g_models_dir;

// Platform abstraction instance (created on first use)
static PathPlatform* platform() {
    static std::unique_ptr<PathPlatform> p = create_path_platform();
    return p.get();
}

void set_cache_dir(const std::string& dir) {
    g_cache_dir = dir;
}

void set_models_dir(const std::string& dir) {
    g_models_dir = dir;
}

std::string get_environment_variable_utf8(const std::string& name) {
    return platform()->get_environment_variable_utf8(name);
}

fs::path path_from_utf8(const std::string& path) {
    return platform()->path_from_utf8(path);
}

std::string path_to_utf8(const fs::path& path) {
    return platform()->path_to_utf8(path);
}

std::string get_executable_dir() {
    return platform()->get_executable_dir();
}

std::string get_resource_path(const std::string& relative_path) {
    fs::path exe_dir = get_executable_dir();
    fs::path resource_path = exe_dir / relative_path;

    // Check if resource exists next to executable (for dev builds)
    if (fs::exists(resource_path)) {
        return resource_path.string();
    }

    // Check platform-specific install locations
    std::vector<std::string> install_prefixes = platform()->get_install_prefixes();
    for (const auto& prefix : install_prefixes) {
        fs::path installed_path = fs::path(prefix) / relative_path;
        if (fs::exists(installed_path)) {
            return installed_path.string();
        }
    }

    // Fallback: return original path (will fail but with clear error)
    return resource_path.string();
}

bool is_safe_executable_path(const std::string& path) {
    for (char c : path) {
        // Allow typical path characters: alphanumeric, path separators, dots,
        // hyphens, underscores, spaces, colons (drive letters), parens (Program Files (x86))
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        switch (c) {
            case '/': case '\\': case '.': case '-': case '_':
            case ' ': case ':': case '(': case ')': case '~':
                continue;
            default:
                return false;
        }
    }
    return !path.empty();
}

bool looks_like_path(const std::string& v) {
    try {
        return fs::path(v).is_absolute();
    } catch (const std::exception&) {
        return false;
    }
}


std::string find_executable_in_path(const std::string& executable_name) {
    if (!is_safe_executable_path(executable_name)) {
        return "";
    }
#ifdef _WIN32
    char found_path[MAX_PATH];
    DWORD result = SearchPathA(
        nullptr,      // Use system PATH
        executable_name.c_str(), // File to search for
        nullptr,      // No default extension needed
        MAX_PATH,
        found_path,
        nullptr
    );

    if (result > 0 && result < MAX_PATH) {
        std::string path(found_path);
        return is_safe_executable_path(path) ? path : "";
    }

    return "";
#else
    // Walk PATH ourselves instead of shelling out to `which`. Minimal Fedora /
    // openSUSE containers (and other slimmed-down environments) do not ship
    // `which`, and even when they do, system() forks a shell which inherits
    // the process's PATH — so this approach is both more portable and more
    // efficient.
    const char* path_env = std::getenv("PATH");
    if (!path_env || *path_env == '\0') {
        return "";
    }
    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string dir = path_str.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            std::error_code ec;
            fs::path candidate = fs::path(dir) / executable_name;
            if (fs::is_regular_file(candidate, ec) &&
                (access(candidate.c_str(), X_OK) == 0)) {
                std::string full = candidate.string();
                return is_safe_executable_path(full) ? executable_name : "";
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
#endif
}


std::string get_cache_dir() {
    // If set_cache_dir() was called at startup, use that
    if (!g_cache_dir.empty()) {
        return g_cache_dir;
    }

    // Check LEMONADE_CACHE_DIR environment variable
    std::string env_cache_dir = get_environment_variable_utf8("LEMONADE_CACHE_DIR");
    if (!env_cache_dir.empty()) {
        return env_cache_dir;
    }

    // Fallback to platform-specific defaults (for backward compat / CLI client)
    return platform()->get_cache_dir(g_cache_dir);
}

std::string default_hf_cache_dir() {
    return platform()->default_hf_cache_dir();
}

std::string resolve_hf_cache_dir() {
    // Follow the HuggingFace spec for cache directory resolution:
    // 1. HF_HUB_CACHE — direct path to the hub cache
    // 2. HF_HOME — base HF directory; cache is at $HF_HOME/hub
    // 3. Platform-specific default (~/.cache/huggingface/hub)
    std::string hf_hub_cache = get_environment_variable_utf8("HF_HUB_CACHE");
    if (!hf_hub_cache.empty()) {
        return hf_hub_cache;
    }
    std::string hf_home = get_environment_variable_utf8("HF_HOME");
    if (!hf_home.empty()) {
#ifdef _WIN32
        return hf_home + "\\hub";
#else
        return hf_home + "/hub";
#endif
    }
    return default_hf_cache_dir();
}

std::string get_hf_cache_dir() {
    if (!g_models_dir.empty() && g_models_dir != "auto") {
        fs::path p = path_from_utf8(g_models_dir);
        if (p.is_relative()) {
            p = path_from_utf8(get_executable_dir()) / p;
        }
        return path_to_utf8(p);
    }
    return resolve_hf_cache_dir();
}

std::string get_runtime_dir() {
    return platform()->get_runtime_dir();
}

std::string get_downloaded_bin_dir() {
    // Use cache directory on all platforms for consistent multi-user support
    // This is important for All Users installs on Windows where Program Files is read-only
    // Use fs::path to ensure native path separators (avoids cmd.exe issues on Windows)
    std::string bin_dir = (fs::path(get_cache_dir()) / "bin").make_preferred().string();

    // Ensure directory exists
    fs::path bin_path = path_from_utf8(bin_dir);
    if (!fs::exists(bin_path)) {
        fs::create_directories(bin_path);
    }

    return bin_dir;
}

bool atomic_replace_file(const fs::path& src, const fs::path& dest, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    if (MoveFileExW(src.c_str(), dest.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::rename(src, dest, ec);
    if (!ec) return true;
#endif
    std::error_code copy_ec;
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
        ec = copy_ec;
        std::error_code rm_ec;
        fs::remove(src, rm_ec);
        return false;
    }
    ec.clear();
    std::error_code rm_ec;
    fs::remove(src, rm_ec);
    return true;
}


} // namespace utils::lemon
