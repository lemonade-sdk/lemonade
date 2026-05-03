#pragma once

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <nlohmann/json.hpp>
#include "model_manager.h"

namespace httplib { struct Response; }

namespace lemon {

enum class JobStatus {
    Queued,
    Running,
    Complete,
    Failed,
    Cancelled
};

enum class CancelResult { Ok, NotFound, AlreadyTerminal };

struct DownloadJob {
    // Immutable after enqueue
    std::string id;
    std::string model_name;
    nlohmann::json model_data;
    bool do_not_upgrade = false;
    int64_t created_at = 0;

    // Mutable state (guarded by DownloadQueue::mutex_)
    JobStatus status = JobStatus::Queued;
    int64_t started_at = 0;
    int64_t finished_at = 0;
    std::string error;
    DownloadProgress last_progress;

    // Ring buffer of SSE event strings (guarded by DownloadQueue::mutex_)
    static constexpr size_t kEventBufCap = 100;
    std::deque<std::string> event_buffer;
    uint64_t total_events = 0;  // monotonically increasing counter

    // Atomic cancel flag — set without holding the mutex
    std::atomic<bool> cancel_flag{false};

    // Worker thread (one per job; joinable until shutdown)
    std::thread worker_thread;
};

class DownloadQueue {
public:
    explicit DownloadQueue(ModelManager* model_manager, const std::string& cache_dir);
    ~DownloadQueue();

    // Enqueue a download. Returns job ID.
    // If the model is already queued/running, returns the existing job ID.
    std::string enqueue(const std::string& model_name,
                        const nlohmann::json& model_data,
                        bool do_not_upgrade);

    CancelResult cancel(const std::string& id);

    // Cancel all active jobs and join all worker threads.
    void shutdown();

    // Stream SSE events for a job. Replays buffered events then streams live.
    // Sets res.status=404 and returns if job not found.
    void stream_job(const std::string& id, httplib::Response& res);

    // Thread-safe JSON serialisation of job list / single job.
    nlohmann::json list_jobs_json() const;
    nlohmann::json get_job_json(const std::string& id) const;

    static std::string status_str(JobStatus s);
    static bool is_terminal(JobStatus s);

private:
    void run_job(std::shared_ptr<DownloadJob> job);
    void persist();
    void load_persisted();
    std::string generate_id();

    static JobStatus status_from_str(const std::string& s);
    static nlohmann::json progress_to_json(const DownloadProgress& p);
    static nlohmann::json job_to_json_locked(const DownloadJob& job);

    ModelManager* model_manager_;
    std::string persist_path_;

    mutable std::mutex mutex_;
    std::condition_variable event_cv_;
    std::vector<std::shared_ptr<DownloadJob>> jobs_;

    std::atomic<uint32_t> next_id_;
    bool shutdown_ = false;
};

} // namespace lemon
