#include <lemon/models/artifact_ref.h>
#include <lemon/models/ingest.h>
#include <lemon/models/ingest_collections.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/routing_policy.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/canonical_id.h>
#include <lemon/models/model_errors.h>
#include <lemon/models/model_info.h>
#include <lemon/models/registry_types.h>
#include <lemon/model_types.h>
#include <lemon/models/artifact_resolver.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/artifact_layout.h>
#include <lemon/models/change_notifier.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/models/routing_parse_options.h>
#include <lemon/models/registry_documents.h>
#include <lemon/models/store.h>
#include <lemon/registry_files.h>
#include <lemon/runtime_config.h>
#include <lemon/system_info.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace models {

ModelIngest::ModelIngest(Deps deps) : deps_(std::move(deps)) {
    collections_ = std::make_unique<IngestCollections>(deps_, *this);
}

// Defined here so the unique_ptr sees a complete IngestCollections.
ModelIngest::~ModelIngest() = default;

const ModelArtifactStore& ModelIngest::artifacts() const { return *deps_.artifacts; }

std::optional<std::string> ModelIngest::validate_collection(const std::string& model_name,
                                                            const json& model_data) {
    return collections_->validate(model_name, model_data);
}

void ModelIngest::populate_from_cached_manifest(ModelInfo& info) {
    collections_->populate_from_cached_manifest(info);
}

ModelInfo ModelIngest::require_info(const std::string& name) const {
    return *deps_.store->info(name);
}

bool ModelIngest::registered_anywhere(const std::string& name) const {
    const RegistryDocuments::Pin registries = deps_.registries->pin();
    if (find_registry_row(registries, name)) {
        return true;
    }
    return static_cast<bool>(find_registry_row(registries, deps_.store->resolve_name(name)));
}

void ModelIngest::persist_size(const std::string& canonical_model_name) const {
    const ModelInfo* model = deps_.store->read()->find(canonical_model_name);
    if (model == nullptr) {
        return;
    }
    const bool wrote = deps_.registries->update_user([&](json& user_models) {
        auto entry = user_models.find(strip_user_model_prefix(canonical_model_name));
        if (entry == user_models.end()) {
            return false;
        }
        (*entry)["size"] = model->size;
        return true;
    });
    if (wrote) {
        deps_.store->invalidate();
    }
}

std::string bare_component_name(const std::string& name) {
    return is_user_model_name(name) ? strip_user_model_prefix(name) : name;
}

// Fails an inline import closed rather than persisting a half-defined component.
bool collection_component_def_is_valid(const json& def) {
    if (!def.is_object() || !def.contains("recipe") || !def["recipe"].is_string()) {
        return false;
    }
    if (def["recipe"].get<std::string>().empty()) {
        return false;
    }
    if (def.contains("checkpoint") && def["checkpoint"].is_string() &&
        !def["checkpoint"].get<std::string>().empty()) {
        return true;
    }
    if (def.contains("checkpoints") && def["checkpoints"].is_object()) {
        for (const auto& [_, value] : def["checkpoints"].items()) {
            if (value.is_string() && !value.get<std::string>().empty()) {
                return true;
            }
        }
    }
    return false;
}

namespace {

static std::string join_conflict_parts(const std::vector<std::string>& parts) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "; ";
        result += parts[i];
    }
    return result;
}

static std::string requested_registry_source(const json& requested) {
    const std::string public_source = requested.value("source", std::string());
    const std::string explicit_source = requested.value("registry_source", std::string());

    std::string normalized_explicit;
    if (!explicit_source.empty()) {
        normalized_explicit = remote_registry_source_name(
            parse_remote_registry_source(explicit_source));
    }

    if (is_remote_registry_source(public_source)) {
        const std::string normalized_public = remote_registry_source_name(
            parse_remote_registry_source(public_source));
        if (!normalized_explicit.empty() && normalized_explicit != normalized_public) {
            throw std::invalid_argument(
                "Model source and registry_source identify different registries");
        }
        return normalized_public;
    }

    return normalized_explicit;
}

