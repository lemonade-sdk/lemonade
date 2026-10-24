#include <lemon/models/sync_service.h>

#include <lemon/models/model_info.h>
#include <lemon/models/store.h>
#include <lemon/runtime_config.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <utility>

namespace lemon {
namespace models {

SyncService::SyncService(Deps deps) : deps_(std::move(deps)), worker_([this] { run_worker(); }) {}

SyncService::~SyncService() {
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.stop = true;
        state_.cancel_requested = true;
    }
    state_.cv.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SyncService::run_worker() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(state_.mutex);
            state_.cv.wait(lock, [this] { return state_.stop || state_.is_sync_running; });
            if (state_.stop) {
                state_.worker_exited = true;
                state_.cv.notify_all();
                return;
            }
        }
        run_generation();
    }
}

UpdateScanResult SyncService::scan_for_updates(const std::vector<std::string>& targets) {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    return deps_.scan(targets);
}

json SyncService::sync(const std::vector<std::string>& target_models, bool dry_run,
                       bool attach_if_running) {
    if (dry_run) {
        LOG(INFO, "ModelManager") << "Checking models for sync/update (dry-run)..." << std::endl;
        UpdateScanResult check_res = scan_for_updates(target_models);

        std::vector<std::string> models_to_update = check_res.updated_models;
        std::vector<std::string> models_up_to_date = check_res.up_to_date_models;
        std::map<std::string, std::string> failed_models = check_res.failed_models;
        int checked_count = check_res.up_to_date_models.size() + check_res.updated_models.size();

        if (!target_models.empty()) {
            for (const auto& input_name : target_models) {
                const std::string canonical_name = deps_.store->resolve_name(input_name);
                const std::string pub_name = deps_.store->public_name_for(canonical_name);
                if (failed_models.count(pub_name) || failed_models.count(input_name)) {
                    continue;
                }
                ModelInfo info;
                try {
                    info = *deps_.store->info(canonical_name);
                } catch (const std::exception&) {
                    failed_models[input_name] = "Model not found or registered";
                    continue;
                }

                if (!info.downloaded) {
                    failed_models[input_name] = "Model is not downloaded";
                    continue;
                }
            }
        }

        json status_info = status();
        bool running = status_info.value("already_in_progress", false);
        return json{
            {"status", running ? "in_progress" : (failed_models.empty() ? "success" : "failed")},
            {"dry_run", true},
            {"already_in_progress", running},
            {"sync_id", status_info.value("sync_id", 0)},
            {"completed_sync_id", status_info.value("completed_sync_id", 0)},
            {"checked_count", checked_count},
            {"updated_count", models_to_update.size()},
            {"models_updated", models_to_update},
            {"models_up_to_date", models_up_to_date},
            {"failed_models", failed_models}
        };
    }

    const Enqueued enqueue_res = enqueue(target_models, attach_if_running);
    return wait_for(enqueue_res.sync_id);
}

json SyncService::status(uint64_t sync_id) const {
    std::lock_guard<std::mutex> lock(state_.mutex);
    if (sync_id > 0) {
        if (state_.is_sync_running && state_.current_generation == sync_id) {
            return status_locked();
        }
        auto it = state_.completed_generation_results.find(sync_id);
        if (it != state_.completed_generation_results.end()) {
            return it->second;
        }
        return json{
            {"status", "not_found"},
            {"sync_id", sync_id},
            {"completed_sync_id", state_.completed_generation},
            {"already_in_progress", false}
        };
    }
    return status_locked();
}

json SyncService::wait_for(uint64_t generation) const {
    std::unique_lock<std::mutex> lock(state_.mutex);
    state_.cv.wait(lock, [this, generation] {
        return state_.worker_exited ||
               (state_.completed_generation >= generation &&
                (!state_.is_sync_running || state_.current_generation > generation));
    });
    if (state_.worker_exited && state_.completed_generation < generation) {
        LOG(WARNING, "ModelManager")
            << "Waited for sync generation " << generation << " after the worker exited." << std::endl;
    }
    auto it = state_.completed_generation_results.find(generation);
    if (it != state_.completed_generation_results.end()) {
        return it->second;
    }
    return status_locked();
}

// Shutdown uses this so a download cannot outlive the store it writes into.
void SyncService::wait_until_idle() {
    std::unique_lock<std::mutex> lock(state_.mutex);
    state_.cv.wait(lock, [this] { return !state_.is_sync_running || state_.worker_exited; });
    if (state_.is_sync_running) {
        LOG(WARNING, "ModelManager")
            << "Sync reported idle because the worker exited with a generation still running."
            << std::endl;
    }
}

bool SyncService::cancel_requested() const {
    std::lock_guard<std::mutex> lock(state_.mutex);
    return state_.cancel_requested;
}

