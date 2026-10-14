#pragma once

#include <filesystem>

namespace lemon {
namespace utils {

// fs::exists / fs::is_directory equivalents for paths that may contain symlinks.
// MSVC's std::filesystem refuses to traverse reparse points when the process
// token lacks symlink privileges, which a HuggingFace cache hits constantly.
bool safe_exists(const std::filesystem::path& path);
bool safe_is_directory(const std::filesystem::path& path);

// Creates a directory tree, throwing on failure like fs::create_directories.
void ensure_create_directories(const std::filesystem::path& path);

}  // namespace utils
}  // namespace lemon
