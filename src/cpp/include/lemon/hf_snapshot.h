#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lemon {
namespace hf_snapshot {

// Snapshot-relative artifacts a single model consumes, given its resolved
// checkpoint paths (role -> absolute path). Used to decide whether a new
// upstream commit actually touches that model.
//
// Deliberately per-model rather than "everything in the snapshot": repositories
// that host many variants would otherwise look updated whenever any sibling
// variant changes. A sharded GGUF resolves to a single shard, so the rest of the
// shard family is added too. Paths outside `snapshot_dir` belong to another
// repository and are skipped.
//
// Returns empty when the set cannot be established, which callers must treat as
// "assume changed". Defined in model_manager.cpp, which owns the Windows-safe
// filesystem helpers.
std::vector<std::string> model_artifacts(
    const std::map<std::string, std::string>& resolved_paths,
    const std::filesystem::path& snapshot_dir);

} // namespace hf_snapshot
} // namespace lemon
