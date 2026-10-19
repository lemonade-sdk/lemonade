#pragma once

#include <lemon/models/download_progress.h>
#include <lemon/models/update_scan.h>

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lemon {

struct ModelInfo;

namespace models {

// The one queue and the one worker in model management. Scanning is the only
// model-management work that starts on one request, runs for minutes, reports
// progress and can be cancelled, so nothing else may spawn work of its own:
// two passes over the same repositories answer nothing twice.
class SyncService {
  public:
    struct Deps {
        std::function<UpdateScanResult(const std::vector<std::string>&)> scan;
        std::function<void(const std::string& name, const nlohmann::json& options,
                           bool do_not_upgrade,
                           DownloadProgressCallback progress)> download;
        std::function<ModelInfo(const std::string& canonical)> model_info;
        std::function<std::string(const std::string& name)> resolve_name;
        std::function<std::string(const std::string& canonical)> public_name;
        std::function<void(const std::string& phase)> phase;
    };

    struct Enqueued {
        bool already_running = false;
        uint64_t sync_id = 0;
    };

    // Starts the worker, which then waits: work runs only when queued, never
    // because time passed.
    explicit SyncService(Deps deps);
    ~SyncService();

    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    // A request arriving while a sync runs joins it and gets that sync's id.
    Enqueued enqueue(const std::vector<std::string>& targets = {},
                     bool attach_if_running = false);
    UpdateScanResult scan_for_updates(const std::vector<std::string>& targets = {});

    // Report what a sync would change, or run one and wait for it.
    nlohmann::json sync(const std::vector<std::string>& target_models = {}, bool dry_run = false,
                        bool attach_if_running = false);
    nlohmann::json status(uint64_t sync_id = 0) const;
    nlohmann::json wait_for(uint64_t generation) const;
    void cancel();
    void wait_until_idle();
    bool cancel_requested() const;
    bool should_auto_update(const ModelInfo& info) const;

  private:
    struct State {
        mutable std::mutex mutex;
        mutable std::condition_variable cv;
        bool is_sync_running = false;
        bool stop = false;
        // Set when the worker has left run_worker(). A wait treats it as "no
        // further state change is coming" rather than waiting for a transition
        // that can no longer happen.
        bool worker_exited = false;
        bool is_full_sync = false;
        bool cancel_requested = false;
        uint64_t current_generation = 0;
        uint64_t completed_generation = 0;
        std::map<uint64_t, nlohmann::json> completed_generation_results;
        std::set<std::string> active_targets;
        std::set<std::string> pending_targets;
        std::vector<std::string> completed_targets;
        std::vector<std::string> models_up_to_date;
        std::map<std::string, std::string> failed_models;
        int checked_count = 0;
        std::string terminal_error;

        std::string current_model;
        std::string current_file;
        int file_index = 0;
        int total_files = 0;
        size_t bytes_downloaded = 0;
        size_t bytes_total = 0;
        int percent = 0;
    };

    void run_worker();
    void run_generation();
    nlohmann::json status_locked() const;

    Deps deps_;
    State state_;
    // Startup, an explicit check request and the worker's own pass may all want
    // to scan; one at a time.
    std::mutex scan_mutex_;
    // Declared last on purpose. Constructing it starts the thread, and members
    // initialize in declaration order, so anything run_worker() reads has to be
    // above it: a member declared after this one is still uninitialized the
    // first time the worker looks at it.
    std::thread worker_;
};

}  // namespace models
}  // namespace lemon