static std::string describe_registration_conflict(const ModelInfo& existing,
                                                  const json& requested) {
    std::vector<std::string> diffs;

    auto add_diff = [&diffs](const std::string& field,
                             const std::string& current_value,
                             const std::string& requested_value) {
        if (!requested_value.empty() && requested_value != current_value) {
            diffs.push_back(field + " (existing='" + current_value +
                            "', requested='" + requested_value + "')");
        }
    };

    std::string requested_main;
    if (requested.contains("checkpoints") && requested["checkpoints"].is_object()) {
        requested_main = requested["checkpoints"].value("main", std::string());
        for (const auto& [role, value] : requested["checkpoints"].items()) {
            if (!value.is_string()) continue;
            add_diff("checkpoint[" + role + "]",
                     existing.checkpoint(role),
                     value.get<std::string>());
        }
    } else {
        requested_main = requested.value("checkpoint", std::string());
        add_diff("checkpoint", existing.checkpoint(), requested_main);
    }

    const std::string requested_recipe = requested.value("recipe", std::string());
    add_diff("recipe", existing.recipe, requested_recipe);

    const std::string requested_source = requested_registry_source(requested);
    add_diff("source", models::registry_source_of(existing), requested_source);

    const std::string requested_mmproj = requested.value("mmproj", std::string());
    if (!requested_mmproj.empty()) {
        const std::string main_for_mmproj = requested_main.empty()
            ? existing.checkpoint()
            : requested_main;
        add_diff("mmproj",
                 existing.checkpoint("mmproj"),
                 models::legacy_mmproj_to_checkpoint(main_for_mmproj, requested_mmproj));
    }

    return join_conflict_parts(diffs);
}

// Load-bearing differences between a local entry and the copy the manifest
// carries, empty when they agree.
}  // namespace

RegistryId ModelIngest::download_source_for(const ModelInfo& info) const {
    if (!info.registry_source.empty()) {
        return parse_remote_registry_source(info.registry_source);
    }
    if (deps_.default_registry_source) {
        if (const auto configured = deps_.default_registry_source()) {
            return *configured;
        }
    }
    return models::huggingface_descriptor().id;
}

void ModelIngest::fetch_registry_artifacts(const ModelInfo& info,
                                           DownloadProgressCallback progress,
                                           const utils::CancelToken& cancel) {
    download_from_registry(info, progress, cancel);
}

