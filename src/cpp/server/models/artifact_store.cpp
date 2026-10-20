#include <lemon/models/artifact_ref.h>
#include <lemon/models/artifact_store.h>

#include <lemon/backends/backend_ops.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/backends/hf_cache_util.h>
#include <lemon/gguf_reader.h>
#include <lemon/canonical_id.h>
#include <lemon/gguf_shard_utils.h>
#include <lemon/hf_variants.h>
#include <lemon/models/registry_types.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/repository_provenance.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
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


std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A download marker reduced to what identifies the checkpoint it belongs to: bare
// file name, no .partial, no shard index, no extension.
std::string marker_key(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (ends_with(name, ".partial")) name.erase(name.size() - strlen(".partial"));
    std::string base;
    int total = 0;
    int index = 0;
    if (is_gguf_shard_filename(name, &base, &total, &index)) name = base;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

// Whether an unfinished file is the download of `variant`. A variant may be a bare
// quantization, which is only a token inside the file name, or a repo-relative path,
// which the snapshot revision sits in front of.
bool pending_matches_variant(const std::string& name, const std::string& variant) {
    if (variant.empty()) return false;
    const std::string n = lower(name);
    const std::string v = lower(variant);
    if (n == v) return true;
    if (ends_with(n, "/" + v)) return true;
    if (marker_key(n) == marker_key(v)) return true;
    const std::string token = lower(gguf_quant_token(name));
    return !token.empty() && marker_key(v).find('/') == std::string::npos && token == marker_key(v);
}

json read_json_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    json doc;
    if (!in) return doc;
    in >> doc;
    return in.fail() ? json() : doc;
}

// An interrupted download leaves .partial files and the manifest that counts them;
// a manifest on its own is still a live download.
struct DownloadMarkers {
    std::vector<fs::path> partials;
    std::vector<fs::path> manifests;
};

DownloadMarkers collect_download_markers(const fs::path& repo_dir) {
    DownloadMarkers markers;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(repo_dir, safe_dir_options, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name == ".download_manifest.json") {
            markers.manifests.push_back(entry.path());
        } else if (entry.path().extension() == ".partial") {
            markers.partials.push_back(entry.path());
        }
    }
    return markers;
}

bool manifest_lists(const json& doc, std::vector<std::string>& files) {
    if (!doc.is_object() || !doc.contains("files") || !doc["files"].is_array()) return false;
    for (const auto& entry : doc["files"]) {
        if (entry.is_string()) {
            files.push_back(entry.get<std::string>());
        } else if (entry.is_object() && entry.contains("name") && entry["name"].is_string()) {
            files.push_back(entry["name"].get<std::string>());
        }
    }
    return !files.empty();
}

