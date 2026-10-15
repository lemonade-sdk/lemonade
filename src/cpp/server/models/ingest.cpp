#include <lemon/models/ingest.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/routing_policy.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/canonical_id.h>
#include <lemon/model_manager.h>
#include <lemon/model_registry.h>
#include <lemon/model_types.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/models/definition_labels.h>
#include <lemon/models/routing_parse_options.h>
#include <lemon/models/registry_documents.h>
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

ModelIngest::ModelIngest(Deps deps) : deps_(std::move(deps)) {}

const ModelArtifactStore& ModelIngest::artifacts() const { return *deps_.artifacts; }

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

// Parses an entry the way build_cache does, so two entries can be compared
// field-for-field without the two parsers drifting.
static ModelInfo model_info_from_def(const json& def) {
    ModelInfo info;
    models::parse_checkpoints(info, def);
    info.recipe = JsonUtils::get_or_default<std::string>(def, "recipe", "");
    models::parse_model_source_fields(info, def);
    return info;
}

// Load-bearing differences between a local entry and the copy the manifest
// carries, empty when they agree.
static std::string collection_component_drift(const ModelInfo& local, const json& manifest_comp) {
    ModelInfo m = model_info_from_def(manifest_comp);
    std::vector<std::string> diffs;
    if (!m.recipe.empty() && m.recipe != local.recipe) {
        diffs.push_back("recipe (manifest='" + m.recipe + "' local='" + local.recipe + "')");
    }
    if (models::registry_source_of(m) != models::registry_source_of(local)) {
        diffs.push_back("source (manifest='" + models::registry_source_of(m) +
                        "' local='" + models::registry_source_of(local) + "')");
    }
    for (const auto& [type, ck] : m.checkpoints) {
        if (ck.empty()) continue;
        auto it = local.checkpoints.find(type);
        std::string lck = (it == local.checkpoints.end()) ? "" : it->second;
        if (ck != lck) {
            diffs.push_back("checkpoint[" + type + "] (manifest='" + ck + "' local='" + lck + "')");
        }
    }
    std::string out;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (i) out += "; ";
        out += diffs[i];
    }
    return out;
}

// Locate a collection manifest in a cached registry snapshot: any *.json file whose
// content is an object with recipe == "collection.omni". Note the two halves use
// different discovery rules by design: the *initial* remote fetch is
// filename-keyed on <RepoName>.json (see fetch_pull_variants in hf_variants.cpp),
// but once the snapshot is cached this reader is content-based, so the filename is
// not load-bearing here. Returns an empty json when no manifest is cached.
static json read_cached_collection_manifest(const models::ModelArtifactStore& artifacts,
                                           const fs::path& cache_dir) {
    fs::path snap = artifacts.active_snapshot_path(cache_dir);
    if (snap.empty()) return json();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(snap, safe_dir_options, ec)) {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec)) continue;
        if (entry.path().extension() != ".json") continue;
        json candidate;
        try {
            candidate = JsonUtils::load_from_file(path_to_utf8(entry.path()));
        } catch (const std::exception&) {
            continue;
        }
        if (candidate.is_object() &&
            is_model_collection_recipe(JsonUtils::get_or_default<std::string>(candidate, "recipe", ""))) {
            return candidate;
        }
    }
    return json();
}

// Bare component name: collection files reference components by public name;
// definitions may carry a `user.` prefix from the export transform.
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
                                            DownloadProgressCallback progress) {
    download_from_registry(info, progress);
}

