#include <lemon/models/artifact_store.h>

#include <lemon/backends/backend_ops.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/backends/hf_cache_util.h>
#include <lemon/gguf_reader.h>
#include <lemon/gguf_shard_utils.h>
#include <lemon/model_registry.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using lemon::utils::path_inside;
using lemon::utils::safe_dir_options;
using lemon::utils::safe_exists;
using lemon::utils::safe_is_directory;

namespace lemon {
namespace models {

namespace {

constexpr const char* kLog = "ModelArtifactStore";
constexpr const char kProvenanceFile[] = ".lemonade_registry.json";

// Non-recursive scan for .partial markers to confirm folder integrity.
bool has_partial_files(const fs::path& dir) {
    std::error_code ec;
    if (!safe_is_directory(dir)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".partial") {
            return true;
        }
    }
    return false;
}

// A side cache the engine fetches on first use is optional: absent bytes there
// leave the model downloaded.
bool acquired_lazily(const ModelInfo& info, const std::string& type) {
    return backends::ops_for(info.recipe)->acquires_lazily(type);
}

// A checkpoint placed beside another checkpoint's file instead of in a snapshot of
// its own belongs where it is; it is never a stray copy.
bool lands_beside_main(const ModelInfo& info, const std::string& type) {
    return backends::ops_for(info.recipe)->checkpoint_layout({info.model_name, type}) != nullptr;
}

std::string normalized_relative_path(const fs::path& path, const fs::path& root) {
    std::string rel = utils::path_to_utf8(path.lexically_relative(root));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

void remove_file_or_throw(const fs::path& path, const std::string& description) {
    if (!safe_exists(path)) {
        return;
    }

    LOG(INFO, kLog) << "Removing " << description << ": " << utils::path_to_utf8(path) << std::endl;
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        throw std::runtime_error("Failed to remove " + description + " '" +
                                 utils::path_to_utf8(path) + "': " + ec.message());
    }
}

void remove_path_or_throw(const fs::path& path, const std::string& description) {
    if (!safe_exists(path)) {
        return;
    }

    LOG(INFO, kLog) << "Removing " << description << ": " << utils::path_to_utf8(path) << std::endl;
    std::error_code ec;
    if (safe_is_directory(path)) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }
    if (ec) {
        throw std::runtime_error("Failed to remove " + description + " '" +
                                 utils::path_to_utf8(path) + "': " + ec.message());
    }
}

// Remove empty parent directories up to (but not including) the stop directory.
void cleanup_empty_parents(const fs::path& path, const fs::path& stop_dir) {
    std::error_code ec;
    fs::path parent = path.parent_path();
    for (int i = 0; i < 6 && !parent.empty() && parent != stop_dir; ++i) {
        if (fs::is_empty(parent, ec) && !ec) {
            fs::remove(parent, ec);
            parent = parent.parent_path();
        } else {
            break;
        }
    }
}

// A snapshot entry that is a symlink into blobs/ shares that blob with every other
// entry pointing at it, so the blob may only go when nothing references it.
//
// TODO: no Lemonade unit test covers this. Lemonade's own downloader writes real
// files without blobs, so a test would have to build the blob+symlink layout with
// huggingface_hub tooling (e.g. hf_hub_download()).
void cleanup_orphaned_blob(const fs::path& file_path, const fs::path& repo_dir) {
    std::error_code ec;
    if (!fs::is_symlink(file_path, ec) || ec) {
        return;
    }

    fs::path link_target = fs::read_symlink(file_path, ec);
    if (ec) return;
    fs::path blob_path = fs::canonical(file_path.parent_path() / link_target, ec);
    if (ec || !fs::exists(blob_path)) return;

    fs::path snapshots_dir = repo_dir / "snapshots";
    if (!fs::exists(snapshots_dir)) return;

    for (auto& entry : fs::recursive_directory_iterator(snapshots_dir, ec)) {
        if (ec) break;
        if (entry.path() == file_path) continue;
        if (!fs::is_symlink(entry.path(), ec) || ec) continue;

        fs::path other_target = fs::read_symlink(entry.path(), ec);
        if (ec) continue;
        if (fs::canonical(entry.path().parent_path() / other_target, ec) == blob_path) {
            return;  // Another entry still points here.
        }
    }

    LOG(INFO, kLog) << "Removing orphaned blob: " << utils::path_to_utf8(blob_path) << std::endl;
    fs::remove(blob_path, ec);

    fs::path blobs_dir = blob_path.parent_path();
    if (fs::exists(blobs_dir) && fs::is_empty(blobs_dir, ec) && !ec) {
        fs::remove(blobs_dir, ec);
    }
}

void cleanup_orphaned_blobs_under(const fs::path& path, const fs::path& repo_dir) {
    if (!safe_exists(path)) {
        return;
    }

    if (!safe_is_directory(path)) {
        cleanup_orphaned_blob(path, repo_dir);
        return;
    }

    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(path, safe_dir_options, ec)) {
        if (ec) break;
        cleanup_orphaned_blob(entry.path(), repo_dir);
    }
}

