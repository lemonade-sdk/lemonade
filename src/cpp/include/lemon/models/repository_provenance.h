#pragma once

#include <lemon/models/artifact_store.h>

#include <filesystem>
#include <string>

namespace lemon {
namespace models {

// A repository cache's own bookkeeping: which snapshot is active, and which model was
// processed into it. The store forwards these; the rules live here.

// The snapshot `refs/main` points at, empty when the cache has none.
std::string read_active_snapshot_ref(const std::filesystem::path& repo_cache_dir);
std::filesystem::path read_active_snapshot_path(const std::filesystem::path& repo_cache_dir);
void write_active_snapshot_ref(const std::filesystem::path& repo_cache_dir,
                               const std::string& commit_hash);

// A superseded snapshot, kept only while it is not the active one.
void remove_unused_snapshot(const std::filesystem::path& repo_cache_dir,
                            const std::string& snapshot_dir, const std::string& active_ref);

// The snapshot this model was last processed from, so a re-pull can reuse it. Empty when
// there is none, or when what was processed is not this selection.
std::string processed_snapshot_id(const std::filesystem::path& repo_cache_dir,
                                  const ModelInfo& info);

// checkpoint_type names the slot whose bytes landed in this repository; empty means main.
void record_processed_model(const std::filesystem::path& repo_cache_dir, const ModelInfo& info,
                            const Provenance& provenance, const std::string& checkpoint_type);

}  // namespace models
}  // namespace lemon
