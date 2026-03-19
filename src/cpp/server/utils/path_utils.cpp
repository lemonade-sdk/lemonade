#include <lemon/utils/path_utils.h>
#include <lemon/utils/json_utils.h>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#ifdef __APPLE__
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace lemon::utils {

#ifdef _WIN32
static std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size_needed <= 0) {
        return std::wstring();
    }

    std::wstring result(size_needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size_needed);
    result.resize(size_needed - 1);
    return result;
}

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) {
        return std::string();
    }

    std::string result(size_needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size_needed, nullptr, nullptr);
    result.resize(size_needed - 1);
    return result;
}
#endif

std::string get_environment_variable_utf8(const std::string& name) {
#ifdef _WIN32
    std::wstring wide_name = utf8_to_wstring(name);
    DWORD size_needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size_needed == 0) {
        return "";
    }

    std::wstring value(size_needed, L'\0');
    GetEnvironmentVariableW(wide_name.c_str(), &value[0], size_needed);
    value.resize(size_needed - 1);
    return wstring_to_utf8(value);
#else
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : "";
#endif
}

fs::path path_from_utf8(const std::string& path) {
#ifdef _WIN32
    return fs::u8path(path);
#else
    return fs::path(path);
#endif
}

std::string path_to_utf8(const fs::path& path) {
#ifdef _WIN32
    return wstring_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

std::string get_executable_dir() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exe_path(buffer);
    return exe_path.parent_path().string();
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        fs::path exe_path(buffer);
        return exe_path.parent_path().string();
    }
    // Fallback: return current directory
    return ".";
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        fs::path exe_path(buffer);
        return exe_path.parent_path().string();
    }
    // Fallback: return current directory
    return ".";
#else
    // Generic Unix fallback
    return ".";
#endif
}

