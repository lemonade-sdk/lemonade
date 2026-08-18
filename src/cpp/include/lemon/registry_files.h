#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace lemon {
namespace registry_files {

// Shared by pull and the update check — a drift between them either hides an
// update pull would download or flags one it would not. Throws like pull when
// the variant matches nothing.
std::vector<std::string> select_main_repo_files(
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files);

// Union across two listings (e.g. previous and current revision), so a file
// dropped from one side doesn't silently drop out of comparison too.
std::vector<std::string> select_main_repo_files_union(
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files_a,
    const std::vector<std::string>& repo_files_b);

// Keeps same-repository auxiliary checkpoints in the reuse comparison.
std::vector<std::string> merge_reuse_comparison_files(
    const std::vector<std::string>& base_files,
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files_a,
    const std::vector<std::string>& repo_files_b);

// Checkpoints without a "repo:variant" shape are skipped.
std::map<std::string, std::vector<std::string>> group_aux_checkpoint_variants(
    const std::map<std::string, std::string>& checkpoints);

// Empty when the resolved path does not point inside snapshots/.
std::string snapshot_id_from_resolved_path(
    const std::string& resolved_path,
    const std::filesystem::path& model_cache_path);

// Deliberately NOT the processed-at-pull sha recorded in
// .lemonade_registry.json, which can name a snapshot that was never
// materialized locally (pull keeps refs/main on an older snapshot when the
// selected artifacts are unchanged).
std::string active_local_snapshot(
    const std::string& resolved_path,
    const std::filesystem::path& model_cache_path);

// content_id identifies content across commits (LFS sha256 or git blob oid),
// so revisions compare without downloading anything.
struct HfFileMetadata {
    std::size_t size = 0;
    std::string content_id;
    std::string hash_algorithm;
    std::string hash_value;

    bool has_content_id() const { return !content_id.empty(); }
    bool has_hash() const { return !hash_algorithm.empty() && !hash_value.empty(); }
};

std::string hf_file_metadata_key(const std::string& repo_id, const std::string& filename);

// A selected file absent from tree_entries is omitted; callers must treat
// that as "changed".
std::map<std::string, HfFileMetadata> select_metadata(
    const std::string& repo_id,
    const std::vector<std::string>& selected_files,
    const std::map<std::string, HfFileMetadata>& tree_entries);

// False on any gap: a file missing from either revision's metadata (e.g.
// added upstream inside a directory checkpoint), missing/partial on disk, or
// a size mismatch.
bool can_reuse_previous_hf_snapshot(
    const std::string& repo_id,
    const std::vector<std::string>& selected_files,
    const std::filesystem::path& previous_snapshot,
    const std::map<std::string, HfFileMetadata>& current_metadata,
    const std::map<std::string, HfFileMetadata>& previous_metadata);

// A model spanning several repositories (e.g. an auxiliary checkpoint outside
// the main checkpoint's repository) becomes verified only once every
// repository it spans has completed a determination. mark_determined() is
// idempotent past zero so a caller re-marking the same model twice can't
// corrupt another model's outstanding count.
class DeterminationTracker {
public:
    void add_pending(const std::string& model_name) {
        ++pending_[model_name];
    }

    // True exactly on the call that reaches zero outstanding.
    bool mark_determined(const std::string& model_name) {
        auto it = pending_.find(model_name);
        if (it == pending_.end() || it->second == 0) {
            return false;
        }
        if (--it->second == 0) {
            verified_.insert(model_name);
            return true;
        }
        return false;
    }

    bool is_verified(const std::string& model_name) const {
        return verified_.count(model_name) != 0;
    }

    std::size_t pending_count(const std::string& model_name) const {
        auto it = pending_.find(model_name);
        return it == pending_.end() ? 0 : it->second;
    }

private:
    std::map<std::string, std::size_t> pending_;
    std::unordered_set<std::string> verified_;
};

} // namespace registry_files
} // namespace lemon
