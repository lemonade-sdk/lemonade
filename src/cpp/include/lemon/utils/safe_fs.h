#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

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

// Whether a path sits under a root as written, nothing resolved: a Hugging Face
// snapshot entry is a symlink into blobs/, and resolved it has left its own
// snapshot. Trusting a path wants path_inside, which resolves.
bool path_inside_lexically(const std::filesystem::path& path,
                           const std::filesystem::path& root);

// Whether a string can be exactly one path component: no separator, drive specifier,
// `.` or `..`, or control character. Snapshot ids, directory names and file names reach
// a path from a fetched document or a cache written earlier, and a registry may be
// pointed at a mirror, so none of them arrives trusted.
bool is_safe_path_component(const std::string& name);

// Every component safe, so nothing traverses out and nothing is absolute.
bool is_safe_relative_path(const std::string& path);

}  // namespace utils
}  // namespace lemon
