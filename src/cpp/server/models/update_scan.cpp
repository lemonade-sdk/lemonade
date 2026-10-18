#include <lemon/models/update_scan.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/model_manager.h>
#include <lemon/model_registry.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/store.h>
#include <lemon/models/store.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/registry_files.h>
#include <lemon/runtime_config.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

// One round trip per repository answers for every model selecting from it.
struct RepoEntry {
    std::vector<std::string> model_names;
    std::string repo_id;
    std::string registry_source;
};

}  // namespace

UpdateScanResult run_update_scan(const UpdateScanDeps& deps,
                                 const std::vector<std::string>& targets) {
    if (deps.scan_override) {
        UpdateScanResult res = deps.scan_override(targets);
        if (deps.phase) {
            for (const auto& m : res.up_to_date_models) {
                deps.phase("registry_check:" + m);
            }
            for (const auto& m : res.updated_models) {
                deps.phase("registry_check:" + m);
            }
            for (const auto& [m, _] : res.failed_models) {
                deps.phase("registry_check:" + m);
            }
        }
        return res;
    }

    const auto public_name = [&deps](const std::string& canonical) {
        const std::string pub = deps.snapshot()->public_name_for(canonical);
        return pub.empty() ? canonical : pub;
    };

    UpdateScanResult result;

    if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
        LOG(DEBUG, "ModelManager")
            << "Offline mode enabled, skipping model update check" << std::endl;
        return result;
    }

    std::unordered_map<std::string, RepoEntry> repos;
    std::unordered_map<std::string, ModelInfo> candidate_models;
    registry_files::DeterminationTracker determination_tracker;
    // Fast path only — never the comparison baseline (see active_local_snapshot).
    std::unordered_map<std::string, std::string> processed_snapshots;

    deps.ensure_store();

    {
        auto snapshot = deps.snapshot();
        std::unordered_set<std::string> target_canonicals;
        for (const auto& t : targets) {
            std::string canonical_name = snapshot->canonical_for(t);
            target_canonicals.insert(canonical_name.empty() ? t : canonical_name);
        }

        for (const auto& [name, info] : snapshot->models) {
            if (!info.downloaded) {
                continue;
            }

            if (!target_canonicals.empty() && target_canonicals.find(name) == target_canonicals.end()) {
                continue;
            }

            // FLM and cloud models are managed by their own backends.
            if (info.recipe == "flm" || info.recipe == "cloud") {
                continue;
            }

            // Local-path, local-upload and extra-directory models have no
            // remote registry to check.
            if (!info.source.empty()) {
                continue;
            }

            const std::string main_cp = info.checkpoint("main");
            if (main_cp.empty()) {
                continue;
            }

            const std::string main_repo_id = repo_id_from_checkpoint(main_cp);
            if (main_repo_id.empty()) {
                continue;
            }

            const std::string source = deps.registry_source(info);

            candidate_models.emplace(name, info);

            auto add_repo_entry = [&](const std::string& repo_id) {
                auto& entry = repos[source + ":" + repo_id];
                entry.repo_id = repo_id;
                entry.registry_source = source;
                entry.model_names.push_back(name);
                determination_tracker.add_pending(name);
            };
            add_repo_entry(main_repo_id);

            // Auxiliary checkpoints can live in other repositories; those are
            // verified under their own repository entry.
            for (const auto& [repo_id, variants] :
                 registry_files::group_aux_checkpoint_variants(info.recipe, info.checkpoints)) {
                (void)variants;
                if (repo_id != main_repo_id) {
                    add_repo_entry(repo_id);
                }
            }

            const fs::path cache_path =
                utils::path_from_utf8(deps.hf_cache_dir()) /
                registry_repo_cache_dir_name(main_repo_id, parse_remote_registry_source(source));
            processed_snapshots[name] = deps.processed_snapshot_id(cache_path, name, info);
        }
    }

    std::unordered_set<std::string> updated_models;
    registry_files::HfTreePageCache tree_cache;

    for (auto& [key, entry] : repos) {
        (void)key;

        if (deps.cancel_requested && deps.cancel_requested()) {
            break;
        }

        try {
            const auto source =
                parse_remote_registry_source(entry.registry_source);
            const ModelRegistry& registry =
                deps.registry_for ? deps.registry_for(source) : model_registry(source);

            LOG(DEBUG, "ModelManager")
                << "Checking for updates on "
                << remote_registry_display_name(source)
                << ": " << entry.repo_id << std::endl;

            for (const auto& model_name : entry.model_names) {
                if (deps.phase) {
                    deps.phase("registry_check:" + model_name);
                }
            }

            const RegistryRepository latest =
                registry.fetch_repository(entry.repo_id);

            if (latest.snapshot_id.empty()) {
                for (const auto& model_name : entry.model_names) {
                    std::string pub_name = public_name(model_name);
                    result.failed_models[pub_name] = "Empty snapshot returned from registry";
                }
                continue;
            }

            const bool artifact_aware = source == models::huggingface_descriptor().id;
            const fs::path cache_path =
                utils::path_from_utf8(deps.hf_cache_dir()) /
                registry_repo_cache_dir_name(entry.repo_id, parse_remote_registry_source(entry.registry_source));
            const auto headers =
                artifact_aware ? registry.auth_headers()
                               : std::map<std::string, std::string>{};

            std::vector<std::string> latest_files;
            if (artifact_aware) {
                for (const auto& file : latest.files) {
                    if (!file.directory) latest_files.push_back(file.path);
                }
            }

            // Previous revision's file listing, fetched lazily per distinct
            // active ref. A ref that failed to fetch stays failed, so every
            // model sharing it consistently assumes changed too.
            std::map<std::string, std::vector<std::string>> previous_files_by_ref;
            std::set<std::string> previous_files_unavailable;

            size_t updated_variants = 0;
            for (const auto& model_name : entry.model_names) {
                const ModelInfo& info = candidate_models.at(model_name);
                const bool is_main_repo =
                    entry.repo_id == repo_id_from_checkpoint(info.checkpoint("main"));

                // Fast path: pull already processed this exact remote state.
                if (is_main_repo) {
                    const auto processed_it = processed_snapshots.find(model_name);
                    if (processed_it != processed_snapshots.end() &&
                        !processed_it->second.empty() &&
                        processed_it->second == latest.snapshot_id) {
                        determination_tracker.mark_determined(model_name);
                        continue;
                    }
                }

                // The comparison baseline is the snapshot on disk for THIS
                // repository, never the processed-at-pull sha.
                std::string resolved;
                if (is_main_repo) {
                    resolved = info.resolved_path("main");
                } else {
                    for (const auto& [role, checkpoint] : info.checkpoints) {
                        if (role == "main" ||
                            backends::ops_for(info.recipe)->acquires_lazily(role)) {
                            continue;
                        }
                        if (repo_id_from_checkpoint(checkpoint) == entry.repo_id) {
                            resolved = info.resolved_path(role);
                            break;
                        }
                    }
                }

                const std::string active =
                    registry_files::active_local_snapshot(resolved, cache_path);
                if (!active.empty() && active == latest.snapshot_id) {
                    determination_tracker.mark_determined(model_name);
                    continue;
                }

                // A new commit on a shared multi-artifact repository doesn't
                // imply this model changed (could be a README or a sibling
                // variant) — compare the artifacts pull would actually
                // download before advertising one. Any indeterminate result,
                // including a repository with no local baseline, assumes changed.
                bool changed = true;
                if (artifact_aware && !active.empty()) {
                    try {
                        std::vector<std::string> selected;
                        if (is_main_repo) {
                            if (previous_files_unavailable.count(active)) {
                                throw std::runtime_error(
                                    "Previous revision listing unavailable for " + entry.repo_id);
                            }
                            auto previous_it = previous_files_by_ref.find(active);
                            if (previous_it == previous_files_by_ref.end()) {
                                try {
                                    const RegistryRepository previous_repo =
                                        registry.fetch_repository(entry.repo_id, active);
                                    std::vector<std::string> previous_files;
                                    for (const auto& file : previous_repo.files) {
                                        if (!file.directory) previous_files.push_back(file.path);
                                    }
                                    previous_it = previous_files_by_ref
                                                      .emplace(active, std::move(previous_files))
                                                      .first;
                                } catch (...) {
                                    previous_files_unavailable.insert(active);
                                    throw;
                                }
                            }
                            selected = registry_files::select_main_repo_files_union(
                                entry.repo_id, info.recipe,
                                variant_from_checkpoint(info.checkpoint("main")),
                                previous_it->second, latest_files);
                        }
                        const auto aux_variants =
                            registry_files::group_aux_checkpoint_variants(info.recipe, info.checkpoints);
                        const auto aux_it = aux_variants.find(entry.repo_id);
                        if (aux_it != aux_variants.end()) {
                            selected.insert(selected.end(),
                                            aux_it->second.begin(), aux_it->second.end());
                        }

                        if (!selected.empty() &&
                            registry_files::selected_artifacts_unchanged(
                                entry.repo_id, active, latest.snapshot_id,
                                cache_path / "snapshots" / active, selected, headers,
                                &tree_cache)) {
                            changed = false;
                            LOG(DEBUG, "ModelManager")
                                << "Skipping update for " << model_name
                                << ": artifacts unchanged in "
                                << latest.snapshot_id.substr(0, 18) << ", keeping "
                                << active.substr(0, 18) << std::endl;
                        }
                    } catch (const std::exception& e) {
                        LOG(DEBUG, "ModelManager")
                            << "Artifact comparison failed for " << model_name
                            << ", assuming changed: " << e.what() << std::endl;
                    } catch (...) {
                        LOG(DEBUG, "ModelManager")
                            << "Artifact comparison failed for " << model_name
                            << ", assuming changed" << std::endl;
                    }
                } else if (active.empty()) {
                    LOG(DEBUG, "ModelManager")
                        << "No local baseline for " << model_name << " in "
                        << entry.repo_id << ", assuming changed" << std::endl;
                }

                // Only a completed determination may clear an update flag
                // discovered earlier.
                determination_tracker.mark_determined(model_name);
                if (changed) {
                    updated_models.insert(model_name);
                    ++updated_variants;
                }
            }

            if (updated_variants > 0) {
                LOG(INFO, "ModelManager")
                    << "Update available for " << entry.repo_id
                    << " on " << remote_registry_display_name(source)
                    << ": latest=" << latest.snapshot_id.substr(0, 18)
                    << " (" << updated_variants << " variant(s))" << std::endl;
            }

        } catch (const RegistryNotFoundError& e) {
            LOG(DEBUG, "ModelManager")
                << e.what() << ", skipping update check" << std::endl;
            auto snapshot = deps.snapshot();
            for (const auto& model_name : entry.model_names) {
                std::string pub_name = snapshot->public_name_for(model_name);
                if (pub_name.empty()) {
                    pub_name = model_name;
                }
                result.failed_models[pub_name] = e.what();
            }

        } catch (const std::exception& e) {
            LOG(WARNING, "ModelManager")
                << "Failed to check updates for "
                << entry.repo_id
                << " on " << entry.registry_source
                << ": " << e.what() << std::endl;
            auto snapshot = deps.snapshot();
            for (const auto& model_name : entry.model_names) {
                std::string pub_name = snapshot->public_name_for(model_name);
                if (pub_name.empty()) {
                    pub_name = model_name;
                }
                result.failed_models[pub_name] = e.what();
            }
        }
    }

    ScanDetermination settled;
    for (const auto& name : updated_models) {
        settled.updated.insert(name);
    }
    for (const auto& [name, info] : candidate_models) {
        settled.examined.push_back(name);
        if (determination_tracker.is_verified(name)) {
            settled.verified.insert(name);
        }
    }
    deps.publish(settled, result);

    std::sort(result.updated_models.begin(), result.updated_models.end());
    std::sort(result.up_to_date_models.begin(), result.up_to_date_models.end());

    if (!result.updated_models.empty()) {
        LOG(INFO, "ModelManager")
            << "Updates available for "
            << result.updated_models.size()
            << " model(s)" << std::endl;
    }

    return result;
}


