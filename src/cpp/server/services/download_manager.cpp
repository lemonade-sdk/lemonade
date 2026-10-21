#include "lemon/server/download_manager.h"

#include <algorithm>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

using json = nlohmann::json;

namespace {

// Completed jobs stay listed this long so every open client sees them finish.
constexpr auto kTerminalVisibility = std::chrono::seconds(30);

} // namespace

const RouteError DownloadManager::kDownloadNotFound{
    404, "", "`id` names no download job and `action` is not `remove`"};

DownloadManager::~DownloadManager() {
    cancel_all();
}

json DownloadManager::progress_to_json(const DownloadProgress& p) {
    json event_data;
    event_data["file"] = p.file;
    event_data["file_index"] = p.file_index;
    event_data["total_files"] = p.total_files;
    event_data["bytes_downloaded"] = static_cast<uint64_t>(p.bytes_downloaded);
    event_data["bytes_total"] = static_cast<uint64_t>(p.bytes_total);
    event_data["total_download_size"] = static_cast<uint64_t>(p.total_download_size);
    event_data["bytes_previously_downloaded"] = static_cast<uint64_t>(p.bytes_previously_downloaded);
    event_data["percent"] = p.percent;
    event_data["complete"] = p.complete;
    if (!p.error.empty()) {
        event_data["error"] = p.error;
    }
    return event_data;
}

json DownloadManager::job_to_json(const std::shared_ptr<Job>& job) {
    json item = job->progress.is_object() ? job->progress : json::object();
    item["id"] = job->id;
    item["type"] = job->type;
    item["model_name"] = job->display_name;
    item["status"] = job->status;
    item["running"] = job->running;
    if (!job->error.empty()) {
        item["error"] = job->error;
    }
    return item;
}

bool DownloadManager::is_visible(const std::shared_ptr<Job>& job) {
    if (!job) return false;
    if (job->running) {
        return true;
    }
    if (job->status == "downloading" || job->status == "paused" || job->status == "error") {
        return true;
    }
    if (job->status == "cancelled") {
        // Cancelled downloads may still have partial files on disk. Keep them
        // discoverable across reloads/restarts until the UI explicitly removes
        // the row after cleanup, retry, or user dismissal.
        return true;
    }
    if (job->status == "completed") {
        return job->terminal_since.time_since_epoch().count() > 0 &&
               std::chrono::steady_clock::now() - job->terminal_since < kTerminalVisibility;
    }
    return false;
}

void DownloadManager::join(const std::shared_ptr<Job>& job) {
    // Move the thread out under the job-local mutex, then join without holding either
    // worker_mutex or mutex_. This avoids both data races on std::thread and deadlocks
    // with progress callbacks.
    if (!job) return;

    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(job->worker_mutex);
        if (!job->worker.joinable()) return;
        if (job->worker.get_id() == std::this_thread::get_id()) return;
        worker = std::move(job->worker);
    }

    worker.join();
}