void remove_stale_manifest_for_partial(const fs::path& partial_path, const fs::path& stop_dir) {
    fs::path parent = partial_path.parent_path();
    for (int i = 0; i < 8 && !parent.empty() && parent != stop_dir; ++i) {
        fs::path manifest_path = parent / ".download_manifest.json";
        if (safe_exists(manifest_path)) {
            remove_file_or_throw(manifest_path, "stale download manifest");
            cleanup_empty_parents(manifest_path, stop_dir);
            return;
        }
        parent = parent.parent_path();
    }
}

// Another model in the same registry source still reads this repository.
bool repo_shared(const std::string& repo_id, const std::string& registry_source,
                 const std::string& exclude_model, const KnownModels& all_models) {
    for (const auto& [name, info] : all_models) {
        if (name == exclude_model || !info.source.empty()) continue;
        if (registry_source_of(info) != registry_source) continue;
        for (const auto& [type, checkpoint] : info.checkpoints) {
            (void)type;
            if (repo_id_from_checkpoint(checkpoint) == repo_id) return true;
        }
    }
    return false;
}

// Another model asked for this exact checkpoint, so its bytes are that model's too.
bool checkpoint_shared(const std::string& checkpoint, const std::string& registry_source,
                       const std::string& exclude_model, const KnownModels& all_models) {
    for (const auto& [name, info] : all_models) {
        if (name == exclude_model || !info.source.empty()) continue;
        if (registry_source_of(info) != registry_source) continue;
        for (const auto& [type, other] : info.checkpoints) {
            (void)type;
            if (other == checkpoint) return true;
        }
    }
    return false;
}

}  // namespace

std::string registry_source_of(const ModelInfo& info) {
    return remote_registry_source_name(parse_remote_registry_source(info.registry_source));
}

ModelArtifactStore::ModelArtifactStore(Hooks hooks) : hooks_(std::move(hooks)) {}

std::string ModelArtifactStore::root() const {
    return hooks_.cache_root ? hooks_.cache_root() : std::string();
}

std::string ModelArtifactStore::resolve_path(const ModelInfo& info, const std::string& type,
                                             const std::string& checkpoint) const {
    // Collections are virtual entries with no direct checkpoint to resolve.
    if (is_model_collection_recipe(info.recipe)) {
        return "";
    }

    // Local-path models use the checkpoint as-is (absolute path to a file).
    if (info.source == "local_path") {
        return checkpoint;
    }

    const std::string cache = root();

    // Local uploads: the checkpoint is a path relative to the cache root. Joined
    // rather than concatenated, so a row naming an absolute path resolves to that
    // path and the delete proof can see it is not ours to remove.
    if (info.source == "local_upload") {
        std::string normalized = checkpoint;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return utils::path_to_utf8(utils::path_from_utf8(cache) / utils::path_from_utf8(normalized));
    }

    return resolve_path_in(info, type, checkpoint, cache);
}

std::string ModelArtifactStore::resolve_path_in(const ModelInfo& info, const std::string& type,
                                                const std::string& checkpoint,
                                                const std::string& cache_root) const {
    // The backend's ops find the artifact within its repo cache dir: a .gguf file,
    // a genai_config.json directory, a .bin, and so on.
    backends::CheckpointResolveContext ctx;
    ctx.hf_cache = cache_root;
    ctx.repo_id = repo_id_from_checkpoint(checkpoint);
    ctx.main_repo_id = repo_id_from_checkpoint(info.checkpoint("main"));
    ctx.variant = variant_from_checkpoint(checkpoint);
    ctx.registry_source = registry_source_of(info);
    ctx.model_cache_path = cache_root + "/" +
        backends::hf_cache::repo_id_to_cache_dir_name(ctx.repo_id, ctx.registry_source);
    ctx.type = type;
    ctx.checkpoint = checkpoint;

    return backends::ops_for(info.recipe)->resolve_checkpoint_path(info, ctx);
}

