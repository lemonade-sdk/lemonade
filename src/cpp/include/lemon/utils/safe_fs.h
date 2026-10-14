#pragma once

#include <filesystem>

namespace lemon {
namespace utils {

// fs::exists / fs::is_directory that tolerate symlinks: MSVC refuses to traverse
// reparse points without symlink privileges, which a HuggingFace cache hits
// constantly.
bool safe_exists(const std::filesystem::path& path);
bool safe_is_directory(const std::filesystem::path& path);

// Creates a directory tree, throwing on failure like fs::create_directories.
void ensure_create_directories(const std::filesystem::path& path);

// fs::recursive_directory_iterator throws on entries it cannot open; skipping
// them keeps a cache scan going past one unreadable directory.
#ifdef _WIN32
inline constexpr auto safe_dir_options = std::filesystem::directory_options::skip_permission_denied;
#else
inline constexpr auto safe_dir_options = std::filesystem::directory_options::none;
#endif

// Whether a path sits under a root, resolved first, so a symlink out of the tree
// does not read as inside it.
bool path_inside(const std::filesystem::path& path, const std::filesystem::path& root);

}  // namespace utils
}  // namespace lemon
