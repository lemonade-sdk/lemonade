#include <lemon/utils/safe_fs.h>
#include <lemon/utils/path_utils.h>

#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <Shlobj.h>
#endif

namespace lemon {
namespace utils {

#ifdef _WIN32
bool safe_exists(const std::filesystem::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool safe_is_directory(const std::filesystem::path& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void ensure_create_directories(const std::filesystem::path& path) {
    if (path.empty()) return;
    if (safe_is_directory(path)) return;
    if (safe_exists(path)) {
        throw std::runtime_error("Cannot create directory; a non-directory already exists at '" +
                                 path_to_utf8(path) + "'");
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (!ec) return;
    // SHCreateDirectoryExW succeeds where create_directories reports an
    // untrusted mount point for a symlink crossing volumes.
    std::wstring wpath = path.wstring();
    DWORD result = SHCreateDirectoryExW(NULL, wpath.c_str(), NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
        char error_msg[256];
        FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            result,
            0,
            error_msg,
            sizeof(error_msg),
            nullptr
        );
        std::string desc = error_msg[0] ? error_msg : "unknown error";
        throw std::runtime_error("Failed to create directory '" + path_to_utf8(path) +
                                 "': " + desc);
    }
}
#else
bool safe_exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

bool safe_is_directory(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path);
}

void ensure_create_directories(const std::filesystem::path& path) {
    if (path.empty()) return;
    if (safe_is_directory(path)) return;
    if (safe_exists(path)) {
        throw std::runtime_error("Cannot create directory; a non-directory already exists at '" +
                                 path_to_utf8(path) + "'");
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw std::runtime_error("Failed to create directory '" + path_to_utf8(path) +
                                 "': " + ec.message());
    }
}
#endif

bool path_inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty()) return false;
    std::error_code ec;
    const std::filesystem::path actual = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const std::filesystem::path relative = actual.lexically_relative(base);
    return !relative.empty() && *relative.begin() != std::filesystem::path("..");
}

}  // namespace utils
}  // namespace lemon