bool ModelArtifactStore::complete(const ModelInfo& info) const {
    for (const auto& [type, checkpoint] : info.checkpoints) {
        (void)checkpoint;

        // A side cache the engine fetches on first use is not required.
        if (acquired_lazily(info, type)) continue;

        const std::string resolved_path = info.resolved_path(type);
        if (!path_complete(resolved_path)) {
            return false;
        }

        // Per-backend file validation (e.g. llamacpp checks GGUF magic).
        std::string invalid = backends::ops_for(info.recipe)->validate_checkpoint_file(resolved_path);
        if (!invalid.empty()) {
            LOG(WARNING, kLog)
                << invalid << "; marking model as not downloaded: " << resolved_path << std::endl;
            return false;
        }
    }
    return true;
}

bool ModelArtifactStore::path_complete(const std::string& path_str) {
    if (path_str.empty()) return false;

    fs::path resolved = utils::path_from_utf8(path_str);
    if (!safe_exists(resolved)) return false;

    // A manifest or .partial file marks an interrupted multi-file download: file
    // checkpoints check their parent directory for the manifest and their own
    // .partial marker, directory checkpoints check the directory itself.
    fs::path marker_dir = safe_is_directory(resolved) ? resolved : resolved.parent_path();
    if (safe_exists(marker_dir / ".download_manifest.json")) return false;

    if (!safe_is_directory(resolved)) {
        return !safe_exists(utils::path_from_utf8(path_str + ".partial"));
    }

    return !has_partial_files(resolved);
}

