#pragma once

#include <filesystem>

namespace lemon {
namespace utils {

// Replacements for fs::exists / fs::is_directory on paths that may contain
// symlinks. MSVC's std::filesystem refuses to traverse reparse points it treats
// as "untrusted" when the process token lacks symlink privileges — a HuggingFace
// cache, which deduplicates through symlinks, hits this constantly. The Win32
// API has no such restriction.
bool safe_exists(const std::filesystem::path& path);
bool safe_is_directory(const std::filesystem::path& path);

// Creates a directory tree, throwing on failure to keep the fail-fast contract
// of fs::create_directories.
void ensure_create_directories(const std::filesystem::path& path);

}  // namespace utils
}  // namespace lemon