json DownloadManager::start(const std::string& download_id, const std::string& download_type,
                            const std::string& display_name, Operation operation) {
    std::shared_ptr<Job> old_job;
    auto job = std::make_shared<Job>();
    job->id = download_id;
    job->type = download_type;
    job->display_name = display_name;
    job->status = "downloading";
    job->running = true;
    job->progress = {
        {"id", download_id},
        {"type", download_type},
        {"model_name", display_name},
        {"file", ""},
        {"file_index", 0},
        {"total_files", 0},
        {"bytes_downloaded", 0},
        {"bytes_total", 0},
        {"total_download_size", 0},
        {"bytes_previously_downloaded", 0},
        {"completed_files_bytes", 0},
        {"cumulative_bytes_downloaded", 0},
        {"overall_bytes_downloaded", 0},
        {"percent", 0},
        {"complete", false}
    };

    std::unique_lock<std::mutex> worker_lock(job->worker_mutex);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = jobs_.find(download_id);
        if (existing != jobs_.end()) {
            if (existing->second->running || existing->second->status == "downloading") {
                return job_to_json(existing->second);
            }
            old_job = existing->second;
        }
        jobs_[download_id] = job;
    }

    join(old_job);

    job->worker = std::thread([this, job, operation = std::move(operation)]() mutable {
        try {
            DownloadProgressCallback progress_cb = [this, job](const DownloadProgress& p) -> bool {
                std::lock_guard<std::mutex> lock(mutex_);
                // A final complete event wins over a late pause/cancel request.
                if (job->cancel_requested && !p.complete) {
                    job->stop_acknowledged = true;
                    return false;
                }

                if (job->current_file_index != p.file_index) {
                    if (job->current_file_index >= 0 && p.file_index > job->current_file_index) {
                        job->completed_files_bytes += job->current_file_bytes_total;
                    }
                    job->current_file_index = p.file_index;
                    job->current_file_bytes_total = 0;
                }
                if (p.bytes_total > 0) {
                    job->current_file_bytes_total = std::max<uint64_t>(
                        job->current_file_bytes_total,
                        static_cast<uint64_t>(p.bytes_total));
                }

                const uint64_t total_download_size = static_cast<uint64_t>(p.total_download_size);
                uint64_t cumulative_bytes = job->completed_files_bytes + static_cast<uint64_t>(p.bytes_downloaded);
                if (p.complete && total_download_size > 0) {
                    cumulative_bytes = total_download_size;
                } else if (total_download_size > 0) {
                    cumulative_bytes = std::min(cumulative_bytes, total_download_size);
                }

                job->progress = progress_to_json(p);
                job->progress["completed_files_bytes"] = job->completed_files_bytes;
                job->progress["cumulative_bytes_downloaded"] = cumulative_bytes;
                job->progress["overall_bytes_downloaded"] = cumulative_bytes;
                job->status = p.complete ? "completed" : "downloading";
                // terminal_since is the time the worker has actually stopped, not
                // the time a terminal status first becomes visible. Keeping it
                // empty while running=true prevents list() from expiring the
                // row before other tabs can observe that files are released.
                job->terminal_since = std::chrono::steady_clock::time_point{};
                job->error.clear();
                return true;
            };

            operation(progress_cb);

            std::lock_guard<std::mutex> lock(mutex_);
            if (job->cancel_requested && job->stop_acknowledged) {
                job->status = job->cancel_action == "cancel" ? "cancelled" : "paused";
                job->progress["complete"] = false;
            } else {
                job->status = "completed";
                job->progress["complete"] = true;
                job->progress["percent"] = 100;
                const uint64_t total_download_size = job->progress.value("total_download_size", uint64_t{0});
                if (total_download_size > 0) {
                    job->progress["cumulative_bytes_downloaded"] = total_download_size;
                    job->progress["overall_bytes_downloaded"] = total_download_size;
                }
            }
            job->running = false;
            job->terminal_since = (job->status == "completed" || job->status == "cancelled")
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
        } catch (const lemon::UnknownModelError& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            LOG(ERROR, "DownloadManager") << "worker unknown-model error id=" << job->id
                                           << " error=\"" << e.what() << "\"" << std::endl;
            job->status = "error";
            job->terminal_since = std::chrono::steady_clock::time_point{};
            job->error = e.what();
            job->progress["error"] = e.what();
            job->progress["code"] = lemon::kUnknownModelErrorCode;
            job->running = false;
        } catch (const std::exception& e) {
            bool cancel_requested = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancel_requested = job->cancel_requested;
            }

            if (!cancel_requested) {
                LOG(ERROR, "DownloadManager") << "worker exception id=" << job->id
                                               << " error=\"" << e.what() << "\"" << std::endl;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (job->cancel_requested) {
                job->status = job->cancel_action == "cancel" ? "cancelled" : "paused";
                job->error.clear();
            } else {
                job->status = "error";
                job->terminal_since = std::chrono::steady_clock::time_point{};
                job->error = e.what();
                job->progress["error"] = e.what();
            }
            job->running = false;
            if (job->status == "cancelled") {
                job->terminal_since = std::chrono::steady_clock::now();
            }
        }
    });
    worker_lock.unlock();

    // The worker can update the job as soon as it starts, so the snapshot is taken
    // under the same mutex its progress callback holds.
    std::lock_guard<std::mutex> lock(mutex_);
    return job_to_json(job);
}