bool manifest_owner(const json& doc, std::string& owner) {
    owner = doc.is_object() ? doc.value("model_name", std::string()) : std::string();
    return !owner.empty();
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

std::string repo_id_to_cache_dir_name(const std::string& repo_id,
                                      const std::string& registry_source) {
    return registry_repo_cache_dir_name(repo_id, parse_remote_registry_source(registry_source));
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
    if (info.source == local_origin::path) {
        return checkpoint;
    }

    const std::string cache = root();

    // Local uploads: the checkpoint is a path relative to the cache root. Joined
    // rather than concatenated, so a row naming an absolute path resolves to that
    // path and the delete proof can see it is not ours to remove.
    if (info.source == local_origin::upload) {
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

// The same three checks every ArtifactSource::present() applies, asked of the path this
// store resolved. Asking a source instead would derive the location a second time from
// provenance, and the two derivations part company wherever the store knows something the
// provenance rules do not — see adopt_extra_cache and collect_registered_shards.
bool ModelArtifactStore::complete(const ModelInfo& info) const {
    for (const auto& [type, checkpoint] : info.checkpoints) {
        (void)checkpoint;
        // Optionality is the backend's single rule, read through the same predicate the
        // resolver uses when it sets ArtifactRef::required.
        if (acquired_lazily(info, type)) continue;

        const Presence presence = checkpoint_bytes_present(
            info.resolved_path(type),
            [recipe = info.recipe](const std::string& path) {
                return backends::ops_for(recipe)->validate_checkpoint_file(path);
            });
        if (presence.complete) continue;

        // Bytes that are there and refused are worth a line; a model simply not
        // fetched yet is not.
        if (!presence.path.empty()) {
            LOG(WARNING, kLog) << presence.reason << "; marking model as not downloaded: "
                               << presence.path << std::endl;
        }
        return false;
    }
    return true;
}

bool ModelArtifactStore::path_complete(const std::string& path_str) {
    return checkpoint_bytes_present(path_str).complete;
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

static bool collect_registered_shards(const fs::path& resolved, const fs::path& repo_root,
                                      std::set<fs::path>& owned_paths);

namespace {

// The unfinished downloads of one repository, split into this model's own and
// somebody else's. A manifest's recorded name settles what a file name cannot, and
// reaches no further than the files that manifest lists.
struct PendingOwnership {
    std::vector<fs::path> ours;
    bool foreign = false;
};

PendingOwnership classify_pending(const fs::path& repo_dir, const std::string& variant,
                                  const std::string& model_name,
                                  const std::vector<std::string>& sibling_variants,
                                  bool unattributed_is_mine) {
    const auto claimed_by_sibling = [&sibling_variants](const std::string& name) {
        for (const auto& other : sibling_variants) {
            if (pending_matches_variant(name, other)) return true;
        }
        return false;
    };

    const DownloadMarkers markers = collect_download_markers(repo_dir);

    std::string recorded_owner;
    std::vector<json> documents;
    documents.reserve(markers.manifests.size());
    for (const fs::path& path : markers.manifests) {
        json doc = read_json_file(path);
        std::string owner;
        if (manifest_owner(doc, owner)) recorded_owner = owner;
        documents.push_back(std::move(doc));
    }
    const bool named_for_us = !recorded_owner.empty() && recorded_owner == model_name;
    const bool named_for_other = !recorded_owner.empty() && recorded_owner != model_name;

    PendingOwnership out;
    for (const fs::path& partial : markers.partials) {
        std::string target = partial.filename().string();
        target.erase(target.size() - strlen(".partial"));
        const bool ours_partial =
            unattributed_is_mine ||
            (pending_matches_variant(target, variant) &&
             (named_for_us || !claimed_by_sibling(target)) && !named_for_other);
        if (ours_partial) {
            out.ours.push_back(partial);
        } else {
            out.foreign = true;
        }
    }

    for (size_t i = 0; i < markers.manifests.size(); ++i) {
        std::vector<std::string> listed;
        const bool ours_manifest =
            named_for_us ||
            (!named_for_other && manifest_lists(documents[i], listed) &&
             !std::any_of(listed.begin(), listed.end(), claimed_by_sibling) &&
             std::any_of(listed.begin(), listed.end(), [&variant](const std::string& name) {
                 return pending_matches_variant(name, variant);
             }));
        if (ours_manifest) {
            out.ours.push_back(markers.manifests[i]);
        } else {
            out.foreign = true;
        }
    }
    return out;
}

std::vector<std::string> sibling_variants_in(const ModelInfo& victim, const std::string& repo_id,
                                             const std::string& registry_source,
                                             const KnownModels& all_models) {
    std::vector<std::string> variants;
    for (const auto& [name, other] : all_models) {
        if (name == victim.model_name || !other.source.empty()) continue;
        if (registry_source_of(other) != registry_source) continue;
        for (const auto& [type, checkpoint] : other.checkpoints) {
            (void)type;
            if (repo_id_from_checkpoint(checkpoint) == repo_id) {
                variants.push_back(variant_from_checkpoint(checkpoint));
            }
        }
    }
    return variants;
}

}  // namespace

namespace {

// Revisions, blobs and the cache's own dotfiles are not one model's bytes.
bool is_repo_plumbing(const fs::path& path, const fs::path& repo_dir) {
    std::error_code ec;
    const fs::path rel = fs::relative(path, repo_dir, ec);
    if (ec || rel.empty()) return true;
    const std::string first = rel.begin()->string();
    if (first == "refs" || first == "blobs") return true;
    return path.filename().string().find('.') == 0;
}

}  // namespace

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

    // Another application can fill a shared repository at any moment, so the delete
    // resolves again rather than trusting the cached answer.
    ModelInfo current = info;
    resolve_all(current);

    // A download that never resolved can still leave resumable .partial files and
    // a manifest behind.
    if (current.resolved_path().empty()) {
        report.cleaned_partial_download = cleanup_incomplete_download(current, all_models);
        return report;
    }

    // Nothing lemond wrote: imported bytes stay where their owner put them, and an
    // incomplete copy in a registry cache has nothing to remove.
    if (ownership.cache_dir.empty()) {
        return report;
    }

    const fs::path managed_root = utils::path_from_utf8(root());

    // A registered model is refused here, before anything is removed: the error the
    // caller gets has to mean a server that did nothing.
    bool outside = false;
    for (const std::string& type : ownership.cache_dir) {
        const std::string resolved = current.resolved_path(type);
        if (!resolved.empty() && !path_inside(utils::path_from_utf8(resolved), managed_root)) {
            outside = true;
        }
    }
    if (outside && !is_user_model_name(info.model_name)) {
        report.outside_managed_cache = true;
        return report;
    }

    for (const std::string& type : ownership.cache_dir) {
        const std::string checkpoint = info.checkpoint(type);
        const std::string resolved_str = current.resolved_path(type);
        if (resolved_str.empty()) continue;
        const fs::path resolved = utils::path_from_utf8(resolved_str);

        if (!path_inside(resolved, managed_root)) {
            report.outside_managed_cache = true;
            continue;
        }

        if (info.source == local_origin::upload) {
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

        // Who lives here is a question about the disk: rows name repositories they
        // never filled, and rows do not announce what arrived since.
        const std::string variant = variant_from_checkpoint(checkpoint);
        const std::vector<std::string> siblings =
            sibling_variants_in(info, repo, registry_source, all_models);

        std::set<fs::path> theirs;
        for (const auto& [name, other] : all_models) {
            if (name == info.model_name || !other.source.empty()) continue;
            if (registry_source_of(other) != registry_source) continue;
            ModelInfo fresh = other;
            resolve_all(fresh);
            for (const auto& [other_type, other_checkpoint] : other.checkpoints) {
                if (repo_id_from_checkpoint(other_checkpoint) != repo) continue;
                const std::string path = fresh.resolved_path(other_type);
                if (path.empty() || !safe_exists(path)) continue;
                collect_registered_shards(utils::path_from_utf8(path), repo_dir, theirs);
            }
        }

        const PendingOwnership pending =
            classify_pending(repo_dir, variant, info.model_name, siblings,
                             /*unattributed_is_mine=*/false);

        if (theirs.empty() && !pending.foreign) {
            if (safe_exists(repo_dir)) {
                LOG(INFO, kLog) << "Removing repository cache: " << utils::path_to_utf8(repo_dir)
                                << std::endl;
                std::error_code ec;
                fs::remove_all(repo_dir, ec);
                if (ec) {
                    throw std::runtime_error("Failed to remove repository cache '" +
                                             utils::path_to_utf8(repo_dir) + "': " + ec.message());
                }
            }
            continue;
        }

        LOG(INFO, kLog) << "Repository " << repo
                        << " still holds other models' bytes, removing only this model's"
                        << std::endl;

        std::set<fs::path> ours;
        if (safe_is_directory(resolved)) {
            // A checkpoint that resolves to a folder — a directory checkpoint, or a
            // repository with no refs/main — never takes the folder with it.
            std::error_code ec;
            for (const auto& entry :
                 fs::recursive_directory_iterator(resolved, safe_dir_options, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec) || ec) continue;
                const fs::path& path = entry.path();
                if (is_repo_plumbing(path, repo_dir) || path.extension() == ".partial") continue;
                if (siblings.empty()) {
                    ours.insert(path);
                } else if (pending_matches_variant(path.filename().string(), variant)) {
                    collect_registered_shards(path, repo_dir, ours);
                }
            }
        } else {
            collect_registered_shards(resolved, repo_dir, ours);
        }

        for (const fs::path& path : ours) {
            if (theirs.count(path) || !safe_exists(path)) continue;
            cleanup_orphaned_blobs_under(path, repo_dir);
            remove_path_or_throw(path, "variant path");
            cleanup_empty_parents(path, repo_dir);
        }
        for (const fs::path& path : pending.ours) {
            if (!safe_exists(path)) continue;
            remove_file_or_throw(path, "incomplete model download partial");
            cleanup_empty_parents(path, repo_dir);
        }
    }

    // A model served from elsewhere can still have left its own bytes here; refusing
    // to follow it out of the managed cache is not a refusal to clean the cache.
    if (outside) {
        report.cleaned_partial_download = cleanup_incomplete_download(current, all_models);
    }

    return report;
}

// Whatever a cancelled download left behind.
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

    const std::vector<std::string> siblings =
        sibling_variants_in(info, main_repo, registry_source, all_models);
    const std::string variant = variant_from_checkpoint(main_checkpoint);
    PendingOwnership pending = classify_pending(repo_dir, variant, info.model_name, siblings,
                                                /*unattributed_is_mine=*/siblings.empty());

    // While somebody else names this repository, a complete file in it is theirs
    // unless this model can name it too.
    if (!siblings.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(repo_dir, safe_dir_options, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) continue;
            const std::string filename = entry.path().filename().string();
            if (entry.path().extension() == ".partial" || filename == ".download_manifest.json") {
                continue;
            }
            if (!ModelArtifactStore::path_complete(utils::path_to_utf8(entry.path()))) continue;
            if (!pending_matches_variant(filename, variant)) {
                pending.foreign = true;
                break;
            }
        }
    }

    if (!pending.foreign) {
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

    bool removed_any = false;
    for (const fs::path& path : pending.ours) {
        if (!safe_exists(path)) continue;
        LOG(INFO, kLog) << "Removing leftover of an interrupted download: "
                        << utils::path_to_utf8(path) << std::endl;
        if (path.extension() == ".partial") {
            remove_file_or_throw(path, "incomplete model download partial");
        } else {
            remove_file_or_throw(path, "stale download manifest");
        }
        cleanup_empty_parents(path, repo_dir);
        removed_any = true;
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
    return read_active_snapshot_ref(repo_cache_dir);
}

fs::path ModelArtifactStore::active_snapshot_path(const fs::path& repo_cache_dir) const {
    return read_active_snapshot_path(repo_cache_dir);
}

void ModelArtifactStore::write_active_snapshot(const fs::path& repo_cache_dir,
                                               const std::string& commit_hash) const {
    write_active_snapshot_ref(repo_cache_dir, commit_hash);
}

void ModelArtifactStore::remove_unused_snapshot(const fs::path& repo_cache_dir,
                                                const std::string& snapshot_dir,
                                                const std::string& active_ref) const {
    models::remove_unused_snapshot(repo_cache_dir, snapshot_dir, active_ref);
}

std::string ModelArtifactStore::processed_snapshot_id(const fs::path& repo_cache_dir,
                                                      const ModelInfo& info) const {
    return models::processed_snapshot_id(repo_cache_dir, info);
}

void ModelArtifactStore::record_processed_model(const fs::path& repo_cache_dir,
                                                const ModelInfo& info,
                                                const Provenance& provenance,
                                                const std::string& checkpoint_type) const {
    models::record_processed_model(repo_cache_dir, info, provenance, checkpoint_type);
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

// Only a complete cache reclaims a registered identity; loose GGUFs stay extra.*.
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
    candidate.source = local_origin::extra_models_dir;
    std::set<fs::path> matched_paths;
    const bool all_complete = [&]() -> bool {
        try {
            // Main first: a later role may invalidate the registration, and the
            // shard rule below still needs the main family to have been matched.
            for (bool main_pass : {true, false}) {
                for (const auto& [type, checkpoint] : info.checkpoints) {
                    if (acquired_lazily(info, type) || (type == "main") != main_pass) continue;
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
                            own_repo ? expected_repo : main_repo, matched_paths)) {
                        return false;
                    }
                }
            }
            return complete(candidate);
        } catch (const std::exception&) {
            return false;  // An optional directory must never break catalog discovery.
        }
    }();

    if (registered_paths) {
        for (const auto& path : matched_paths) {
            // A partial family stays excluded — see collect_registered_shards.
            if (all_complete || is_gguf_shard_filename(path.filename().string(), nullptr)) {
                registered_paths->insert(path);
            }
        }
    }
    if (!all_complete) return false;
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

// Replace the static registry size with the aggregate on-disk size once the
// files exist, so directory-checkpoint models (whose repos can carry more than
// the registry estimate) report what was actually downloaded. A sharded GGUF
// resolves to a single shard, which for unsloth-style layouts is a small stub,
// so the whole family is summed instead.
void ModelArtifactStore::refresh_on_disk_size(ModelInfo& info) {
    uintmax_t total_size = 0;
    for (auto& [type, path] : info.resolved_paths) {
        (void)type;
        fs::path resolved = utils::path_from_utf8(path);
        uintmax_t shard_total = sharded_gguf_size_bytes(resolved);
        total_size += (shard_total > 0) ? shard_total : models::ModelArtifactStore::size_bytes(resolved);
    }
    if (total_size == 0) {
        return;
    }
    double file_size_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);
    if (file_size_gb < 1.0) {
        info.size = std::round(file_size_gb * 1000) / 1000;
    } else if (file_size_gb < 10.0) {
        info.size = std::round(file_size_gb * 100) / 100;
    } else {
        info.size = std::round(file_size_gb * 10) / 10;
    }
}

}  // namespace models

}  // namespace lemon