std::string get_resource_path(const std::string& relative_path) {
    fs::path exe_dir = get_executable_dir();
    fs::path resource_path = exe_dir / relative_path;

    // Check if resource exists next to executable (for dev builds)
    if (fs::exists(resource_path)) {
        return resource_path.string();
    }

#ifndef _WIN32
    // On Linux/macOS, also check standard install locations
    std::vector<std::string> install_prefixes = {
        "/Library/Application Support/Lemonade",  // macOS system install location
        "/usr/local/share/lemonade-server",
        "/opt/share/lemonade-server",
        "/usr/share/lemonade-server"
    };


    // Also check user's local install directory
    const char* home = std::getenv("HOME");
    if (home) {
        std::string home_local = std::string(home) + "/.local/share/lemonade-server";
        install_prefixes.insert(install_prefixes.begin(), home_local);
    }

    for (const auto& prefix : install_prefixes) {
        fs::path installed_path = fs::path(prefix) / relative_path;
        if (fs::exists(installed_path)) {
            return installed_path.string();
        }
    }
#endif

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

std::string find_flm_executable() {
#ifdef _WIN32
    // Refresh PATH from Windows registry to pick up any changes since process started
    // This is important because users may install FLM after starting lemonade-server
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[32767];
        DWORD bufferSize = sizeof(buffer);
        if (RegQueryValueExA(hKey, "PATH", nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(buffer), &bufferSize) == ERROR_SUCCESS) {
            std::string system_path = buffer;
            // Combine with current process PATH (system PATH takes priority for FLM lookup)
            const char* current_path = std::getenv("PATH");
            if (current_path) {
                system_path = system_path + ";" + std::string(current_path);
            }
            _putenv(("PATH=" + system_path).c_str());
        }
        RegCloseKey(hKey);
    }

    // Use SearchPathA which is the same API that CreateProcessA uses internally
    // This ensures we find the exact same executable that will be launched
    char found_path[MAX_PATH];
    DWORD result = SearchPathA(
        nullptr,      // Use system PATH
        "flm.exe",    // File to search for
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
    // On Linux/Mac, check PATH using which
    if (system("which flm > /dev/null 2>&1") == 0) {
        return "flm";
    }
    return "";
#endif
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
    // On Linux/Mac, check PATH using which
    std::string command = "which " + executable_name + " > /dev/null 2>&1";
    if (system(command.c_str()) == 0) {
        return executable_name; // Return the executable name itself, relying on PATH for execution
    }
    return "";
#endif
}

bool is_ggml_hip_plugin_available() {
#ifdef __linux__
    // On Linux x86_64, check common system library paths for the HIP plugin
    std::vector<std::string> possible_paths = {
        // Debian/Ubuntu multiarch path (most common)
        "/usr/lib/x86_64-linux-gnu/ggml/backends0/libggml-hip.so",
        // Standard Linux paths
        "/usr/lib/ggml/backends0/libggml-hip.so",
        "/usr/lib64/ggml/backends0/libggml-hip.so"
    };

    // Check all possible paths
    for (const auto& path : possible_paths) {
        if (fs::exists(path)) {
            return true;
        }
    }
#endif

    return false;
}

std::string get_cache_dir() {
    std::string cache_dir_env = get_environment_variable_utf8("LEMONADE_CACHE_DIR");
    if (!cache_dir_env.empty()) {
        std::string cache_dir = cache_dir_env;
#ifdef __APPLE__
        // Ensure directory exists on macOS
        fs::path cache_path = path_from_utf8(cache_dir);
        if (!fs::exists(cache_path)) {
            fs::create_directories(cache_path);
        }
#endif
        return cache_dir;
    }

#ifdef _WIN32
    std::string userprofile = get_environment_variable_utf8("USERPROFILE");
    if (!userprofile.empty()) {
        return userprofile + "\\.cache\\lemonade";
    }
#elif defined(__APPLE__)
    // Check if we are running as root (UID 0)
    if (geteuid() != 0) {
        // --- NORMAL USER MODE ---
        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            std::string cache_dir = home + "/.cache/lemonade";
            // Ensure directory exists
            fs::path cache_path = path_from_utf8(cache_dir);
            if (!fs::exists(cache_path)) {
                fs::create_directories(cache_path);
            }
            return cache_dir;
        }
        // Fallback if HOME is missing but we aren't root
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            std::string cache_dir = std::string(pw->pw_dir) + "/.cache/lemonade";
            // Ensure directory exists
            fs::path cache_path = path_from_utf8(cache_dir);
            if (!fs::exists(cache_path)) {
                fs::create_directories(cache_path);
            }
            return cache_dir;
        }
    }

    // --- SYSTEM SERVICE / ROOT MODE ---
    // If we are root (or getting HOME failed), use a shared system location.
    // /Users/Shared is okay, but /Library/Application Support is the standard macOS system path.
    std::string cache_dir = "/Library/Application Support/lemonade/.cache";
    // Ensure directory exists
    fs::path cache_path = path_from_utf8(cache_dir);
    if (!fs::exists(cache_path)) {
        fs::create_directories(cache_path);
    }
    return cache_dir;

#else
    // Linux and other Unix systems
    std::string home = get_environment_variable_utf8("HOME");
    if (!home.empty()) {
        return home + "/.cache/lemonade";
    }
    #endif

    return ".cache/lemonade";
}

std::string get_hf_cache_dir() {
    // Check HF_HUB_CACHE first (highest priority)
    std::string hf_hub_cache_env = get_environment_variable_utf8("HF_HUB_CACHE");
    if (!hf_hub_cache_env.empty()) {
        return hf_hub_cache_env;
    }

    // Check HF_HOME second (append /hub)
    std::string hf_home_env = get_environment_variable_utf8("HF_HOME");
    if (!hf_home_env.empty()) {
        return hf_home_env + "/hub";
    }

    // Default platform-specific paths
#ifdef _WIN32
    std::string userprofile = get_environment_variable_utf8("USERPROFILE");
    if (!userprofile.empty()) {
        return userprofile + "\\.cache\\huggingface\\hub";
    }
    return "C:\\.cache\\huggingface\\hub";
#else
    std::string home = get_environment_variable_utf8("HOME");
    if (!home.empty()) {
        return home + "/.cache/huggingface/hub";
    }
    return "/tmp/.cache/huggingface/hub";
#endif
}