json DownloadManager::list() {
    json response = json::array();
    std::vector<std::shared_ptr<Job>> expired_jobs;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            const auto& job = it->second;
            if (is_visible(job)) {
                response.push_back(job_to_json(job));
                ++it;
                continue;
            }

            const bool expired_terminal = job &&
                !job->running &&
                job->status == "completed" &&
                job->terminal_since.time_since_epoch().count() > 0 &&
                now - job->terminal_since >= kTerminalVisibility;

            if (expired_terminal) {
                expired_jobs.push_back(job);
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& job : expired_jobs) {
        join(job);
    }

    return response;
}

json DownloadManager::control(const std::string& id, const std::string& action, int& status) {
    json response_json;
    std::shared_ptr<Job> job_to_join;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            if (action == "remove") {
                return {{"status", "ok"}, {"missing", true}};
            }
            status = kDownloadNotFound.status;
            return {{"error", "Download not found"}};
        }

        auto job = it->second;
        if (action == "pause" || action == "cancel") {
            const bool terminal = job->status == "completed" ||
                job->status == "cancelled" ||
                job->status == "error";

            if (!terminal) {
                job->cancel_requested = true;
                job->cancel_action = action;
                job->status = action == "cancel" ? "cancelled" : "paused";
                // Paused jobs remain visible until resumed/removed. A cancel
                // request for an already-stopped job has no worker that will
                // later stamp terminal_since, so start the terminal visibility
                // window here to avoid leaving a stale hidden registry entry.
                job->terminal_since = (action == "cancel" && !job->running)
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
            }

            response_json = job_to_json(job);
        } else if (action == "remove") {
            if (job->running) {
                // A remove request must not make the job disappear while the
                // worker may still hold file handles. Convert it to a cancel
                // request and keep the job visible until running=false.
                job->cancel_requested = true;
                job->cancel_action = "cancel";
                if (job->status != "completed" && job->status != "error") {
                    job->status = "cancelled";
                    job->terminal_since = std::chrono::steady_clock::time_point{};
                }
                response_json = job_to_json(job);
            } else {
                job_to_join = job;
                jobs_.erase(it);
                response_json = {{"status", "ok"}};
            }
        } else {
            status = ApiRoute::kArgInvalid.status;
            return {{"error", "Unsupported download action"}};
        }
    }

    join(job_to_join);
    return response_json;
}

void DownloadManager::cancel_all() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, job] : jobs_) {
            job->cancel_requested = true;
            job->cancel_action = "cancel";
            jobs.push_back(job);
        }
    }

    for (auto& job : jobs) {
        join(job);
    }
}

