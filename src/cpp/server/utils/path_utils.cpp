#include <lemon/utils/path_utils.h>
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
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

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
        "/usr/local/share/lemonade-server",
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
        return found_path;
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

std::string get_cache_dir() {
    const char* cache_dir_env = std::getenv("LEMONADE_CACHE_DIR");
    if (cache_dir_env) {
        return std::string(cache_dir_env);
    }
    
    #ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.cache\\lemonade";
    }
    #else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/lemonade";
    }
    #endif
    
    return ".cache/lemonade";
}

std::string get_downloaded_bin_dir() {
    // Use cache directory on all platforms for consistent multi-user support
    // This is important for All Users installs on Windows where Program Files is read-only
    std::string bin_dir = get_cache_dir() + "/bin";

    // Ensure directory exists
    if (!fs::exists(bin_dir)) {
        fs::create_directories(bin_dir);
    }

    return bin_dir;
}

} // namespace utils
} // namespace lemon