// Always queries the registry for the current file tree: offline and
// do_not_upgrade policy are the caller's, settled before this runs.
void ModelIngest::download_from_registry(const ModelInfo& info,
                                         DownloadProgressCallback progress_callback) const {
    const std::string main_repo_id = repo_id_from_checkpoint(info.checkpoint("main"));
    const std::string main_variant = variant_from_checkpoint(info.checkpoint("main"));
    if (main_repo_id.empty()) {
        throw std::runtime_error("Model checkpoint does not contain a repository id");
    }

    const std::string source_name = models::registry_source_of(info);
    const auto source = parse_remote_registry_source(source_name);
    const auto& registry = model_registry(source);
    const std::string source_display = remote_registry_display_name(source);
    std::map<std::string, std::string> headers = registry.auth_headers();

    const fs::path cache_root = path_from_utf8(artifacts().root());
    ensure_create_directories(cache_root);

    LOG(INFO, "ModelManager") << "Fetching repository file list from "
                               << source_display << ": " << main_repo_id << std::endl;

    std::map<std::string, RegistryRepository> repositories;
    repositories.emplace(main_repo_id, registry.fetch_repository(main_repo_id));

    std::map<std::string, std::vector<std::string>> files_to_download;
    std::vector<std::string> main_repo_files;
    for (const auto& file : repositories.at(main_repo_id).files) {
        if (!file.directory) main_repo_files.push_back(file.path);
    }
    LOG(INFO, "ModelManager") << "Repository contains " << main_repo_files.size()
                               << " files" << std::endl;

    // The update check uses the same selection helper, so a file pull would
    // download is always a file the check compares.
    files_to_download[main_repo_id] = registry_files::select_main_repo_files(
        main_repo_id, info.recipe, main_variant, main_repo_files);

    // Auxiliary checkpoints inherit the model-level registry source. This is
    // intentional: a registration has one provenance and update domain.
    // Pull must reject a malformed auxiliary checkpoint loudly, where the
    // update check (group_aux_checkpoint_variants) silently skips it.
    for (const auto& [type, checkpoint] : info.checkpoints) {
        if (type == "main" || backends::ops_for(info.recipe)->acquires_lazily(type)) continue;
        if (repo_id_from_checkpoint(checkpoint).empty() ||
            variant_from_checkpoint(checkpoint).empty()) {
            throw std::runtime_error("Additional checkpoints must contain an exact repository variant");
        }
    }
    for (const auto& [repo_id, variants] :
         registry_files::group_aux_checkpoint_variants(info.recipe, info.checkpoints)) {
        if (!repositories.count(repo_id)) {
            repositories.emplace(repo_id, registry.fetch_repository(repo_id));
        }
        const auto& repo = repositories.at(repo_id);
        for (const auto& variant : variants) {
            const bool exists = std::any_of(repo.files.begin(), repo.files.end(),
                [&](const RegistryFile& file) { return !file.directory && file.path == variant; });
            if (!exists) {
                throw std::runtime_error("Additional checkpoint file not found on " +
                                         source_display + ": " + repo_id + ":" + variant);
            }
            files_to_download[repo_id].push_back(variant);
        }
    }

    for (auto& [repo_id, files] : files_to_download) {
        (void)repo_id;
        std::set<std::string> seen;
        std::vector<std::string> unique_files;
        for (auto& filename : files) {
            if (seen.insert(filename).second) {
                unique_files.push_back(filename);
            }
        }
        files = std::move(unique_files);
    }

    int total_files = 0;
    LOG(INFO, "ModelManager") << "Identified files to download:" << std::endl;
    for (const auto& [repo_id, files] : files_to_download) {
        for (const auto& filename : files) {
            ++total_files;
            LOG(INFO, "ModelManager") << "  - " << source_name << ':' << repo_id
                                       << ':' << filename << std::endl;
        }
    }
    if (total_files == 0) {
        throw std::runtime_error("No files selected for download from " + source_display +
                                 " repository " + main_repo_id);
    }

    std::map<std::string, fs::path> repo_cache_paths;
    std::map<std::string, std::string> repo_previous_refs;
    std::map<std::string, std::string> repo_snapshot_paths;
    std::map<std::string, std::string> repo_snapshot_ids;
    std::map<std::string, std::string> repo_download_paths;

    for (const auto& [repo_id, files] : files_to_download) {
        if (files.empty()) continue;
        const auto& repo = repositories.at(repo_id);
        fs::path repo_cache = cache_root / repo_id_to_cache_dir_name(repo_id, source_name);
        ensure_create_directories(repo_cache);
        const std::string previous_ref = artifacts().active_snapshot_ref(repo_cache);
        fs::path snapshot = repo_cache / "snapshots" / repo.snapshot_id;
        ensure_create_directories(snapshot);

        repo_cache_paths[repo_id] = repo_cache;
        repo_previous_refs[repo_id] = previous_ref;
        repo_snapshot_paths[repo_id] = path_to_utf8(snapshot);
        repo_snapshot_ids[repo_id] = repo.snapshot_id;
        repo_download_paths[repo_id] = path_to_utf8(snapshot);
    }

    // Unchanged artifacts keep refs/main on the previous snapshot; ModelScope has no
    // commit pin, so its snapshots advance with the tree.
    std::set<std::string> repos_reusing_previous_snapshot;
    if (source == models::huggingface_descriptor().id) {
        for (const auto& [repo_id, files] : files_to_download) {
            if (files.empty()) continue;
            const std::string current_ref = repo_snapshot_ids.at(repo_id);
            const std::string previous_ref = repo_previous_refs.at(repo_id);
            if (previous_ref.empty() || previous_ref == current_ref) continue;

            // Union with the previous revision's listing so a file it dropped
            // isn't missed; files_to_download itself stays current-revision-only.
            // Merge, not replace: files also carries same-repo aux checkpoints.
            std::vector<std::string> comparison_files = files;
            if (repo_id == main_repo_id) {
                try {
                    const RegistryRepository previous_repo =
                        registry.fetch_repository(repo_id, previous_ref);
                    std::vector<std::string> previous_repo_files;
                    for (const auto& file : previous_repo.files) {
                        if (!file.directory) previous_repo_files.push_back(file.path);
                    }
                    comparison_files = registry_files::merge_reuse_comparison_files(
                        files, repo_id, info.recipe, main_variant, previous_repo_files,
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

    json manifest;
    manifest["repo_id"] = main_repo_id;
    manifest["registry_source"] = source_name;
    manifest["commit_hash"] = repo_snapshot_ids.at(main_repo_id);
    manifest["download_path"] = repo_snapshot_paths.at(main_repo_id);
    manifest["files_count"] = total_files;
    manifest["files"] = json::array();

    for (const auto& [repo_id, files] : files_to_download) {
        const auto& repo = repositories.at(repo_id);
        std::map<std::string, RegistryFile> metadata;
        for (const auto& file : repo.files) metadata[file.path] = file;

        for (const auto& filename : files) {
            json entry;
            entry["name"] = filename;
            entry["url"] = registry.resolve_file_url(repo_id, repo.revision, filename);
            entry["download_path"] = repo_download_paths.at(repo_id);
            const auto it = metadata.find(filename);
            entry["size"] = it == metadata.end() ? 0 : it->second.size;
            if (it != metadata.end() && !it->second.hash.empty()) {
                entry["hash"] = {
                    {"algorithm", it->second.hash_algorithm},
                    {"value", it->second.hash}
                };
            }
            manifest["files"].push_back(std::move(entry));
        }
    }

    const fs::path manifest_path =
        path_from_utf8(repo_snapshot_paths.at(main_repo_id)) / ".download_manifest.json";
    JsonUtils::save_to_file(manifest, path_to_utf8(manifest_path));
    download_from_manifest(manifest, headers, progress_callback);
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

        const auto& repo = repositories.at(repo_id);
        artifacts().record_processed_model(cache_path, info, models::Provenance{
            source_name,
            repo_id,
            repo.revision,
            active_snapshot_id,
            snapshot_id,
        });
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
                                         DownloadProgressCallback progress_callback) const {
    const DownloadPlan plan = download_plan_from_manifest(manifest);
    ArtifactTransfer transfer = ArtifactTransfer::with_default_transfers();
    transfer.execute(plan, headers, progress_callback);
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
                                 DownloadProgressCallback progress_callback) {
    std::set<std::string> visited;
    pull(model_name, model_data, do_not_upgrade, progress_callback, visited,
         false, false, false);
}

void ModelIngest::pull(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 std::set<std::string>& visited,
                                 bool register_only,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing) {
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
    bool model_registered = deps_.model_exists(model_name);

    if (!model_registered) {
        // First, check if the model exists but was filtered out (unsupported recipe)
        if (deps_.model_exists_unfiltered(model_name)) {
            // Model exists in registry but is not available on this system
            std::string filter_reason = deps_.filter_reason(model_name);
            throw std::runtime_error(
                "Model '" + model_name + "' is not available on this system. " +
                filter_reason
            );
        }

        // Model not in registry - this must be a user model registration
        // Validate it has the "user." prefix
        if (!is_user_model_name(model_name)) {
            // See UnknownModelError contract in include/lemon/model_manager.h.
            throw UnknownModelError(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }

        if (is_model_collection_recipe(actual_recipe)) {
            if (auto err = validate_collection(model_name, model_data)) {
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
        auto info = deps_.model_info(model_name);
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
                if (auto err = validate_collection(model_name, model_data)) {
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
                if (auto err = validate_collection(model_name, model_data)) {
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
        deps_.register_model(model_name, registration_data);
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

        auto info = deps_.model_info(model_name);
        std::vector<std::string> components = info.components;

        try {
            if (model_data.contains("models") && model_data["models"].is_array() &&
                model_data.contains("components") && model_data["components"].is_array()) {
                // Exported-collection format: `models` carries what `components` names.
                components = register_components(model_data["components"], model_data["models"],
                                                models::registry_source_of(info));

                // Rewrite with canonical names; the bulky payload is dropped.
                if (is_user_model_name(model_name) && !components.empty()) {
                    json reg = registration_data;
                    reg["components"] = components;
                    deps_.register_model(model_name, reg);
                }
            } else if (!repo_id.empty() && (components.empty() || !do_not_upgrade)) {
                // A non-empty checkpoint marks a registry-backed collection: the list
                // lives in the registry, so fetch it. An explicit pull refreshes so a
                // member added upstream since the last pull is picked up.
                components = resolve_components_from_manifest(
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
                deps_.unregister_model(model_name);
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
            if (!deps_.model_exists(component)) {
                LOG(WARNING, "ModelManager") << "Skipping unknown component: " << component << std::endl;
                continue;
            }
            auto comp_info = deps_.model_info(component);
            if (comp_info.downloaded && do_not_upgrade) {
                LOG(INFO, "ModelManager") << "Component already downloaded: " << component << std::endl;
                continue;
            }
            LOG(INFO, "ModelManager") << "Downloading component: " << component << std::endl;
            json comp_data = json::object();
            pull(component, comp_data, do_not_upgrade, forward, visited, false, false, false);
        }

        // A startup build saw no manifest and published an empty list with no
        // bytes; drop that so the next read rebuilds from the manifest just
        // cached instead of staying wrong until a restart.
        if (!repo_id.empty()) {
            deps_.invalidate();
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
        deps_.register_model(model_name, registration_data);
    }

    auto model_info = deps_.model_info(model_name);

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
    if (do_not_upgrade && deps_.is_downloaded(model_name)) {
        LOG(INFO, "ModelManager") << "Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }

    const auto resolved_paths_before = deps_.resolved_paths(model_info.model_name);

    fetch_model(model_info, do_not_upgrade, progress_callback);

    const auto resolved_paths_after = deps_.resolved_paths(model_info.model_name);

    if (resolved_paths_before != resolved_paths_after) {
        deps_.model_updated(model_name);
    }

}

void ModelIngest::fetch_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress_callback) {
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
    backends::ops_for(info.recipe)->download_model(info, do_not_upgrade, progress_callback, octx);

    // Update cache after successful download
    deps_.patch_downloaded(info.model_name, true);

    const std::string canonical_model_name = deps_.resolve_name(info.model_name);
    if (is_user_model_name(canonical_model_name)) {
        deps_.persist_size(canonical_model_name);
    }
}

json ModelIngest::fetch_collection_manifest(const std::string& repo_id,
                                             const std::string& registry_source,
                                             bool do_not_upgrade) {
    const std::string source = remote_registry_source_name(parse_remote_registry_source(registry_source));
    fs::path cache_dir = path_from_utf8(artifacts().root()) / repo_id_to_cache_dir_name(repo_id, source);

    // A usable manifest needs both arrays; an incomplete cached copy (e.g. a
    // stale old-format file) must not satisfy do_not_upgrade - it should
    // trigger a refresh instead.
    auto manifest_complete = [](const json& m) {
        return m.is_object() &&
               m.contains("components") && m["components"].is_array() &&
               !m["components"].empty() &&
               m.contains("models") && m["models"].is_array();
    };

    json manifest = read_cached_collection_manifest(artifacts(), cache_dir);
    bool have_cache = manifest_complete(manifest);

    bool offline = false;
    if (auto* cfg = RuntimeConfig::global()) {
        offline = cfg->offline();
    }

    if (offline) {
        if (!have_cache) {
            throw std::runtime_error(
                "Offline mode: collection definition for '" + repo_id +
                "' is not cached locally.");
        }
    } else if (!(do_not_upgrade && have_cache)) {
        // Download the collection repo (the manifest plus its other small files)
        // into the shared registry cache. A bare repo id with no variant downloads all files.
        ModelInfo manifest_info;
        manifest_info.model_name = repo_id;
        manifest_info.checkpoints["main"] = repo_id;
        try {
            manifest_info.registry_source = source;
            fetch_registry_artifacts(manifest_info, nullptr);
            manifest = read_cached_collection_manifest(artifacts(), cache_dir);
        } catch (const std::exception& e) {
            if (!have_cache) throw;
            LOG(WARNING, "ModelManager") << "Could not refresh collection manifest for "
                << repo_id << " (" << e.what() << "); using cached copy" << std::endl;
        }
    }

    if (!manifest_complete(manifest)) {
        throw std::runtime_error(
            "Collection manifest for '" + repo_id +
            "' must contain a non-empty 'components' array and a 'models' array.");
    }
    return manifest;
}

std::vector<std::string> ModelIngest::register_components(const json& component_names,
                                                           const json& component_defs,
                                                           const std::string& registry_source) {
    auto def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };

    // Two passes so register_components is atomic: validate every unknown
    // component's inline definition first, and only register them once the whole
    // list is known good. A half-defined component therefore never persists a
    // stray user.* entry that a later failure would have to roll back.
    std::vector<std::string> components;
    std::vector<std::pair<std::string, json>> pending_registrations;  // canonical, def
    for (const auto& entry : component_names) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        if (name.empty()) {
            LOG(WARNING, "ModelManager") << "Skipping collection component with an "
                << "empty name" << std::endl;
            continue;
        }

        // Find the inline definition matching this component, if any.
        json def;
        if (component_defs.is_array()) {
            for (const auto& candidate : component_defs) {
                if (def_name(candidate) == name) {
                    def = candidate;
                    break;
                }
            }
        }

        // A remote collection defines one provenance domain. Components without
        // an explicit source inherit it before drift comparison or registration.
        if (def.is_object() && !def.contains("source") && !def.contains("registry_source")) {
            def["source"] = remote_registry_source_name(
                parse_remote_registry_source(registry_source));
        }

        // Components must be regular models, not collections (backstop for the
        // remote-manifest path, which does not go through validate_collection_request).
        bool def_is_collection =
            def.is_object() && is_model_collection_recipe(def.value("recipe", std::string()));
        if (def_is_collection ||
            (deps_.model_exists(name) && is_model_collection_recipe(deps_.model_info(name).recipe))) {
            throw std::runtime_error(
                "Collection components must be regular models, not collections: '" + name + "'");
        }

        if (deps_.model_exists(name)) {
            // Local-wins: the shipped/registered definition is authoritative. Warn
            // (do not overwrite) when the inline definition has drifted.
            if (def.is_object()) {
                ModelInfo local = deps_.model_info(name);
                std::string drift = collection_component_drift(local, def);
                if (!drift.empty()) {
                    LOG(WARNING, "ModelManager") << "Collection component '" << name
                        << "' differs from the local model definition; using the local "
                        << "definition. Drift: " << drift << std::endl;
                }
            }
            components.push_back(deps_.resolve_name(name));
        } else if (def.is_object()) {
            // Unknown: add it from what the file carried, so the file is self-contained.
            std::string canonical = "user." + name;
            // Manifest content is remote input - apply the same reserved-name
            // check as the CLI and /pull registration paths.
            if (is_reserved_registration_name(canonical)) {
                LOG(WARNING, "ModelManager") << "Skipping collection component with "
                    << "reserved name: " << name << std::endl;
                continue;
            }
            // Nothing has been written yet, so refusing here leaves no stray entry.
            if (!collection_component_def_is_valid(def)) {
                throw std::runtime_error(
                    "Collection component '" + name + "' has an incomplete inline "
                    "definition (a recipe and at least one checkpoint are required).");
            }
            pending_registrations.emplace_back(canonical, def);
            components.push_back(canonical);
        } else {
            LOG(WARNING, "ModelManager") << "Skipping unknown collection component with no "
                << "inline definition: " << name << std::endl;
        }
    }

    for (const auto& [canonical, def] : pending_registrations) {
        LOG(INFO, "ModelManager") << "Registering collection component as user model: "
            << canonical << std::endl;
        deps_.register_model(canonical, def);
    }
    return components;
}

std::vector<std::string> ModelIngest::resolve_components_from_manifest(
        const std::string& repo_id, const std::string& registry_source,
        bool do_not_upgrade) {
    json manifest = fetch_collection_manifest(repo_id, registry_source, do_not_upgrade);
    return register_components(manifest["components"], manifest["models"], registry_source);
}

std::optional<std::string> ModelIngest::validate_collection(
    const std::string& model_name, const json& model_data) {
    std::string checkpoint_pointer = model_data.value("checkpoint", std::string());
    if (checkpoint_pointer.empty() && model_data.contains("checkpoints") &&
        model_data["checkpoints"].is_object()) {
        checkpoint_pointer = model_data["checkpoints"].value("main", std::string());
    }
    const bool has_components =
        model_data.contains("components") && model_data["components"].is_array() &&
        !model_data["components"].empty();
    if (!has_components) {
        if (!checkpoint_pointer.empty()) {
            return std::nullopt;  // pointer-only registry-backed collection
        }
        return std::string("Collection recipe requires a non-empty 'components' array");
    }
    // An inline collection import carries its component definitions in a `models`
    // array. Every component must be *resolvable*: either it is already
    // registered locally (local-wins) or it has a matching definition in
    // `models`. Validate this per-component and fail closed. `models` being
    // present is not a blanket license - a component it omits would otherwise
    // pass here and then be silently skipped during registration, persisting a
    // collection smaller than the imported file describes. Mirror the
    // name-matching that register_components() uses (bare names; `model_name`
    // or `id` keys) so this gate rejects exactly what registration would drop.
    const bool has_models_array =
        model_data.contains("models") && model_data["models"].is_array();
    auto inline_def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };
    auto find_inline_def = [&](const std::string& bare) -> const json* {
        if (!has_models_array) return nullptr;
        for (const auto& candidate : model_data["models"]) {
            if (inline_def_name(candidate) == bare) return &candidate;
        }
        return nullptr;
    };
    const std::string bare_collection = bare_component_name(model_name);
    for (const auto& component : model_data["components"]) {
        if (!component.is_string()) {
            return std::string("components entries must be strings");
        }
        std::string component_name = component.get<std::string>();
        std::string bare = bare_component_name(component_name);
        // Self-reference: compare bare forms so a collection `user.MyCol` with
        // `components: ["MyCol"]` is rejected too, not only the exact-string case.
        if (bare == bare_collection) {
            return "Collection cannot reference itself: " + component_name;
        }
        // Components must be regular models, not collections.
        const json* def = find_inline_def(bare);
        bool is_collection_component =
            (def != nullptr && def->is_object() &&
             is_model_collection_recipe(def->value("recipe", std::string()))) ||
            (deps_.model_exists(bare) && is_model_collection_recipe(deps_.model_info(bare).recipe));
        if (is_collection_component) {
            return "Collection components must be regular models, not collections: '" +
                   component_name + "'";
        }
        if (has_models_array) {
            if (!deps_.model_exists(bare) && def == nullptr) {
                return "Inline collection component '" + component_name +
                       "' has no matching definition in 'models' and is not a "
                       "registered model. Every component must be defined inline "
                       "(matching 'model_name') or reference an already-registered "
                       "model.";
            }
            // A component that resolves to its inline definition (rather than an
            // already-registered model) must be a usable registration. Reject a
            // half-defined component here instead of registering it and failing
            // later mid-download.
            if (!deps_.model_exists(bare) && def != nullptr &&
                !collection_component_def_is_valid(*def)) {
                return "Inline collection component '" + component_name +
                       "' has an incomplete definition in 'models' (a recipe and "
                       "at least one checkpoint are required).";
            }
            // register_user_model() would throw on this once the import reached
            // registration, leaving a partly-imported collection behind. Report
            // it here, where the import can still be refused whole.
            if (!deps_.model_exists(bare) && def != nullptr) {
                std::string illegal;
                normalized_definition_labels(*def, &illegal);
                if (!illegal.empty()) {
                    return describe_illegal_labels(component_name, illegal);
                }
            }
        } else if (!deps_.model_exists(component_name)) {
            return "Collection component not registered: '" + component_name +
                   "'. Pull or register it before referencing it in a collection.";
        }
    }
    if (is_router_collection_recipe(model_data.value("recipe", std::string()))) {
        // Same resolution shape as build_cache()'s, differing only in its
        // tiers: this one resolves live and falls back to the request's own
        // inline `models[]`, because an import validates names that are not
        // registered yet. Unresolvable names are kept as authored by
        // make_routing_parse_options, so component-role validation can still
        // compare them against the authored components array.
        RoutingPolicyParseOptions options = make_routing_parse_options({
            [this](const std::string& name) -> std::optional<std::string> {
                try {
                    return deps_.resolve_name(name);
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this](const std::string& name) -> std::optional<ModelType> {
                try {
                    return deps_.model_info(name).type;
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this, &find_inline_def](const std::string& name) -> std::optional<ModelType> {
                // Not registered - the name may match an inline `models[]`
                // definition in this same request. Derive the type exactly as
                // register_user_model() would once that definition is
                // registered, so validation and runtime cannot disagree
                // (absent labels default to LLM, same as everywhere else).
                // nullopt only when no definition exists at all - the
                // "truly unknown" case.
                const json* def = find_inline_def(bare_component_name(name));
                if (!def) {
                    return std::nullopt;
                }
                std::set<std::string> label_set = normalized_definition_labels(*def);
                return get_model_type_from_labels(
                    std::vector<std::string>(label_set.begin(), label_set.end()));
            },
        });
        try {
            // Strip pull-protocol keys (stream, subscribe, do_not_upgrade, …)
            // that the client merges into the body but the policy parser does
            // not know about. Passing the raw request body would trigger the
            // reject_unknown_keys check inside parse_route_policy_collection.
            json policy_json = json::object();
            for (const auto& key : routing_policy_root_keys()) {
                if (model_data.contains(key)) policy_json[key] = model_data.at(key);
            }
            RoutePolicy policy = parse_route_policy_collection(policy_json, options);
            RoutingPolicyEngine(std::move(policy), ClassifierServices{});
        } catch (const std::exception& e) {
            return std::string("Invalid collection.router routing policy: ") + e.what();
        }
    }
    return std::nullopt;
}

void ModelIngest::populate_from_cached_manifest(ModelInfo& info) {
    std::string repo_id = info.checkpoint();
    if (repo_id.empty()) return;

    fs::path cache_dir = path_from_utf8(artifacts().root()) /
        repo_id_to_cache_dir_name(repo_id, models::registry_source_of(info));
    json manifest = read_cached_collection_manifest(artifacts(), cache_dir);
    if (!manifest.is_object() || !manifest.contains("components") ||
        !manifest["components"].is_array()) {
        return;
    }

    const models::RegistryDocuments::Pin registries = deps_.pin();

    for (const auto& entry : manifest["components"]) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        std::string canonical;
        if (registries.contains(models::RegistryDocument::Builtin, name)) {
            canonical = name;
        } else if (registries.contains(models::RegistryDocument::User, name)) {
            canonical = "user." + name;
        } else {
            canonical = name;  // not yet registered → resolves as not-downloaded
        }
        info.components.push_back(canonical);
    }

    // Surface the collection's total download size when the slim registry entry
    // omits it.
    if (info.size == 0.0 && manifest.contains("size") && manifest["size"].is_number()) {
        info.size = manifest["size"].get<double>();
    }

    // Same lift-from-manifest pattern for the optional per-collection system
    // prompt: the registry entry wins when it sets one, otherwise the published
    // remote manifest acts as the source of truth.
    if (info.system_prompt.empty() && manifest.contains("system_prompt") &&
        manifest["system_prompt"].is_string()) {
        info.system_prompt = manifest["system_prompt"].get<std::string>();
    }
}

}  // namespace models
}  // namespace lemon
