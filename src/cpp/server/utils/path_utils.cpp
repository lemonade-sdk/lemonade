#include <lemon/utils/path_utils.h>
#include <filesystem>
#include <vector>
#include <string>

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

} // namespace utils
} // namespace lemon