std::string get_runtime_dir() {
#ifdef _WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    return std::string(temp_path);
#else
    // Use $XDG_RUNTIME_DIR/lemonade only when the base directory is set,
    // actually exists on disk, and is writable by the current process.
    // This guards against CI environments, containers, or minimal systems
    // where the variable might be set but the directory is absent/unwritable.
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') {
        std::error_code ec;
        fs::path base(xdg);
        if (fs::is_directory(base, ec) && !ec && access(xdg, W_OK) == 0) {
            fs::path lemon_dir = base / "lemonade";
            ec.clear();
            fs::create_directory(lemon_dir, ec);
            // Treat "already exists as a directory" as success: some platforms
            // set ec to EEXIST even though the standard says they shouldn't.
            std::error_code ec2;
            if (!ec || fs::is_directory(lemon_dir, ec2)) {
                return lemon_dir.string();
            }
        }
    }
    // Fallback: /tmp for CI runners and systems without XDG session support
    return "/tmp";
#endif
}

std::string get_downloaded_bin_dir() {
    // Use cache directory on all platforms for consistent multi-user support
    // This is important for All Users installs on Windows where Program Files is read-only
    std::string bin_dir = get_cache_dir() + "/bin";

    // Ensure directory exists
    fs::path bin_path = path_from_utf8(bin_dir);
    if (!fs::exists(bin_path)) {
        fs::create_directories(bin_path);
    }

    return bin_dir;
}

bool run_flm_validate(const std::string& flm_path, std::string& error_message) {
    FILE* pipe;

    std::string flm_exe = flm_path.empty() ? find_flm_executable() : flm_path;
    if (flm_exe.empty()) {
        error_message = "FLM executable not found";
        return false;
    }
    if (!is_safe_executable_path(flm_exe)) {
        error_message = "FLM path contains invalid characters";
        return false;
    }

    std::string command = "\"" + flm_exe + "\" validate --json";
#ifdef _WIN32
    pipe = _popen(command.c_str(), "r");
#else
    pipe = popen(command.c_str(), "r");
#endif

    if (!pipe) {
        error_message = "Failed to execute " + flm_exe;
        return false;
    }

    char buffer[1024];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

#ifdef _WIN32
    int exit_code = _pclose(pipe);
#else
    int exit_code = pclose(pipe);
    if (exit_code != -1) {
        exit_code = WEXITSTATUS(exit_code);
    }
#endif

    try {
        if (!output.empty()) {
            json j = JsonUtils::parse(output);
            if (j.is_object()) {
                // Check for overall status
                bool validation_ok = false;
                if (j.contains("ready")) {
                    validation_ok = j["ready"].get<bool>();
                }

                if (validation_ok) {
                    error_message.clear();
                    return true;
                }

                std::vector<std::string> errors;

                if (j.contains("amd_device_found") && !j["amd_device_found"].get<bool>()) {
                    errors.push_back("No AMD NPU device found.");
                }

                if (j.contains("all_fw_ok") && !j["all_fw_ok"].get<bool>()) {
                    errors.push_back("NPU firmware is incompatible.");
                }
                if (j.contains("kernel_ok") && !j["kernel_ok"].get<bool>()) {
                    errors.push_back("Kernel version is incompatible.");
                }

                if (j.contains("memlock_ok") && !j["memlock_ok"].get<bool>()) {
                    errors.push_back("Memlock limits are too low.");
                }

                if (j.contains("npu_driver_ok") && !j["npu_driver_ok"].get<bool>()) {
                    errors.push_back("NPU driver version is too old.");
                }

                if (errors.empty()) {
                    error_message = "NPU validation failed.";
                } else {
                    error_message = "";
                    for (size_t i = 0; i < errors.size(); ++i) {
                        error_message += errors[i] + (i == errors.size() - 1 ? "" : " ");
                    }
                }
                return false;
            }
        }
    } catch (...) {
        // Fallback for non-JSON output or parsing error
    }

    if (exit_code != 0) {
        error_message = "flm validate failed with exit code " + std::to_string(exit_code);
        return false;
    }

    error_message.clear();
    return true;
}

} // namespace utils::lemon
