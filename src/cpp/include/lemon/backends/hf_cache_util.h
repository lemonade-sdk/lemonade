#pragma once

#include <filesystem>
#include <string>

namespace lemon {
namespace backends {
namespace hf_cache {

// Shared model-hub cache mechanics used by backend ops to locate model
// artifacts on disk (the same logic model_manager uses for its own cache work).

// Exists check that tolerates the symlinks HF uses for dedup (Win32 on Windows,
// where MSVC's std::filesystem refuses untrusted reparse points).
bool exists(const std::filesystem::path& p);

// Directory-iteration options that skip inaccessible/symlinked entries instead
// of throwing.
std::filesystem::directory_options dir_options();

// The active registry snapshot directory (snapshots/<refs/main>) for a model cache
// dir, or an empty path if there is no recorded ref / it doesn't exist.
std::filesystem::path active_snapshot_path(const std::filesystem::path& model_cache_path);

// The revision recorded in <model_cache>/refs/main, or "" when none is recorded.
std::string read_ref_main(const std::filesystem::path& model_cache_path);

// Records the revision. An empty revision writes nothing.
void write_ref_main(const std::filesystem::path& model_cache_path,
                    const std::string& revision);

// Provider-qualified cache directory name for a repository id.
std::string repo_id_to_cache_dir_name(const std::string& repo_id,
                                      const std::string& registry_source);

} // namespace hf_cache
} // namespace backends
} // namespace lemon