std::uint64_t ModelArtifactStore::size_bytes(const fs::path& path) {
    std::error_code ec;
    if (!safe_exists(path)) {
        return 0;
    }

    if (!safe_is_directory(path)) {
        auto size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    std::uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(path, safe_dir_options, ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        auto size = fs::file_size(entry.path(), ec);
        if (!ec) {
            total += size;
        } else {
            ec.clear();
        }
    }
    return total;
}

std::vector<ModelFileInfo> ModelArtifactStore::list_files(const ModelInfo& info) const {
    std::vector<ModelFileInfo> files;
    files.reserve(info.resolved_paths.size());

    for (const auto& [role, resolved_path] : info.resolved_paths) {
        if (resolved_path.empty()) {
            continue;
        }

        fs::path path = utils::path_from_utf8(resolved_path);
        const bool path_exists = safe_exists(path);

        ModelFileInfo file;
        file.name = utils::path_to_utf8(path.filename());
        file.path = resolved_path;
        file.role = role;
        file.exists = path_exists;
        file.size_bytes = path_exists ? size_bytes(path) : 0;
        files.push_back(std::move(file));
    }

    return files;
}

// A checkpoint whose source cannot be classified counts as lemond's, which is the
// safe direction: removal then has to prove the bytes sit in the managed cache.
ModelArtifactStore::Ownership ModelArtifactStore::ownership_of(const ModelInfo& info) const {
    Ownership ownership;
    for (const auto& [type, checkpoint] : info.checkpoints) {
        (void)checkpoint;

        // It is beside the file it was built for, which this pass does remove.
        if (acquired_lazily(info, type)) continue;

        Deletion::Kind kind = Deletion::Kind::CacheDir;
        try {
            kind = hooks_.source_for(info, type)->deletion().kind;
        } catch (const std::exception&) {
            kind = Deletion::Kind::CacheDir;
        }

        if (kind == Deletion::Kind::Delegate) {
            ownership.delegated_recipe = info.recipe;
        } else if (kind == Deletion::Kind::CacheDir) {
            ownership.cache_dir.push_back(type);
        }
    }
    return ownership;
}

DeleteReport ModelArtifactStore::delete_artifacts(const ModelInfo& info,
                                                  const KnownModels& all_models) const {
    DeleteReport report;
    const Ownership ownership = ownership_of(info);

    // A backend that fetched its own weights removes them; lemond held only the
    // registration.
    if (!ownership.delegated_recipe.empty()) {
        backends::ops_for(ownership.delegated_recipe)->remove_self_managed_model(info);
        report.delegated = true;
        return report;
    }

    // A download that never resolved can still leave resumable .partial files and
    // a manifest behind.
    if (info.resolved_path().empty()) {
        report.cleaned_partial_download = cleanup_incomplete_download(info, all_models);
        return report;
    }

    // Nothing lemond wrote: imported bytes stay where their owner put them, and an
    // incomplete copy in a registry cache has nothing to remove.
    if (ownership.cache_dir.empty()) {
        return report;
    }

    const fs::path managed_root = utils::path_from_utf8(root());

    // One checkpoint's repository cache, or only its variant when another model
    // shares the repository.
    for (const std::string& type : ownership.cache_dir) {
        const std::string checkpoint = info.checkpoint(type);
        const fs::path resolved = utils::path_from_utf8(info.resolved_path(type));

        // The proof against the models directory rather than the checkpoint's own
        // repository: an uploaded copy sits in the cache root, not in a
        // models-- directory, and lemond wrote it, so lemond removes it.
        if (!path_inside(resolved, managed_root)) {
            report.outside_managed_cache = true;
            return report;
        }

        if (info.source == "local_upload") {
            remove_path_or_throw(resolved, "uploaded copy");
            cleanup_empty_parents(resolved, managed_root);
            continue;
        }

        const std::string registry_source = registry_source_of(info);
        const std::string repo = repo_id_from_checkpoint(checkpoint);
        if (repo.empty()) {
            throw std::runtime_error("Cannot delete a registered model with an invalid checkpoint.");
        }
        const fs::path repo_dir = managed_root /
            backends::hf_cache::repo_id_to_cache_dir_name(repo, registry_source);

        // An auxiliary checkpoint is a dependency: while another model reads the
        // same file it stays. A main checkpoint is this model's own identity, so a
        // repository shared with other models still loses just its variant.
        if (type != "main" &&
            checkpoint_shared(checkpoint, registry_source, info.model_name, all_models)) {
            LOG(INFO, kLog) << "Keeping " << checkpoint
                            << ", another model reads the same checkpoint" << std::endl;
            continue;
        }

        if (!repo_shared(repo, registry_source, info.model_name, all_models)) {
            if (safe_exists(repo_dir)) {
                LOG(INFO, kLog) << "Removing repository cache: " << utils::path_to_utf8(repo_dir)
                                << std::endl;
                std::error_code ec;
                fs::remove_all(repo_dir, ec);
                if (ec) {
                    throw std::runtime_error("Failed to remove repository cache '" +
                                             utils::path_to_utf8(repo_dir) + "': " + ec.message());
                }
            } else {
                LOG(WARNING, kLog) << "Repository cache not found (may already be deleted): "
                                   << utils::path_to_utf8(repo_dir) << std::endl;
            }
            continue;
        }

        LOG(INFO, kLog) << "Repository " << repo
                        << " is shared with other models, removing its variant path only"
                        << std::endl;
        if (safe_exists(resolved)) {
            cleanup_orphaned_blobs_under(resolved, repo_dir);
            remove_path_or_throw(resolved, "variant path");
            cleanup_empty_parents(resolved, repo_dir);
        }
    }

    return report;
}

// Whatever a cancelled download left behind; path_complete() says what marks one.
bool ModelArtifactStore::cleanup_incomplete_download(const ModelInfo& info,
                                                     const KnownModels& all_models) const {
    const std::string main_checkpoint = info.checkpoint("main");
    const std::string main_repo = repo_id_from_checkpoint(main_checkpoint);
    if (main_repo.empty()) {
        return false;
    }

    const fs::path managed_root = utils::path_from_utf8(root());
    const std::string registry_source = registry_source_of(info);
    const fs::path repo_dir = managed_root /
        backends::hf_cache::repo_id_to_cache_dir_name(main_repo, registry_source);
    if (!safe_exists(repo_dir)) {
        return false;
    }

    // With no other model on this repository, the whole incomplete cache goes:
    // .partial files, stale manifests, whatever finished before the cancellation,
    // refs and blobs, in one atomic intent.
    if (!repo_shared(main_repo, registry_source, info.model_name, all_models)) {
        LOG(INFO, kLog) << "Removing incomplete model cache: " << utils::path_to_utf8(repo_dir)
                       << std::endl;
        std::error_code ec;
        fs::remove_all(repo_dir, ec);
        if (ec) {
            throw std::runtime_error("Failed to remove incomplete model cache '" +
                                     utils::path_to_utf8(repo_dir) + "': " + ec.message());
        }
        return true;
    }

    // A shared repository must not go wholesale: only this variant's partial, plus
    // the manifest that counted it as an incomplete download.
    const std::string variant = variant_from_checkpoint(main_checkpoint);
    if (variant.empty()) {
        LOG(INFO, kLog) << "Keeping shared incomplete cache for " << main_repo
                        << " because the model has no exact variant" << std::endl;
        return false;
    }

    fs::path snapshots_dir = repo_dir / "snapshots";
    if (!safe_exists(snapshots_dir)) {
        return false;
    }

    std::string normalized_variant = variant;
    std::replace(normalized_variant.begin(), normalized_variant.end(), '\\', '/');

    bool removed_any = false;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(snapshots_dir, safe_dir_options, ec)) {
        if (ec) break;

        const fs::path partial_path = entry.path();
        const std::string rel = normalized_relative_path(partial_path, snapshots_dir);
        const std::string filename = partial_path.filename().string();
        const bool matches_variant_partial =
            filename == variant + ".partial" ||
            rel == normalized_variant + ".partial" ||
            rel.rfind("/" + normalized_variant + ".partial") != std::string::npos;
        if (!matches_variant_partial) {
            continue;
        }

        remove_file_or_throw(partial_path, "incomplete model download partial");
        remove_stale_manifest_for_partial(partial_path, repo_dir);
        cleanup_empty_parents(partial_path, repo_dir);
        removed_any = true;
    }

    if (!removed_any) {
        LOG(INFO, kLog) << "No incomplete cache artifacts found for shared repo " << main_repo
                        << std::endl;
    }
    return removed_any;
}