// Always queries the registry for the current file tree: offline and
// do_not_upgrade policy are the caller's, settled before this runs.
void ModelIngest::download_from_registry(const ModelInfo& info,
                                         DownloadProgressCallback progress_callback,
                                         const utils::CancelToken& cancel) const {
    const std::string main_repo_id = repo_id_from_checkpoint(info.checkpoint("main"));
    const std::string main_variant = variant_from_checkpoint(info.checkpoint("main"));

    const std::string source_name = models::registry_source_of(info);
    const auto source = parse_remote_registry_source(source_name);
    const std::string source_display = remote_registry_display_name(source);

    // Selection is the resolver's, and it is asked before anything touches the filesystem:
    // a model whose checkpoint names no repository fails here, not after a directory.
    const std::vector<ArtifactRef> refs = deps_.resolver->refs_for(info);

    const fs::path cache_root = path_from_utf8(artifacts().root());
    ensure_create_directories(cache_root);

    // One placement for the whole pull: every checkpoint here belongs to the same model,
    // so the same recipe decides the file selection of every repository it reads.
    SourcePlacement placement;
    placement.recipe = info.recipe;

    std::map<std::string, std::unique_ptr<FetchableRepository>> repositories;
    std::map<std::string, RepoListing> listings;
    for (const auto& ref : refs) {
        if (listings.count(ref.repo_id)) continue;
        std::unique_ptr<FetchableRepository> repository =
            deps_.resolver->repository_for(ref, placement);
        listings.emplace(ref.repo_id, repository->list(ref.repo_id, ref.revision));
        repositories.emplace(ref.repo_id, std::move(repository));
    }

    // The existence rule stays the pull's own: its words tell the user which
    // host refused, where the plan builder's generic words do not.
    for (const auto& ref : refs) {
        if (ref.checkpoint_type == "main") continue;
        const auto& listing = listings.at(ref.repo_id);
        const bool exists = std::any_of(listing.files.begin(), listing.files.end(),
            [&](const RegistryFile& file) { return !file.directory && file.path == ref.variant; });
        if (!exists) {
            throw std::runtime_error("Additional checkpoint file not found on " +
                                     source_display + ": " + ref.repo_id + ":" + ref.variant);
        }
    }

    // Selection lives in one builder. The update check calls the same selector,
    // so a file pull would download is always a file the check compares.
    struct PlannedFile {
        std::string repo_id;
        DownloadFile file;
    };
    std::vector<PlannedFile> selected;
    std::set<std::pair<std::string, std::string>> seen;  // (repository, name)
    for (const auto& ref : refs) {
        const FetchableRepository& repository = *repositories.at(ref.repo_id);
        const RepoListing& listing = listings.at(ref.repo_id);
        Placement where;
        where.cache_dir =
            path_to_utf8(cache_root / repository.cache_dir_name(ref.repo_id));
        where.revision =
            listing.revision.id.empty() ? listing.download_revision : listing.revision.id;
        const DownloadPlan part =
            registry_download_plan(repository, ref, hf_hub_layout(), where, listing);
        for (const DownloadFile& file : part.files) {
            if (seen.insert({ref.repo_id, file.name}).second) {
                selected.push_back(PlannedFile{ref.repo_id, file});
            }
        }
    }

    const int total_files = static_cast<int>(selected.size());
    LOG(INFO, "ModelManager") << "Identified files to download:" << std::endl;
    for (const auto& planned : selected) {
        LOG(INFO, "ModelManager") << "  - " << source_name << ':' << planned.repo_id
                                  << ':' << planned.file.name << std::endl;
    }
    if (total_files == 0) {
        throw std::runtime_error("No files selected for download from " + source_display +
                                 " repository " + main_repo_id);
    }

    std::set<std::string> repo_ids;
    for (const auto& planned : selected) repo_ids.insert(planned.repo_id);

    std::map<std::string, fs::path> repo_cache_paths;
    std::map<std::string, std::string> repo_previous_refs;
    std::map<std::string, std::string> repo_snapshot_paths;
    std::map<std::string, std::string> repo_snapshot_ids;
    std::map<std::string, std::string> repo_download_paths;

    for (const auto& repo_id : repo_ids) {
        FetchableRepository& repository = *repositories.at(repo_id);
        const RepoListing& listing = listings.at(repo_id);
        fs::path repo_cache = cache_root / repository.cache_dir_name(repo_id);
        ensure_create_directories(repo_cache);
        const std::string previous_ref = artifacts().active_snapshot_ref(repo_cache);
        const std::string snapshot_id =
            listing.revision.id.empty() ? listing.download_revision : listing.revision.id;
        fs::path snapshot = repo_cache / "snapshots" / snapshot_id;
        ensure_create_directories(snapshot);

        repo_cache_paths[repo_id] = repo_cache;
        repo_previous_refs[repo_id] = previous_ref;
        repo_snapshot_paths[repo_id] = path_to_utf8(snapshot);
        repo_snapshot_ids[repo_id] = snapshot_id;
        repo_download_paths[repo_id] = path_to_utf8(snapshot);
    }

    // Headers are the host's fact; the source carries it.
    std::map<std::string, std::string> headers = repositories.at(main_repo_id)->auth_headers();

    // Unchanged artifacts keep refs/main on the previous snapshot; ModelScope has no
    // commit pin, so its snapshots advance with the tree.
    std::set<std::string> repos_reusing_previous_snapshot;
    if (source == models::huggingface_descriptor().id) {
        for (const auto& repo_id : repo_ids) {
            const std::string current_ref = repo_snapshot_ids.at(repo_id);
            const std::string previous_ref = repo_previous_refs.at(repo_id);
            if (previous_ref.empty() || previous_ref == current_ref) continue;

            // Union with the previous revision's listing so a file it dropped
            // isn't missed; the selection itself stays current-revision-only.
            // Merge, not replace: the selection also carries same-repo aux checkpoints.
            std::vector<std::string> comparison_files;
            for (const auto& planned : selected) {
                if (planned.repo_id == repo_id) comparison_files.push_back(planned.file.name);
            }
            if (repo_id == main_repo_id) {
                try {
                    const RepoListing previous_listing =
                        repositories.at(repo_id)->list(repo_id, previous_ref);
                    std::vector<std::string> previous_repo_files;
                    for (const auto& file : previous_listing.files) {
                        if (!file.directory) previous_repo_files.push_back(file.path);
                    }
                    std::vector<std::string> main_repo_files;
                    for (const auto& file : listings.at(repo_id).files) {
                        if (!file.directory) main_repo_files.push_back(file.path);
                    }
                    comparison_files = registry_files::merge_reuse_comparison_files(
                        comparison_files, repo_id, info.recipe, main_variant, previous_repo_files,
                        main_repo_files);
                } catch (const std::exception& e) {
                    LOG(DEBUG, "ModelManager")
                        << "Could not confirm no files were removed from " << repo_id
                        << " since " << previous_ref << ", re-downloading: " << e.what()
                        << std::endl;
                    continue;
                } catch (...) {
                    LOG(DEBUG, "ModelManager")
                        << "Could not confirm no files were removed from " << repo_id
                        << " since " << previous_ref << ", re-downloading" << std::endl;
                    continue;
                }
            }

            const auto current_metadata = registry_files::fetch_file_metadata_for_ref(
                repo_id, current_ref, comparison_files, headers, nullptr);
            const auto previous_metadata = registry_files::fetch_file_metadata_for_ref(
                repo_id, previous_ref, comparison_files, headers, nullptr);
            const fs::path previous_snapshot =
                repo_cache_paths.at(repo_id) / "snapshots" / previous_ref;
            if (registry_files::can_reuse_previous_hf_snapshot(
                    repo_id, comparison_files, previous_snapshot, current_metadata,
                    previous_metadata)) {
                repo_download_paths[repo_id] = path_to_utf8(previous_snapshot);
                repos_reusing_previous_snapshot.insert(repo_id);
                LOG(INFO, "ModelManager") << "Keeping active Hugging Face snapshot for "
                    << repo_id << " at " << previous_ref
                    << " because selected artifacts are unchanged in " << current_ref
                    << std::endl;
            }
        }
    }

    // The manifest is the merged plan, written for the resume and collection
    // paths, then handed to the one place a plan is executed.
    json manifest;
    manifest["repo_id"] = main_repo_id;
    manifest["registry_source"] = source_name;
    manifest["commit_hash"] = repo_snapshot_ids.at(main_repo_id);
    manifest["download_path"] = repo_snapshot_paths.at(main_repo_id);
    manifest["files_count"] = total_files;
    manifest["files"] = json::array();

    for (auto& planned : selected) {
        planned.file.download_path = repo_download_paths.at(planned.repo_id);
        json entry;
        entry["name"] = planned.file.name;
        entry["url"] = planned.file.url;
        entry["download_path"] = planned.file.download_path;
        entry["size"] = planned.file.size;
        if (!planned.file.expected_hash.empty()) {
            entry["hash"] = {
                {"algorithm", planned.file.expected_hash_algorithm},
                {"value", planned.file.expected_hash}
            };
        }
        manifest["files"].push_back(std::move(entry));
    }

    const fs::path manifest_path =
        path_from_utf8(repo_snapshot_paths.at(main_repo_id)) / ".download_manifest.json";
    JsonUtils::save_to_file(manifest, path_to_utf8(manifest_path));
    download_from_manifest(manifest, headers, progress_callback, cancel);
    std::error_code manifest_ec;
    fs::remove(manifest_path, manifest_ec);

    for (const auto& [repo_id, snapshot_id] : repo_snapshot_ids) {
        const fs::path& cache_path = repo_cache_paths.at(repo_id);
        const bool reusing_previous_snapshot =
            repos_reusing_previous_snapshot.count(repo_id) != 0;
        const std::string active_snapshot_id = reusing_previous_snapshot
            ? repo_previous_refs.at(repo_id)
            : snapshot_id;

        artifacts().write_active_snapshot(cache_path, active_snapshot_id);
        if (reusing_previous_snapshot) {
            artifacts().remove_unused_snapshot(
                cache_path, repo_snapshot_paths.at(repo_id), active_snapshot_id);
        }

        const RepoListing& listing = listings.at(repo_id);
        std::string checkpoint_type = "main";
        if (repo_id != main_repo_id) {
            for (const auto& ref : refs) {
                if (ref.repo_id == repo_id && ref.checkpoint_type != "main") {
                    checkpoint_type = ref.checkpoint_type;
                    break;
                }
            }
        }
        artifacts().record_processed_model(cache_path, info, models::Provenance{
            source_name,
            repo_id,
            listing.download_revision,
            active_snapshot_id,
            snapshot_id,
        }, checkpoint_type);
    }

    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);
    }

    LOG(INFO, "ModelManager") << "All files downloaded and validated from "
                               << source_display << std::endl;
    LOG(INFO, "ModelManager") << "Download location: "
        << repo_download_paths.at(main_repo_id) << std::endl;
}