bool SyncService::should_auto_update(const ModelInfo& info) const {
    if (info.auto_update.has_value()) {
        return *info.auto_update;
    }
    auto* cfg = RuntimeConfig::global();
    return cfg ? cfg->auto_update_models() : false;
}

json SyncService::status_locked() const {
    std::string status = "idle";
    if (state_.is_sync_running) {
        status = "in_progress";
    } else if (!state_.terminal_error.empty() || !state_.failed_models.empty()) {
        status = "failed";
    } else if (state_.checked_count > 0) {
        status = "success";
    }

    json j = json{
        {"status", status},
        {"sync_id", state_.current_generation},
        {"completed_sync_id", state_.completed_generation},
        {"already_in_progress", state_.is_sync_running},
        {"is_full_sync", state_.is_full_sync},
        {"active_targets", state_.active_targets},
        {"pending_targets", state_.pending_targets},
        {"completed_targets", state_.completed_targets},
        {"models_updated", state_.completed_targets},
        {"models_up_to_date", state_.models_up_to_date},
        {"failed_models", state_.failed_models},
        {"terminal_error", state_.terminal_error},
        {"checked_count", state_.checked_count},
        {"updated_count", state_.completed_targets.size()}
    };
    if (state_.is_sync_running) {
        j["progress"] = json{
            {"current_model", state_.current_model},
            {"file", state_.current_file},
            {"file_index", state_.file_index},
            {"total_files", state_.total_files},
            {"bytes_downloaded", state_.bytes_downloaded},
            {"bytes_total", state_.bytes_total},
            {"percent", state_.percent}
        };
    }
    return j;
}

void SyncService::cancel() {

    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.cancel_requested = true;
}

SyncService::Enqueued SyncService::enqueue(const std::vector<std::string>& target_models,
                                           bool attach_if_running) {
    std::lock_guard<std::mutex> lock(state_.mutex);

    auto is_already_known = [this](const std::string& target) -> bool {
        std::string canonical = deps_.store->resolve_name(target);
        std::string pub = deps_.store->public_name_for(canonical);
        if (state_.active_targets.count(target) || state_.active_targets.count(pub) || state_.active_targets.count(canonical)) {
            return true;
        }
        if (std::find(state_.completed_targets.begin(), state_.completed_targets.end(), target) != state_.completed_targets.end() ||
            std::find(state_.completed_targets.begin(), state_.completed_targets.end(), pub) != state_.completed_targets.end() ||
            std::find(state_.completed_targets.begin(), state_.completed_targets.end(), canonical) != state_.completed_targets.end()) {
            return true;
        }
        if (std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), target) != state_.models_up_to_date.end() ||
            std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), pub) != state_.models_up_to_date.end() ||
            std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), canonical) != state_.models_up_to_date.end()) {
            return true;
        }
        return false;
    };

    if (state_.is_sync_running) {
        LOG(INFO, "ModelManager") << "Sync requested while a model synchronization is already in progress. Enqueueing targets." << std::endl;
        if (!attach_if_running) {
            if (target_models.empty()) {
                state_.is_full_sync = true;
            } else {
                for (const auto& t : target_models) {
                    std::string canonical = deps_.store->resolve_name(t);
                    std::string pub = deps_.store->public_name_for(canonical);
                    if (!is_already_known(t)) {
                        state_.pending_targets.insert(pub);
                    }
                }
            }
        }
        return Enqueued{/*already_running=*/true, /*sync_id=*/state_.current_generation};
    }

    state_.current_generation++;
    state_.is_sync_running = true;
    state_.is_full_sync = target_models.empty();
    state_.cancel_requested = false;
    state_.active_targets.clear();
    state_.pending_targets.clear();
    state_.completed_targets.clear();
    state_.models_up_to_date.clear();
    state_.failed_models.clear();
    state_.terminal_error.clear();
    state_.checked_count = 0;
    for (const auto& t : target_models) {
        std::string canonical = deps_.store->resolve_name(t);
        std::string pub = deps_.store->public_name_for(canonical);
        state_.pending_targets.insert(pub);
    }
    // The worker is the only executor, so queueing is also what starts work.
    state_.cv.notify_all();
    return Enqueued{/*already_running=*/false, /*sync_id=*/state_.current_generation};
}