CleanupReport ModelArtifactStore::cleanup_orphaned_cache(const KnownModels& all_models,
                                                         bool dry_run) const {
    CleanupReport report;
    const std::string cache = root();

    // Multi-repo models can leave a checkpoint's file inside the main repository's
    // cache instead of its own.
    for (const auto& [name, info] : all_models) {
        if (info.checkpoints.size() <= 1) continue;

        const std::string main_repo = repo_id_from_checkpoint(info.checkpoint("main"));
        const fs::path main_cache = utils::path_from_utf8(
            cache + "/" + backends::hf_cache::repo_id_to_cache_dir_name(main_repo,
                                                                        registry_source_of(info)));
        if (!safe_exists(main_cache)) continue;

        for (const auto& [type, checkpoint] : info.checkpoints) {
            // A sibling copy in the main repository is where it belongs.
            if (type == "main" || lands_beside_main(info, type)) continue;

            const std::string cp_repo = repo_id_from_checkpoint(checkpoint);
            if (cp_repo == main_repo) continue;  // Same repository, no orphan possible.

            const std::string variant = variant_from_checkpoint(checkpoint);
            if (variant.empty()) continue;

            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(main_cache, safe_dir_options, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;

                const std::string filename = entry.path().filename().string();
                const bool matches = (filename == variant) ||
                    (variant.find('/') != std::string::npos &&
                     utils::path_to_utf8(entry.path()).find(variant) != std::string::npos);
                if (!matches) continue;

                std::error_code size_ec;
                const auto file_size = fs::file_size(entry.path(), size_ec);
                OrphanedFile orphan;
                orphan.model = name;
                orphan.type = type;
                orphan.belongs_to = cp_repo;
                orphan.path = utils::path_to_utf8(entry.path());
                orphan.size = size_ec ? 0 : file_size;
                report.orphaned.push_back(std::move(orphan));
                report.total_bytes += orphan.size;

                if (!dry_run) {
                    LOG(INFO, kLog) << "Removing orphaned file: " << orphan.path << std::endl;
                    fs::remove(entry.path());
                }
                break;  // Found the orphan for this checkpoint.
            }
        }
    }

    return report;
}

std::string ModelArtifactStore::active_snapshot_ref(const fs::path& repo_cache_dir) const {
    return backends::hf_cache::read_ref_main(repo_cache_dir);
}

fs::path ModelArtifactStore::active_snapshot_path(const fs::path& repo_cache_dir) const {
    const std::string ref = active_snapshot_ref(repo_cache_dir);
    if (ref.empty()) {
        return fs::path();
    }
    const fs::path snapshot = repo_cache_dir / "snapshots" / utils::path_from_utf8(ref);
    return safe_exists(snapshot) ? snapshot : fs::path();
}

void ModelArtifactStore::write_active_snapshot(const fs::path& repo_cache_dir,
                                               const std::string& commit_hash) const {
    backends::hf_cache::write_ref_main(repo_cache_dir, commit_hash);
}