void publish_scan_flags(ModelStore& store, const ScanDetermination& settled,
                        UpdateScanResult& result) {
    ModelStore::Write store_write(store);

    const auto published = store.read();
    if (!published) {
        return;  // Nothing published, so nothing was examined either
    }
    const auto wire_name = [&published](const std::string& cache_key) {
        auto name = published->canonical_public_names.find(cache_key);
        return name == published->canonical_public_names.end() ? cache_key : name->second;
    };

    for (auto& [name, info] : store_write.draft()) {
        if (settled.verified.count(name) == 0) {
            continue;
        }
        if (result.failed_models.find(wire_name(name)) != result.failed_models.end()) {
            continue;
        }
        info.update_available = settled.updated.count(name) != 0;
    }

    for (const auto& name : settled.updated) {
        const std::string pub_name = wire_name(name);
        if (result.failed_models.find(pub_name) == result.failed_models.end()) {
            result.updated_models.push_back(pub_name);
        }
    }

    for (const auto& name : settled.examined) {
        if (settled.verified.count(name) == 0 || settled.updated.count(name) != 0) {
            continue;
        }
        const std::string pub_name = wire_name(name);
        if (result.failed_models.find(pub_name) == result.failed_models.end()) {
            result.up_to_date_models.push_back(pub_name);
        }
    }
}

}  // namespace models
}  // namespace lemon