void SyncService::run_generation() {
    struct SyncStateGuard {
        State& state;
        bool active = true;
        ~SyncStateGuard() {
            if (active) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.is_sync_running = false;
                state.active_targets.clear();
                state.current_model.clear();
                state.current_file.clear();
                state.file_index = 0;
                state.total_files = 0;
                state.bytes_downloaded = 0;
                state.bytes_total = 0;
                state.percent = 0;
                state.completed_generation = state.current_generation;
                state.completed_generation_results[state.current_generation] = json{
                    {"status", "failed"},
                    {"sync_id", state.current_generation},
                    {"completed_sync_id", state.current_generation},
                    {"already_in_progress", false},
                    {"terminal_error", "Sync worker terminated unexpectedly"}
                };
                state.cv.notify_all();
            }
        }
    } guard{state_};

    enum class ModelCheckOutcome {
        UpToDate,
        UpdateAvailable,
        Failed
    };
    std::unordered_map<std::string, ModelCheckOutcome> checked_canonicals_in_job;

    try {
        while (true) {
            std::vector<std::string> current_batch;
            bool run_full_sync = false;

            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                if (state_.is_full_sync) {
                    run_full_sync = true;
                    state_.is_full_sync = false;
                }
            }

            if (run_full_sync) {
                LOG(INFO, "ModelManager") << "Checking all models for sync/update..." << std::endl;
                UpdateScanResult full_check = scan_for_updates();

                std::vector<std::string> canonicals_to_notify;
                {
                    std::lock_guard<std::mutex> lock(state_.mutex);
                    for (const auto& [m, err] : full_check.failed_models) {
                        std::string canonical = deps_.store->resolve_name(m);
                        if (std::find(state_.completed_targets.begin(), state_.completed_targets.end(), m) == state_.completed_targets.end()) {
                            checked_canonicals_in_job[canonical] = ModelCheckOutcome::Failed;
                            canonicals_to_notify.push_back(canonical);
                            state_.failed_models[m] = err;
                            auto up_it = std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), m);
                            if (up_it != state_.models_up_to_date.end()) {
                                state_.models_up_to_date.erase(up_it);
                            }
                        }
                    }
                    for (const auto& m : full_check.updated_models) {
                        std::string canonical = deps_.store->resolve_name(m);
                        if (std::find(state_.completed_targets.begin(), state_.completed_targets.end(), m) == state_.completed_targets.end()) {
                            checked_canonicals_in_job[canonical] = ModelCheckOutcome::UpdateAvailable;
                            canonicals_to_notify.push_back(canonical);
                            auto up_it = std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), m);
                            if (up_it != state_.models_up_to_date.end()) {
                                state_.models_up_to_date.erase(up_it);
                            }
                            state_.failed_models.erase(m);
                            if (state_.active_targets.find(m) == state_.active_targets.end()) {
                                state_.pending_targets.insert(m);
                            }
                        }
                    }
                    for (const auto& m : full_check.up_to_date_models) {
                        std::string canonical = deps_.store->resolve_name(m);
                        if (std::find(state_.completed_targets.begin(), state_.completed_targets.end(), m) == state_.completed_targets.end()) {
                            checked_canonicals_in_job[canonical] = ModelCheckOutcome::UpToDate;
                            canonicals_to_notify.push_back(canonical);
                            state_.failed_models.erase(m);
                            if (std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), m) == state_.models_up_to_date.end()) {
                                state_.models_up_to_date.push_back(m);
                            }
                        }
                    }
                    state_.checked_count = static_cast<int>(checked_canonicals_in_job.size());
                }

                if (deps_.phase) {
                    for (const auto& canonical : canonicals_to_notify) {
                        deps_.phase("check_model:" + canonical);
                    }
                    deps_.phase("full_check_done");
                }
            }

            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                if (!state_.pending_targets.empty()) {
                    pending.assign(state_.pending_targets.begin(), state_.pending_targets.end());
                    state_.pending_targets.clear();
                }
            }

            for (const auto& t : pending) {
                try {
                    std::string canonical_name = deps_.store->resolve_name(t);
                    std::string public_name = deps_.store->public_name_for(canonical_name);

                    bool skip = false;
                    {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        if (state_.active_targets.count(public_name) ||
                            std::find(state_.completed_targets.begin(), state_.completed_targets.end(), public_name) != state_.completed_targets.end() ||
                            std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), public_name) != state_.models_up_to_date.end()) {
                            skip = true;
                        }
                    }
                    if (skip) {
                        continue;
                    }

                    if (deps_.phase) {
                        deps_.phase("check_model:" + canonical_name);
                    }

                    ModelInfo info = *deps_.store->info(canonical_name);
                    if (!info.downloaded) {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        state_.failed_models[public_name] = "Model is not downloaded";
                        checked_canonicals_in_job[canonical_name] = ModelCheckOutcome::Failed;
                        continue;
                    }

                    auto it = checked_canonicals_in_job.find(canonical_name);
                    if (it != checked_canonicals_in_job.end()) {
                        if (it->second == ModelCheckOutcome::UpdateAvailable) {
                            current_batch.push_back(public_name);
                        }
                        continue;
                    }

                    UpdateScanResult target_check = scan_for_updates({public_name});

                    {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        if (!target_check.failed_models.empty() && target_check.failed_models.count(public_name)) {
                            state_.failed_models[public_name] = target_check.failed_models[public_name];
                            checked_canonicals_in_job[canonical_name] = ModelCheckOutcome::Failed;
                        } else if (!target_check.updated_models.empty()) {
                            current_batch.push_back(public_name);
                            checked_canonicals_in_job[canonical_name] = ModelCheckOutcome::UpdateAvailable;
                            state_.checked_count++;
                        } else if (!target_check.up_to_date_models.empty() &&
                                   std::find(target_check.up_to_date_models.begin(), target_check.up_to_date_models.end(), public_name) != target_check.up_to_date_models.end()) {
                            if (std::find(state_.models_up_to_date.begin(), state_.models_up_to_date.end(), public_name) == state_.models_up_to_date.end()) {
                                state_.models_up_to_date.push_back(public_name);
                            }
                            checked_canonicals_in_job[canonical_name] = ModelCheckOutcome::UpToDate;
                            state_.checked_count++;
                        } else {
                            state_.failed_models[public_name] = "Model is not eligible for remote registry update checks";
                            checked_canonicals_in_job[canonical_name] = ModelCheckOutcome::Failed;
                        }
                    }
                    if (deps_.phase) {
                        deps_.phase("target_check_done:" + canonical_name);
                    }
                } catch (const std::exception& e) {
                    {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        state_.failed_models[t] = e.what();
                        checked_canonicals_in_job[deps_.store->resolve_name(t)] = ModelCheckOutcome::Failed;
                    }
                    if (deps_.phase) {
                        deps_.phase("target_check_done:" + deps_.store->resolve_name(t));
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                if (current_batch.empty()) {
                    if (state_.is_full_sync || !state_.pending_targets.empty()) {
                        continue;
                    }
                    state_.is_sync_running = false;
                    state_.active_targets.clear();
                    state_.current_model.clear();
                    state_.current_file.clear();
                    state_.file_index = 0;
                    state_.total_files = 0;
                    state_.bytes_downloaded = 0;
                    state_.bytes_total = 0;
                    state_.percent = 0;
                    state_.completed_generation = state_.current_generation;
                    json res = status_locked();
                    state_.completed_generation_results[state_.current_generation] = res;
                    if (state_.completed_generation_results.size() > 32) {
                        state_.completed_generation_results.erase(state_.completed_generation_results.begin());
                    }
                    guard.active = false;
                    state_.cv.notify_all();
                    return;
                }
            }

            for (const auto& model_name : current_batch) {
                {
                    std::lock_guard<std::mutex> lock(state_.mutex);
                    state_.active_targets.insert(model_name);
                }

                LOG(INFO, "ModelManager") << "Syncing model: " << model_name << std::endl;
                try {
                    auto progress_cb = [this, model_name](const DownloadProgress& prog) -> bool {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        if (state_.cancel_requested) {
                            return false;
                        }
                        state_.current_model = model_name;
                        state_.current_file = prog.file;
                        state_.file_index = prog.file_index;
                        state_.total_files = prog.total_files;
                        state_.bytes_downloaded = prog.bytes_downloaded;
                        state_.bytes_total = prog.bytes_total;
                        state_.percent = prog.percent;
                        return true;
                    };

                    deps_.download(model_name, json::object(), false, progress_cb);

                    std::lock_guard<std::mutex> lock(state_.mutex);
                    state_.active_targets.erase(model_name);
                    state_.completed_targets.push_back(model_name);
                } catch (const std::exception& e) {
                    LOG(ERROR, "ModelManager") << "Failed to sync model " << model_name << ": " << e.what() << std::endl;
                    std::lock_guard<std::mutex> lock(state_.mutex);
                    state_.active_targets.erase(model_name);
                    state_.failed_models[model_name] = e.what();
                }
            }
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Terminal error in model synchronization: " << e.what() << std::endl;
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.terminal_error = e.what();
        state_.is_sync_running = false;
        state_.active_targets.clear();
        state_.current_model.clear();
        state_.current_file.clear();
        state_.file_index = 0;
        state_.total_files = 0;
        state_.bytes_downloaded = 0;
        state_.bytes_total = 0;
        state_.percent = 0;
        state_.completed_generation = state_.current_generation;
        json res = status_locked();
        state_.completed_generation_results[state_.current_generation] = res;
        if (state_.completed_generation_results.size() > 32) {
            state_.completed_generation_results.erase(state_.completed_generation_results.begin());
        }
        guard.active = false;
        state_.cv.notify_all();
        return;
    }

    return;
}

}  // namespace models
}  // namespace lemon
