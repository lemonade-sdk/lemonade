#pragma once

#include <lemon/model_manager.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

struct ArtifactSource;

// Every model the store knows, filtered or not: a model that hardware filtering
// hides still has bytes on disk, and removal has to know that. Not the name index
// in model_index.h, which answers "which canonical id owns this bare name".
using KnownModels = std::map<std::string, ModelInfo>;

// A checkpoint row's registry, normalized. Every read of `ModelInfo::registry_source`
// goes through this, because a row may name the registry in any of its aliases.
std::string registry_source_of(const ModelInfo& info);

// What removal did, so the caller only has to decide what to report.
struct DeleteReport {
    // A backend removed the weights it fetched for itself.
    bool delegated = false;
    // Bytes sit outside the configured models directory: nothing was removed, and
    // only the model's own registration may be forgotten.
    bool outside_managed_cache = false;
    // Remnants of an interrupted download went with it.
    bool cleaned_partial_download = false;
};

// A checkpoint's file left inside another checkpoint's repository cache.
struct OrphanedFile {
    std::string model;
    std::string type;
    std::string belongs_to;
    std::string path;
    std::uint64_t size = 0;
};

// What a repository cache records about the model it was fetched for. Written as
// `.lemonade_registry.json` beside the snapshots.
struct Provenance {
    std::string source;      // Registry display name, e.g. "huggingface".
    std::string repo_id;
    std::string revision;
    std::string active_snapshot_id;     // Where refs/main points now.
    std::string processed_snapshot_id;  // What this run fetched, which differs
                                        // when a cancelled pull fell back to the
                                        // snapshot already in place.
};

struct CleanupReport {
    std::vector<OrphanedFile> orphaned;
    std::uint64_t total_bytes = 0;
};

// Where a model's bytes live, whether they are whole, and what may be removed.
// Disk mechanics only: no catalogs, no registry documents, no store lock. What a
// checkpoint *is* comes from an ArtifactSource, and the layout inside a repository
// belongs to the backend that reads it, so the store asks BackendOps for that and
// owns the cache-root arithmetic around it.
class ModelArtifactStore {
  public:
    struct Hooks {
        // Resolved per call: the models root is process configuration a running
        // server can be told about after this store exists.
        std::function<std::string()> cache_root;
        // What a checkpoint is, asked before anything touches it.
        std::function<std::unique_ptr<ArtifactSource>(const ModelInfo&, const std::string&)>
            source_for;
    };

    explicit ModelArtifactStore(Hooks hooks);

    std::string root() const;

    // Where one checkpoint resolves inside the managed cache. Empty for a
    // collection, which is a virtual entry with no bytes of its own.
    std::string resolve_path(const ModelInfo& info, const std::string& type,
                             const std::string& checkpoint) const;

    // The same resolution against another cache root, for a registered model that
    // finds a complete copy of its repository somewhere else on disk.
    std::string resolve_path_in(const ModelInfo& info, const std::string& type,
                                const std::string& checkpoint,
                                const std::string& cache_root) const;

    // Every required checkpoint present, uninterrupted, and valid to the backend
    // that reads it.
    bool complete(const ModelInfo& info) const;

    // One resolved checkpoint, present and whole.
    static bool path_complete(const std::string& path);

    // Bytes at one resolved path. Deliberately per-path, so shard aggregation
    // never happens here; directories are summed because file_size() fails on
    // them.
    static std::uint64_t size_bytes(const std::filesystem::path& path);

    // What the model's resolved checkpoints look like on disk.
    std::vector<ModelFileInfo> list_files(const ModelInfo& info) const;

    // Remove what the sources say lemond wrote, per checkpoint. `all_models` is the
    // whole model map, which shared-repository scope cannot be answered without.
    DeleteReport delete_artifacts(const ModelInfo& info, const KnownModels& all_models) const;

    // Stray copies of another repository's checkpoint variant, left in a
    // repository cache. A dry run reports and deletes nothing.
    CleanupReport cleanup_orphaned_cache(const KnownModels& all_models, bool dry_run) const;

    // A repository cache's own bookkeeping: which snapshot is active, and which
    // model was processed into it.

    // The snapshot `refs/main` points at, empty when the cache has none.
    std::string active_snapshot_ref(const std::filesystem::path& repo_cache_dir) const;
    std::filesystem::path active_snapshot_path(const std::filesystem::path& repo_cache_dir) const;
    void write_active_snapshot(const std::filesystem::path& repo_cache_dir,
                               const std::string& commit_hash) const;
    // A superseded snapshot, kept only while it is not the active one.
    void remove_unused_snapshot(const std::filesystem::path& repo_cache_dir,
                                const std::string& snapshot_dir,
                                const std::string& active_ref) const;

    // The snapshot this model was last processed from, so a re-pull can reuse it.
    // Empty when there is none, or when what was processed is not this selection.
    std::string processed_snapshot_id(const std::filesystem::path& repo_cache_dir,
                                      const ModelInfo& info) const;
    void record_processed_model(const std::filesystem::path& repo_cache_dir,
                                const ModelInfo& info,
                                const Provenance& provenance) const;

  private:
    // The checkpoints whose bytes lemond wrote, and any recipe whose backend
    // removes its own weights.
    struct Ownership {
        std::vector<std::string> cache_dir;
        std::string delegated_recipe;
    };
    Ownership ownership_of(const ModelInfo& info) const;
    bool cleanup_incomplete_download(const ModelInfo& info, const KnownModels& all_models) const;

    Hooks hooks_;
};

}  // namespace models
}  // namespace lemon
