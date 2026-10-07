#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"

namespace lemon {

// A desktop client that reloads or opens a new tab must find a model or backend
// download still running, so the server owns those jobs and their threads. Pull and
// install also send their progress as an event stream built here.
class DownloadManager {
public:
    using Operation = std::function<void(DownloadProgressCallback)>;

    ~DownloadManager();

    // Starts operation on a worker thread under id, or returns the job already running
    // under that id. Returns the job's snapshot.
    nlohmann::json start(const std::string& id, const std::string& type,
                         const std::string& display_name, Operation operation);

    // Snapshots of every visible job, expiring completed jobs past their grace period.
    nlohmann::json list();

    // Applies a pause, cancel or remove action. Sets status to 404 for an unknown job
    // and 400 for an unknown action; the returned body is the response either way.
    nlohmann::json control(const std::string& id, const std::string& action, int& status);

    // Asks every worker to stop and joins it. Workers capture this object, so they must
    // never outlive it.
    void cancel_all();

    // Runs operation on the calling thread, writing its progress to sink as
    // progress/complete/error server-sent events.
    static void stream(const Operation& operation, httplib::DataSink& sink);

    static nlohmann::json job_schema();
    static nlohmann::json event_schema();

private:
    struct Job {
        std::string id;
        std::string type;
        std::string display_name;
        std::string status;
        std::string cancel_action;
        std::string error;
        nlohmann::json progress;
        uint64_t completed_files_bytes = 0;
        uint64_t current_file_bytes_total = 0;
        int current_file_index = -1;
        bool cancel_requested = false;
        // Set by the worker's progress callback once the downloader has actually
        // observed a pause/cancel request and stopped before completion. This
        // prevents a late UI request from overriding a successful operation that
        // already returned normally.
        bool stop_acknowledged = false;
        bool running = false;
        std::chrono::steady_clock::time_point terminal_since;
        // Protects worker publication/join. A job can be visible in the registry
        // while start() is still joining the previous worker; removals and
        // shutdown must wait until the new worker thread is either assigned or
        // known to be absent before deciding whether to join.
        mutable std::mutex worker_mutex;
        std::thread worker;
    };

    static nlohmann::json progress_to_json(const DownloadProgress& progress);
    static nlohmann::json job_to_json(const std::shared_ptr<Job>& job);
    static bool is_visible(const std::shared_ptr<Job>& job);
    static void join(const std::shared_ptr<Job>& job);

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
};

} // namespace lemon
