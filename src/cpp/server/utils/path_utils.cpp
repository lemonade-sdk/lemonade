#include <lemon/utils/path_utils.h>
#include <filesystem>

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
#else
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        fs::path exe_path(buffer);
        return exe_path.parent_path().string();
    }
    // Fallback for macOS
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        fs::path exe_path(buffer);
        return exe_path.parent_path().string();
    }
    // Last resort: return current directory
    return ".";
#endif
}

std::string get_resource_path(const std::string& relative_path) {
    fs::path exe_dir = get_executable_dir();
    fs::path resource_path = exe_dir / relative_path;
    return resource_path.string();
}

} // namespace utils
} // namespace lemon