void ModelIngest::download_from_manifest(const json& manifest,
                                         std::map<std::string, std::string>& headers,
                                         DownloadProgressCallback progress_callback,
                                         const utils::CancelToken& cancel) const {
    const DownloadPlan plan = download_plan_from_manifest(manifest);
    ArtifactTransfer transfer = ArtifactTransfer::with_default_transfers();
    transfer.execute(plan, headers, progress_callback, cancel);
}


void ModelIngest::register_model(const std::string& model_name,
                                 const json& model_data,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing) {
    std::set<std::string> visited;
    pull(model_name, model_data, true, nullptr, visited,
         true, allow_missing_checkpoint, replace_existing);
}

void ModelIngest::pull(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 const utils::CancelToken& cancel) {
    std::set<std::string> visited;
    pull(model_name, model_data, do_not_upgrade, progress_callback, visited,
         false, false, false, cancel);
}

void ModelIngest::pull(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 std::set<std::string>& visited,
                                 bool register_only,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing,
                                 const utils::CancelToken& cancel) {
    // Keep a mutable registration payload so legacy re-pulls that omit the
    // registry retain the source recorded on the existing model. The original
    // request remains untouched for validation and download semantics.
    json registration_data = model_data;
    std::string actual_checkpoint;

    if (model_data.contains("checkpoints")) {
        json checkpoints = model_data["checkpoints"];
        if (!checkpoints.is_object() || !checkpoints.contains("main")) {
            throw std::runtime_error("If present, the `checkpoints` property must be an object and must contain `main`");
        }

        actual_checkpoint = checkpoints.value("main", "");
    } else {
        actual_checkpoint = model_data.value("checkpoint", "");
    }

    std::string actual_recipe = model_data.value("recipe", "");

    // Either field means the caller is defining an entry, which needs the namespace.
    if (!actual_checkpoint.empty() || !actual_recipe.empty()) {
        if (!is_user_model_name(model_name)) {
            throw std::runtime_error(
                "When providing 'checkpoint' or 'recipe', the model name must include the "
                "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }
    }

    // Check if model exists in registry
    bool model_registered = deps_.store->exists(model_name);

    if (!model_registered) {
        // First, check if the model exists but was filtered out (unsupported recipe)
        if (registered_anywhere(model_name)) {
            // Model exists in registry but is not available on this system
            std::string filter_reason = deps_.store->rejection_reason(model_name);
            throw std::runtime_error(
                "Model '" + model_name + "' is not available on this system. " +
                filter_reason
            );
        }

        // Model not in registry - this must be a user model registration
        // Validate it has the "user." prefix
        if (!is_user_model_name(model_name)) {
            // See UnknownModelError contract in models/model_errors.h.
            throw UnknownModelError(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }

        if (is_model_collection_recipe(actual_recipe)) {
            if (auto err = collections_->validate(model_name, model_data)) {
                throw std::runtime_error(*err);
            }
            LOG(INFO, "ModelManager") << "Registering new collection: " << model_name << std::endl;
        } else {
            if (actual_recipe.empty()) {
                throw std::runtime_error(
                    "Model " + model_name + " is not registered with Lemonade Server. "
                    "To register it, provide the `recipe` argument."
                );
            }
            if (actual_checkpoint.empty() && !allow_missing_checkpoint) {
                throw std::runtime_error(
                    "Model " + model_name + " is not registered with Lemonade Server. "
                    "To register and install it, provide the `checkpoint` and `recipe` "
                    "arguments."
                );
            }

            // Backend-specific checkpoint validation (llamacpp: GGUF needs :variant).
            if (!actual_checkpoint.empty()) {
                if (auto err = backends::ops_for(actual_recipe)->validate_registration_checkpoint(
                        actual_checkpoint);
                    !err.empty()) {
                    throw std::runtime_error(err);
                }
            }

            LOG(INFO, "ModelManager") << "Registering new user model: " << model_name << std::endl;
        }
    } else {
        // An idempotent re-pull of the same entry is allowed; silently replacing a
        // user's own with a different remote source is what made an installed GGUF
        // variant disappear when another variant reused its generated name.
        auto info = require_info(model_name);
        if (!registration_data.contains("source") &&
            !registration_data.contains("registry_source")) {
            registration_data["source"] = models::registry_source_of(info);
        }

        const bool explicit_definition =
            !actual_recipe.empty() || !actual_checkpoint.empty() ||
            model_data.contains("checkpoints") || model_data.contains("components");
        if (register_only && replace_existing &&
            is_user_model_name(model_name) && explicit_definition) {
            if (is_model_collection_recipe(actual_recipe)) {
                if (auto err = collections_->validate(model_name, model_data)) {
                    throw std::runtime_error(*err);
                }
            }
            model_registered = false;
            LOG(INFO, "ModelManager") << "Replacing user model definition: "
                                      << model_name << std::endl;
        } else {
            bool is_collection_overwrite = is_model_collection_recipe(actual_recipe) &&
                                            model_data.contains("components");
            if (is_collection_overwrite) {
                // Validate the original user-authored request, not registration_data:
                // the latter is enriched with the persisted registry source, which is
                // not part of the public routing-policy document the parser accepts.
                if (auto err = collections_->validate(model_name, model_data)) {
                    throw std::runtime_error(*err);
                }
                model_registered = false;
                LOG(INFO, "ModelManager") << "Overwriting collection: "
                                          << model_name << std::endl;
            } else if (actual_checkpoint.empty()) {
                actual_checkpoint = info.checkpoint();
                actual_recipe = info.recipe;
            } else {
                std::string conflict = describe_registration_conflict(info, registration_data);
                if (!conflict.empty()) {
                    throw std::runtime_error(
                        "Model '" + model_name + "' is already registered with different "
                        "model metadata: " + conflict + ". Choose a different model name "
                        "for this registry checkpoint."
                    );
                }
                if (actual_recipe.empty()) {
                    actual_recipe = info.recipe;
                } else {
                    model_registered = false;
                }
            }
        }
    }

    // Parse checkpoint
    std::string repo_id = actual_checkpoint;
    std::string variant = "";

    size_t colon_pos = actual_checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = actual_checkpoint.substr(0, colon_pos);
        variant = actual_checkpoint.substr(colon_pos + 1);
    }

    // Register collections early - the fan-out below looks the entry up.
    // Track that this call created the registration so component-resolution
    // failures can roll it back instead of leaving a broken entry behind.
    bool collection_registered_this_call = false;
    if (is_model_collection_recipe(actual_recipe) && is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
        model_registered = true;
        collection_registered_this_call = true;
    }

    if (register_only && is_model_collection_recipe(actual_recipe)) {
        return;
    }

    // A collection has no backend of its own, so each member is fetched on its own.
    //
    // One persistence rule, uniform across entries: a registry row stores what was
    // *authored locally*; anything *fetched* lives in the shared model-hub cache and
    // is rebuilt on lookup. An ordinary entry keeps what it points at, not its
    // bytes; an inline collection keeps its own list; a registry-backed one keeps
    // only its pointer, the list rebuilt from the cached manifest. The one
    // exception: a fetched manifest adds its members as user models so they are
    // routable.
    if (is_model_collection_recipe(actual_recipe)) {
        // Cycle guard: re-entering the same collection on the current call
        // chain means the user registered a circular reference (e.g. user.A
        // includes user.B which includes user.A). Without this, the fan-out
        // would recurse until the stack or HTTP timeout gives out.
        if (!visited.insert(model_name).second) {
            throw std::runtime_error(
                "Cycle detected in collection components: '" + model_name +
                "' transitively references itself. Edit the offending collection "
                "in user_models.json to remove the cycle."
            );
        }

        auto info = require_info(model_name);
        std::vector<std::string> components = info.components;

        try {
            if (model_data.contains("models") && model_data["models"].is_array() &&
                model_data.contains("components") && model_data["components"].is_array()) {
                // Exported-collection format: `models` carries what `components` names.
                components = collections_->register_components(model_data["components"], model_data["models"],
                                                models::registry_source_of(info));

                // Rewrite with canonical names; the bulky payload is dropped.
                if (is_user_model_name(model_name) && !components.empty()) {
                    json reg = registration_data;
                    reg["components"] = components;
                    register_user_model(model_name, reg);
                }
            } else if (!repo_id.empty() && (components.empty() || !do_not_upgrade)) {
                // A non-empty checkpoint marks a registry-backed collection: the list
                // lives in the registry, so fetch it. An explicit pull refreshes so a
                // member added upstream since the last pull is picked up.
                components = collections_->resolve_from_manifest(
                    repo_id, models::registry_source_of(info), do_not_upgrade);
            }

            if (components.empty()) {
                throw std::runtime_error("Collection '" + model_name + "' has no components defined");
            }
        } catch (...) {
            // Roll back the early registration when component resolution fails,
            // so a failed import does not persist a collection entry whose
            // components were never resolved. Downloads below are not rolled
            // back - a mid-download failure leaves a valid, re-pullable entry.
            if (collection_registered_this_call) {
                LOG(WARNING, "ModelManager") << "Component resolution failed; unregistering "
                    << "collection: " << model_name << std::endl;
                unregister_user_model(model_name);
            }
            throw;
        }
        LOG(INFO, "ModelManager") << "Downloading " << components.size()
                                  << " component(s) for collection: " << model_name << std::endl;

        // Wrap the callback so recursive per-component downloads don't each
        // emit a "complete" event - the SSE stream should only see one final
        // completion after every component finishes.
        //
        // Capture progress_callback by value (not by reference) so `forward`
        // owns a copy of the std::function. This keeps the callback usable
        // even if `forward` is ever copied or outlives this stack frame;
        // today it's only consumed synchronously below, but the defensive
        // copy is free (std::function is copyable) and removes a latent
        // dangling-reference hazard if the recursion pattern changes.
        DownloadProgressCallback forward = nullptr;
        if (progress_callback) {
            forward = [progress_callback](const DownloadProgress& p) -> bool {
                if (p.complete) return true;
                return progress_callback(p);
            };
        }

        for (const auto& component : components) {
            if (!deps_.store->exists(component)) {
                LOG(WARNING, "ModelManager") << "Skipping unknown component: " << component << std::endl;
                continue;
            }
            auto comp_info = require_info(component);
            if (comp_info.downloaded && do_not_upgrade) {
                LOG(INFO, "ModelManager") << "Component already downloaded: " << component << std::endl;
                continue;
            }
            LOG(INFO, "ModelManager") << "Downloading component: " << component << std::endl;
            json comp_data = json::object();
            pull(component, comp_data, do_not_upgrade, forward, visited, false, false, false,
                 cancel);
        }

        // A startup build saw no manifest and published an empty list with no
        // bytes; drop that so the next read rebuilds from the manifest just
        // cached instead of staying wrong until a restart.
        if (!repo_id.empty()) {
            deps_.store->invalidate();
        }

        // Emit a single completion event for the whole collection
        if (progress_callback) {
            DownloadProgress progress;
            progress.complete = true;
            progress.percent = 100;
            (void)progress_callback(progress);
        }
        // Scope cycle detection to the current recursion stack: a sibling branch
        // that legitimately revisits this collection (DAG, e.g. A->{B,C}->D
        // where D is itself a collection) must not be misread as a cycle.
        visited.erase(model_name);
        return;
    }

    // Check if this recipe is supported on the current system
    bool disable_filtering = false;
    if (auto* cfg = RuntimeConfig::global()) {
        disable_filtering = cfg->disable_model_filtering();
    }
    std::string unsupported_reason = SystemInfo::check_recipe_supported(actual_recipe);
    if (!unsupported_reason.empty() && !disable_filtering) {
        throw std::runtime_error(
            "Model '" + model_name + "' cannot be used on this system (recipe: " + actual_recipe + "): " +
            unsupported_reason
        );
    }

    // Persist registration and recipe options BEFORE the cache-first shortcut
    // below. A registration/import/overwrite that targets an already-downloaded
    // model must still update user_models.json and recipe_options.json. The
    // do_not_upgrade fast return only skips the remote-registry download/update
    // check — it must never skip the metadata the caller asked us to record,
    // otherwise an import that reports success would silently leave the old
    // checkpoints/options in place.
    if (is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
    }

    auto model_info = require_info(model_name);

    const std::string download_source = requested_registry_source(model_data);
    if (!download_source.empty() &&
        artifacts().download_policy(model_info) == backends::DownloadPolicy::SelfManaged) {
        model_info.registry_source = download_source;
    }

    if (model_data.contains("recipe_options")) {
        // On top of the 3-level merge, so an import's own overrides win without
        // discarding the layers underneath (e.g. sdcpp_args).
        json merged = model_info.recipe_options.to_json();
        for (auto& [key, value] : model_data["recipe_options"].items()) {
            merged[key] = value;
        }
        model_info.recipe_options = RecipeOptions(model_info.recipe, merged);
        deps_.save_options(model_info);
    }

    if (register_only) {
        return;
    }

    LOG(INFO, "ModelManager") << "Downloading model: " << repo_id;
    if (!variant.empty()) {
        LOG(INFO, "ModelManager") << " (variant: " << variant << ")";
    }
    LOG(INFO, "ModelManager") << std::endl;

    // Check if offline mode
    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline()) {
            LOG(INFO, "ModelManager") << "Offline mode enabled, skipping download" << std::endl;
            return;
        }
    }

    // CRITICAL: If do_not_upgrade=true AND model is already downloaded, skip the
    // remote-registry update check. Registration and recipe options were already
    // persisted above, so an import/overwrite still takes effect on disk.
    // The do_not_upgrade flag means:
    //   - Load/inference endpoints: Don't check the remote registry for updates (use cache if available)
    //   - Pull endpoint: Always check the recorded registry for the latest version (do_not_upgrade=false)
    if (do_not_upgrade && deps_.store->is_downloaded(artifacts(), model_name)) {
        LOG(INFO, "ModelManager") << "Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }

    const auto resolved_paths_before = deps_.store->resolved_paths_for(model_info.model_name);

    fetch_model(model_info, do_not_upgrade, progress_callback, cancel);

    const auto resolved_paths_after = deps_.store->resolved_paths_for(model_info.model_name);

    if (resolved_paths_before != resolved_paths_after) {
        deps_.notifier->model_updated(model_name);
    }

}

void ModelIngest::fetch_model(const ModelInfo& info, bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 const utils::CancelToken& cancel) {
    // Serialize downloads per checkpoint repo. A second request for the same
    // repo (e.g. a client that timed out and retried /pull while the first
    // download is still running) must wait for the in-flight download instead
    // of writing the same .partial files concurrently, which corrupts them and
    // sends the hash verification into an endless retry-from-scratch loop.
    std::shared_ptr<std::mutex> repo_lock;
    {
        std::lock_guard<std::mutex> guard(download_locks_mutex_);
        auto& slot = download_locks_[
            models::registry_source_of(info) + ":" +
            repo_id_from_checkpoint(info.checkpoint("main"))];
        if (!slot) slot = std::make_shared<std::mutex>();
        repo_lock = slot;
    }
    std::lock_guard<std::mutex> download_lock(*repo_lock);

    // The backend's ops own the download (shared registry engine by default; flm pulls
    // via the flm CLI; cloud is a no-op).
    backends::BackendOpsContext octx;
    octx.fetch = this;
    octx.artifacts = deps_.artifacts;
    octx.cancel = cancel;
    backends::ops_for(info.recipe)->download_model(info, do_not_upgrade, progress_callback, octx);

    // Update cache after successful download
    deps_.store->patch_download(*deps_.artifacts, info.model_name, true);

    const std::string canonical_model_name = deps_.store->resolve_name(info.model_name);
    if (is_user_model_name(canonical_model_name)) {
        persist_size(canonical_model_name);
    }
}


static const std::vector<std::string> USER_DEFINED_MODEL_PROPS = std::vector<std::string>{
    "checkpoints", "checkpoint", "recipe", "mmproj", "size",
    "image_defaults", "audio_defaults", "components", "recipe_options",
    "routing", "system_prompt", "version", "source", "registry_source",
    "auto_update"
};

static bool user_entry_is_router_collection(const json& user_models,
                                            const std::string& key) {
    if (!user_models.is_object() || !user_models.contains(key)) {
        return false;
    }
    return is_router_collection_recipe(
        user_models.at(key).value("recipe", std::string()));
}

void ModelIngest::register_user_model(const std::string& model_name,
                                      const json& model_data,
                                      const std::string& source) {
    const std::string recipe = model_data.value("recipe", std::string());

    // Remove "user." prefix if present
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    // Filter only known, user-definable model props
    json model_entry;
    for (const std::string& prop : USER_DEFINED_MODEL_PROPS) {
        if (model_data.contains(prop)) {
            model_entry[prop] = model_data[prop];
        }
    }
    // Every registration path funnels through here — direct registration, an
    // imported collection's inline components, re-registration — so this is the
    // one gate that refuses an illegal set of mode labels. Loading an
    // already-persisted entry does not come through here; it is checked again on
    // load, where an entry written by an older version is skipped rather than
    // blocking startup.
    std::string illegal;
    std::set<std::string> labels = normalized_definition_labels(model_data, &illegal);
    if (!illegal.empty()) {
        throw InvalidModelDefinitionError(describe_illegal_labels(model_name, illegal));
    }

    model_entry["labels"] = labels;
    model_entry["suggested"] = true; // Always set suggested=true for user models

    if (!source.empty()) {
        model_entry["source"] = source;
    }

    // A registry-backed collection persists only its repo pointer, so a refreshed
    // manifest cannot be shadowed by a stale local list.
    if (is_model_collection_recipe(recipe)) {
        std::string pointer = model_entry.value("checkpoint", std::string());
        if (pointer.empty() && model_entry.contains("checkpoints") &&
            model_entry["checkpoints"].is_object()) {
            pointer = model_entry["checkpoints"].value("main", std::string());
        }
        if (!pointer.empty()) {
            model_entry.erase("components");
        }
    }

    // update_user re-reads the file, so a concurrent pull's entry survives this
    // one — a lost registration is a hard "Model not found" on the next
    // auto-load. The store write keeps save and invalidation one step.
    bool overwrote_router_collection = false;
    {
        ModelStore::Write store_write(*deps_.store);
        deps_.registries->update_user([&](json& user_models) {
            overwrote_router_collection =
                user_entry_is_router_collection(user_models, clean_name);
            user_models[clean_name] = model_entry;
            return true;
        });
        store_write.invalidate_cache();
    }

    // A router collection's policy also retires when a plain recipe replaces it, and
    // only a reconcile notices that. An ordinary model — the collection's own component
    // included — must not trigger one.
    if (is_router_collection_recipe(recipe) || overwrote_router_collection) {
        deps_.notifier->models_changed(deps_.store->current()->generation);
    }
}

void ModelIngest::unregister_user_model(const std::string& model_name) {
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    bool was_router_collection = false;
    {
        ModelStore::Write store_write(*deps_.store);
        const bool wrote = deps_.registries->update_user([&](json& user_models) {
            if (!user_models.contains(clean_name)) {
                return false;
            }
            was_router_collection =
                user_entry_is_router_collection(user_models, clean_name);
            user_models.erase(clean_name);
            return true;
        });
        if (!wrote) {
            return;
        }
        store_write.invalidate_cache();
    }

    if (was_router_collection) {
        deps_.notifier->models_changed(deps_.store->current()->generation);
    }
}

// Drop the model's own registration, if it has one. Rows the catalog re-creates
// have none.
void ModelIngest::forget_registered_model(const std::string& canonical_model_name) {
    if (!is_user_model_name(canonical_model_name)) {
        return;
    }
    ModelStore::Write store_write(*deps_.store);
    deps_.registries->update_user([&](json& user_models) {
        user_models.erase(strip_user_model_prefix(canonical_model_name));
        return true;
    });
    store_write.invalidate_cache();
    LOG(INFO, "ModelIngest") << "\u2713 Removed from user_models.json" << std::endl;
}

}  // namespace models
}  // namespace lemon