void DownloadManager::stream(const Operation& operation, httplib::DataSink& sink) {
    try {
        bool complete_sent = false;
        DownloadProgressCallback progress_cb = [&sink, &complete_sent](const DownloadProgress& p) -> bool {
            // The event names carry completion and errors, so the data leaves them out.
            json event_data = progress_to_json(p);
            event_data.erase("complete");
            event_data.erase("error");
            complete_sent = complete_sent || p.complete;
            if (!write_sse_event(sink, p.complete ? "complete" : "progress", event_data)) {
                LOG(INFO, "Server") << "Client disconnected, cancelling download" << std::endl;
                return false;
            }
            return true;
        };

        operation(progress_cb);

        // An operation that had nothing to download (e.g. a backend already
        // installed) never reports completion, so report it here.
        if (!complete_sent) {
            write_sse_event(sink, "complete", {{"status", "ok"}});
        }

    } catch (const lemon::UnknownModelError& e) {
        write_sse_event(sink, "error", {{"error", e.what()}, {"code", lemon::kUnknownModelErrorCode}});
    } catch (const lemon::RegistryRateLimitError& e) {
        write_sse_event(sink, "error", {{"error", e.what()},
                                        {"code", lemon::kRegistryUnavailableErrorCode},
                                        {"retry_after", e.retry_after_seconds()}});
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        if (error_msg != "Download cancelled") {
            write_sse_event(sink, "error", {{"error", error_msg}});
        }
    }

    sink.done();
}

nlohmann::ordered_json DownloadManager::job_schema() {
    return nlohmann::ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "type", "model_name", "status", "running"],
        "properties": {
            "id": {"type": "string", "description": "Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`."},
            "type": {"enum": ["model", "backend"], "description": "Download type."},
            "model_name": {"type": "string", "description": "Lemonade model name associated with the job, or `recipe:backend` for a backend job."},
            "status": {"enum": ["downloading", "paused", "cancelled", "completed", "error"], "description": "Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`."},
            "running": {"type": "boolean", "description": "Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources."},
            "file": {"type": "string", "description": "The file downloading now."},
            "file_index": {"type": "integer", "description": "Its position among the download's files, from `1`."},
            "total_files": {"type": "integer", "description": "How many files the download has."},
            "bytes_downloaded": {"type": "integer", "description": "Bytes of the current file downloaded so far."},
            "bytes_total": {"type": "integer", "description": "Size of the current file in bytes."},
            "percent": {"type": "number", "description": "Share of the current file downloaded, from `0` to `100`."},
            "total_download_size": {"type": "integer", "description": "Total expected bytes across all files when known."},
            "bytes_previously_downloaded": {"type": "integer", "description": "Bytes already present on disk for the current file when resuming or skipping existing data."},
            "completed_files_bytes": {"type": "integer", "description": "Bytes from files completed before the current file."},
            "cumulative_bytes_downloaded": {"type": "integer", "description": "Bytes downloaded across the whole job."},
            "overall_bytes_downloaded": {"type": "integer", "description": "Older name for `cumulative_bytes_downloaded`."},
            "complete": {"type": "boolean", "description": "`true` when the download completed successfully."},
            "error": {"type": "string", "description": "Error message, present only for failed jobs."},
            "code": {"type": "string", "description": "`unknown_model` when the model is not in the registry."}
        }
    })");
}

nlohmann::ordered_json DownloadManager::event_schema() {
    return nlohmann::ordered_json::parse(R"({
        "type": "object",
        "description": "A `progress` event reports one file; a `complete` event follows the last one, and an `error` event replaces it on failure.",
        "properties": {
            "file": {"type": "string", "description": "The file downloading now."},
            "file_index": {"type": "integer", "description": "Its position among the download's files, from `1`."},
            "total_files": {"type": "integer", "description": "How many files the download has."},
            "bytes_downloaded": {"type": "integer", "description": "Bytes of the current file downloaded so far."},
            "bytes_total": {"type": "integer", "description": "Size of the current file in bytes."},
            "total_download_size": {"type": "integer", "description": "Total expected bytes across all files, or `0` when unknown."},
            "bytes_previously_downloaded": {"type": "integer", "description": "Bytes of the current file already on disk before this download, when resuming or skipping existing data."},
            "percent": {"type": "number", "description": "Share of the current file downloaded, from `0` to `100`."},
            "status": {"const": "ok", "description": "Only on a complete event for an operation that had nothing to download."},
            "error": {"type": "string", "description": "Only on an error event."},
            "code": {"type": "string", "description": "`unknown_model` when the model is not in the registry."}
        }
    })");
}

} // namespace lemon