void ModelArtifactStore::remove_unused_snapshot(const fs::path& repo_cache_dir,
                                                const std::string& snapshot_dir,
                                                const std::string& active_ref) const {
    if (repo_cache_dir.empty() || snapshot_dir.empty() || active_ref.empty()) {
        return;
    }

    const fs::path unused = utils::path_from_utf8(snapshot_dir);
    const fs::path active = repo_cache_dir / "snapshots" / utils::path_from_utf8(active_ref);
    std::error_code ec;
    if (!safe_exists(unused) || fs::equivalent(unused, active, ec)) {
        return;
    }
    ec.clear();
    fs::remove_all(unused, ec);
    if (ec) {
        LOG(WARNING, kLog) << "Could not remove unused snapshot: " << utils::path_to_utf8(unused)
                           << " (" << ec.message() << ")" << std::endl;
    }
}

// What was processed: the recipe plus every checkpoint it was asked for, so a row
// that changed shape does not reuse a snapshot that no longer matches it.
std::string processed_selection(const ModelInfo& info) {
    std::ostringstream selection;
    selection << info.recipe;
    for (const auto& [type, checkpoint] : info.checkpoints) {
        selection << '\n' << type << '=' << checkpoint;
    }
    return selection.str();
}

std::string ModelArtifactStore::processed_snapshot_id(const fs::path& repo_cache_dir,
                                                      const ModelInfo& info) const {
    const fs::path provenance_path = repo_cache_dir / kProvenanceFile;
    if (!safe_exists(provenance_path)) {
        return "";
    }

    try {
        const json provenance = utils::JsonUtils::load_from_file(utils::path_to_utf8(provenance_path));
        if (!provenance.contains("processed_models") || !provenance["processed_models"].is_object()) {
            return "";
        }
        const auto& processed = provenance["processed_models"];
        const auto entry = processed.find(info.model_name);
        if (entry == processed.end() || !entry->is_object()) {
            return "";
        }
        if (utils::JsonUtils::get_or_default<std::string>(*entry, "selection", "") != processed_selection(info)) {
            return "";
        }
        return utils::JsonUtils::get_or_default<std::string>(*entry, "snapshot_id", "");
    } catch (const std::exception&) {
        return "";
    }
}

void ModelArtifactStore::record_processed_model(const fs::path& repo_cache_dir,
                                                const ModelInfo& info,
                                                const Provenance& provenance) const {
    const fs::path provenance_path = repo_cache_dir / kProvenanceFile;

    // What other models recorded here survives; only this model's entry changes.
    json processed = json::object();
    if (safe_exists(provenance_path)) {
        try {
            const json previous = utils::JsonUtils::load_from_file(utils::path_to_utf8(provenance_path));
            if (previous.contains("processed_models") && previous["processed_models"].is_object()) {
                processed = previous["processed_models"];
            }
        } catch (const std::exception&) {
            // Invalid provenance is replaced after a successful download.
        }
    }

    // Only the main repository is this model's provenance; an auxiliary repository
    // would otherwise claim a model it holds part of.
    if (provenance.repo_id == repo_id_from_checkpoint(info.checkpoint("main"))) {
        processed[info.model_name] = {
            {"selection", processed_selection(info)},
            {"snapshot_id", provenance.processed_snapshot_id}
        };
    }

    utils::JsonUtils::save_to_file(json{{"source", provenance.source},
                                 {"repo_id", provenance.repo_id},
                                 {"revision", provenance.revision},
                                 {"snapshot_id", provenance.active_snapshot_id},
                                 {"processed_models", std::move(processed)}},
                            utils::path_to_utf8(provenance_path));
}

