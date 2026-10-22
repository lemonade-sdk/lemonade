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

// Whether a string can be exactly one path component. Revisions, repository directory
// names and file names each reach a path from a document fetched over the network or
// from a cache written by an earlier run, and pointing a registry at a mirror or gateway
// is a supported configuration, so none of them arrives trusted. Rejects separators, a
// drive specifier, `.` and `..`, control characters, and a name no filesystem will hold.
bool is_safe_path_component(const std::string& name);

// Whether a string is a path under a root: components that are each safe, so nothing
// traverses out and nothing is absolute.
bool is_safe_relative_path(const std::string& path);

}  // namespace utils
}  // namespace lemon
