#pragma once

// Owner-only file replacement shared by the identity key file and the
// per-user identity store. Not part of the public SDK interface.

#include <filesystem>
#include <string>

namespace lnsdk::detail {

/// Replace `path` with `text`. Writes a new file beside `path` that only the
/// owner can read and write, then renames it over `path`, so a reader sees
/// either the old file or the complete new one. On POSIX the new file has
/// mode 0600 regardless of the umask; on Windows it takes the access control
/// list of its directory. Does not create directories. Returns false and
/// leaves `path` unchanged when the write fails; the temporary file is
/// removed.
bool replace_owner_only_file(const std::filesystem::path& path, const std::string& text);

}  // namespace lnsdk::detail