// Resolve the complete family of a registered sharded GGUF. A first shard
// alone is not a usable model, and its remaining shards must not be discovered
// as an unrelated extra.* model. Other quantizations remain independent.
static bool collect_registered_shards(const fs::path& resolved, const fs::path& repo_root,
                                      std::set<fs::path>& owned_paths) {
    const fs::path file = resolved.lexically_normal();
    std::string base;
    int total = 0;
    if (!is_gguf_shard_filename(file.filename().string(), &base, &total)) {
        owned_paths.insert(file);
        return true;
    }

    std::set<int> indices;
    bool complete = true;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(file.parent_path(), safe_dir_options, ec)) {
        if (ec) return false;
        const std::string filename = entry.path().filename().string();
        if (!same_shard_family(filename, base, total)) continue;

        int index = 0;
        is_gguf_shard_filename(filename, nullptr, nullptr, &index);
        if (!entry.is_regular_file(ec) || ec ||
            !path_inside(entry.path(), repo_root)) {
            return false;
        }
        owned_paths.insert(entry.path().lexically_normal());
        const std::string path = utils::path_to_utf8(entry.path());
        if (!models::ModelArtifactStore::path_complete(path) ||
            !backends::ops_for("llamacpp")->validate_checkpoint_file(path).empty()) {
            complete = false;  // Still scan remaining siblings for discovery exclusion.
        }
        indices.insert(index);
    }
    return !ec && complete && indices.size() == static_cast<size_t>(total);
}

// Only complete repository caches can reclaim registered model identities;
// loose GGUFs remain independent extra.* entries.
bool ModelArtifactStore::adopt_extra_cache(ModelInfo& info,
                                           std::set<fs::path>* registered_paths) const {
    const std::string extra_dir =
        hooks_.extra_models_dir != nullptr ? hooks_.extra_models_dir() : std::string();
    if (extra_dir.empty() || !info.source.empty() ||
        is_model_collection_recipe(info.recipe)) return false;

    const fs::path root = utils::path_from_utf8(extra_dir);
    const std::string repo = repo_id_from_checkpoint(info.checkpoint("main"));
    if (repo.empty()) return false;
    const fs::path main_repo = root /
        backends::hf_cache::repo_id_to_cache_dir_name(repo, registry_source_of(info));
    if (!safe_is_directory(main_repo) || !path_inside(main_repo, root)) {
        return false;
    }

    ModelInfo candidate = info;
    // Discovery must also exclude matched shards when the family is incomplete.
    // Resolved model paths are still only committed after full validation.
    std::set<fs::path> local_owned_paths;
    std::set<fs::path>& owned_paths = registered_paths ? *registered_paths : local_owned_paths;
    try {
        for (const auto& [type, checkpoint] : info.checkpoints) {
            if (acquired_lazily(info, type)) continue;
            const std::string checkpoint_repo = repo_id_from_checkpoint(checkpoint);
            if (checkpoint_repo.empty()) return false;
            const fs::path expected_repo = root /
                backends::hf_cache::repo_id_to_cache_dir_name(checkpoint_repo, registry_source_of(info));
            candidate.resolved_paths[type] =
                resolve_path_in(info, type, checkpoint, extra_dir);
            const std::string& resolved = candidate.resolved_paths[type];
            if (info.recipe == "llamacpp" && type == "main" &&
                (safe_is_directory(utils::path_from_utf8(resolved)) ||
                 !gguf_reader_detail::ends_with_ignore_case(resolved, ".gguf"))) {
                return false;
            }
            // Reject escaped refs/main entries and symlinks, including redirects
            // into a *different* repository under the same extra models root.
            const fs::path resolved_path = utils::path_from_utf8(resolved);
            const bool own_repo = path_inside(expected_repo, root) &&
                path_inside(resolved_path, expected_repo);
            const bool legacy_main_repo = type != "main" &&
                path_inside(resolved_path, main_repo);
            if (resolved.empty() || (!own_repo && !legacy_main_repo) ||
                !collect_registered_shards(resolved_path,
                    own_repo ? expected_repo : main_repo, owned_paths)) {
                return false;
            }
        }
        if (!complete(candidate)) return false;
    } catch (const std::exception&) {
        return false;  // An optional directory must never break catalog discovery.
    }
    info.resolved_paths = std::move(candidate.resolved_paths);
    return true;
}

backends::DownloadPolicy ModelArtifactStore::download_policy(const ModelInfo& info) const {
    backends::BackendOpsContext ctx;
    ctx.artifacts = this;
    return backends::ops_for(info.recipe)->plan_download(info, ctx);
}

void ModelArtifactStore::resolve_all(ModelInfo& info) const {
    for (const auto& [type, checkpoint] : info.checkpoints) {
        info.resolved_paths[type] = resolve_path(info, type, checkpoint);
    }

    // The managed cache wins: a model with a complete copy of its own never looks
    // at the extra directory.
    if (download_policy(info) == backends::DownloadPolicy::Repository && !complete(info)) {
        adopt_extra_cache(info);
    }
}

}  // namespace models
}  // namespace lemon
