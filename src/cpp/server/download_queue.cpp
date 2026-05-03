#include <lemon/download_queue.h>
#include <httplib.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

// ---- static helpers --------------------------------------------------------

std::string DownloadQueue::status_str(JobStatus s) {
    switch (s) {
        case JobStatus::Queued:    return "queued";
        case JobStatus::Running:   return "running";
        case JobStatus::Complete:  return "complete";
        case JobStatus::Failed:    return "failed";
        case JobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

JobStatus DownloadQueue::status_from_str(const std::string& s) {
    if (s == "queued")    return JobStatus::Queued;
    if (s == "running")   return JobStatus::Running;
    if (s == "complete")  return JobStatus::Complete;
    if (s == "failed")    return JobStatus::Failed;
    if (s == "cancelled") return JobStatus::Cancelled;
    return JobStatus::Failed;
}

bool DownloadQueue::is_terminal(JobStatus s) {
    return s == JobStatus::Complete || s == JobStatus::Failed || s == JobStatus::Cancelled;
}

nlohmann::json DownloadQueue::progress_to_json(const DownloadProgress& p) {
    return {
        {"file", p.file},
        {"file_index", p.file_index},
        {"total_files", p.total_files},
        {"bytes_downloaded", static_cast<uint64_t>(p.bytes_downloaded)},
        {"bytes_total", static_cast<uint64_t>(p.bytes_total)},
        {"total_download_size", static_cast<uint64_t>(p.total_download_size)},
        {"bytes_previously_downloaded", static_cast<uint64_t>(p.bytes_previously_downloaded)},
        {"percent", p.percent}
    };
}

// Called with mutex_ already held.
nlohmann::json DownloadQueue::job_to_json_locked(const DownloadJob& job) {
    nlohmann::json j;
    j["id"] = job.id;
    j["model_name"] = job.model_name;
    j["status"] = status_str(job.status);
    j["created_at"] = job.created_at;
    j["started_at"] = job.started_at > 0 ? nlohmann::json(job.started_at) : nlohmann::json(nullptr);
    j["finished_at"] = job.finished_at > 0 ? nlohmann::json(job.finished_at) : nlohmann::json(nullptr);
    j["error"] = job.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.error);
    j["progress"] = progress_to_json(job.last_progress);
    return j;
}

// ---- ctor / dtor -----------------------------------------------------------

DownloadQueue::DownloadQueue(ModelManager* model_manager, const std::string& cache_dir)
    : model_manager_(model_manager),
      persist_path_(cache_dir + "/download_queue.json"),
      next_id_(static_cast<uint32_t>(std::time(nullptr))) {
    load_persisted();
}

DownloadQueue::~DownloadQueue() {
    shutdown();
}

// ---- generate_id -----------------------------------------------------------

std::string DownloadQueue::generate_id() {
    uint32_t val = next_id_.fetch_add(1);
    std::ostringstream oss;
    oss << "dl_" << std::hex << std::setw(8) << std::setfill('0') << val;
    return oss.str();
}

// ---- enqueue ---------------------------------------------------------------

std::string DownloadQueue::enqueue(const std::string& model_name,
                                    const nlohmann::json& model_data,
                                    bool do_not_upgrade) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Return existing job ID if this model is already active.
    for (const auto& job : jobs_) {
        if (job->model_name == model_name && !is_terminal(job->status)) {
            LOG(INFO, "DownloadQueue") << "Model " << model_name
                << " already downloading (job " << job->id << ")" << std::endl;
            return job->id;
        }
    }

    auto job = std::make_shared<DownloadJob>();
    job->id = generate_id();
    job->model_name = model_name;
    job->model_data = model_data;
    job->do_not_upgrade = do_not_upgrade;
    job->created_at = static_cast<int64_t>(std::time(nullptr));

    jobs_.push_back(job);

    LOG(INFO, "DownloadQueue") << "Enqueued " << model_name << " as job " << job->id << std::endl;

    // Spawn a dedicated thread per job (parallel downloads, matching current behaviour).
    job->worker_thread = std::thread([this, job] { run_job(job); });

    return job->id;
}

// ---- run_job ---------------------------------------------------------------

void DownloadQueue::run_job(std::shared_ptr<DownloadJob> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->status = JobStatus::Running;
        job->started_at = static_cast<int64_t>(std::time(nullptr));
    }
    event_cv_.notify_all();

    try {
        DownloadProgressCallback cb = [this, job](const DownloadProgress& p) -> bool {
            if (job->cancel_flag.load()) return false;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job->cancel_flag.load()) return false;

                job->last_progress = p;

                std::string type = p.complete ? "complete" : "progress";
                std::string ev = "event: " + type + "\ndata: "
                    + progress_to_json(p).dump() + "\n\n";

                job->event_buffer.push_back(ev);
                job->total_events++;
                if (job->event_buffer.size() > DownloadJob::kEventBufCap) {
                    job->event_buffer.pop_front();
                }
            }
            event_cv_.notify_all();
            return true;
        };

        model_manager_->download_model(
            job->model_name, job->model_data, job->do_not_upgrade, cb);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job->status != JobStatus::Cancelled) {
                job->status = JobStatus::Complete;
                job->finished_at = static_cast<int64_t>(std::time(nullptr));
                LOG(INFO, "DownloadQueue") << "Job " << job->id << " complete" << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string msg = e.what();
        bool cancelled = job->cancel_flag.load()
            || msg.find("cancelled") != std::string::npos
            || msg.find("Cancelled") != std::string::npos;

        job->finished_at = static_cast<int64_t>(std::time(nullptr));

        if (cancelled) {
            job->status = JobStatus::Cancelled;
            job->event_buffer.push_back("event: cancelled\ndata: {}\n\n");
            LOG(INFO, "DownloadQueue") << "Job " << job->id << " cancelled" << std::endl;
        } else {
            job->status = JobStatus::Failed;
            job->error = msg;
            nlohmann::json err = {{"error", msg}};
            job->event_buffer.push_back("event: error\ndata: " + err.dump() + "\n\n");
            LOG(ERROR, "DownloadQueue") << "Job " << job->id << " failed: " << msg << std::endl;
        }

        job->total_events++;
        if (job->event_buffer.size() > DownloadJob::kEventBufCap) {
            job->event_buffer.pop_front();
        }
    }

    event_cv_.notify_all();
    persist();
}

// ---- cancel ----------------------------------------------------------------

CancelResult DownloadQueue::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& job : jobs_) {
        if (job->id == id) {
            if (is_terminal(job->status)) return CancelResult::AlreadyTerminal;
            job->cancel_flag = true;
            LOG(INFO, "DownloadQueue") << "Cancel requested for job " << id << std::endl;
            return CancelResult::Ok;
        }
    }
    return CancelResult::NotFound;
}

// ---- shutdown --------------------------------------------------------------

void DownloadQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
        for (const auto& job : jobs_) {
            job->cancel_flag = true;
        }
    }
    event_cv_.notify_all();

    for (const auto& job : jobs_) {
        if (job->worker_thread.joinable()) {
            job->worker_thread.join();
        }
    }

    persist();
}

// ---- stream_job ------------------------------------------------------------

void DownloadQueue::stream_job(const std::string& id, httplib::Response& res) {
    // Find the job under lock, then release before setting up the provider.
    std::shared_ptr<DownloadJob> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& j : jobs_) {
            if (j->id == id) { job = j; break; }
        }
    }

    if (!job) {
        res.status = 404;
        res.set_content("{\"error\": \"Download job not found\"}", "application/json");
        return;
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider("text/event-stream",
        [this, job](size_t offset, httplib::DataSink& sink) -> bool {
            if (offset > 0) {
                sink.done();
                return false;
            }

            // Start replay from the oldest event currently in the buffer.
            uint64_t next_abs = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uint64_t buf_start = job->total_events > job->event_buffer.size()
                    ? job->total_events - static_cast<uint64_t>(job->event_buffer.size()) : 0;
                next_abs = buf_start;
            }

            while (true) {
                std::vector<std::string> to_send;
                bool terminal = false;

                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    event_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
                        return next_abs < job->total_events
                            || is_terminal(job->status)
                            || shutdown_;
                    });

                    uint64_t buf_start = job->total_events > job->event_buffer.size()
                        ? job->total_events - static_cast<uint64_t>(job->event_buffer.size()) : 0;

                    // If the ring buffer wrapped past us, jump to its start.
                    if (next_abs < buf_start) next_abs = buf_start;

                    while (next_abs < job->total_events) {
                        size_t idx = static_cast<size_t>(next_abs - buf_start);
                        to_send.push_back(job->event_buffer[idx]);
                        next_abs++;
                    }

                    terminal = is_terminal(job->status) && next_abs >= job->total_events;
                }

                for (const auto& ev : to_send) {
                    if (!sink.write(ev.c_str(), ev.size())) {
                        return false;  // client disconnected; download continues in background
                    }
                }

                if (terminal || shutdown_) {
                    sink.done();
                    return false;
                }
            }
        });
}

// ---- JSON accessors --------------------------------------------------------

nlohmann::json DownloadQueue::list_jobs_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& job : jobs_) {
        arr.push_back(job_to_json_locked(*job));
    }
    return arr;
}

nlohmann::json DownloadQueue::get_job_json(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& job : jobs_) {
        if (job->id == id) return job_to_json_locked(*job);
    }
    return nullptr;
}

// ---- persistence -----------------------------------------------------------

void DownloadQueue::persist() {
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Collect terminal jobs; cap history at 100.
        std::vector<size_t> terminal_indices;
        for (size_t i = 0; i < jobs_.size(); i++) {
            if (is_terminal(jobs_[i]->status)) terminal_indices.push_back(i);
        }
        size_t terminal_start = terminal_indices.size() > 100
            ? terminal_indices.size() - 100 : 0;
        std::vector<size_t> keep_terminal(
            terminal_indices.begin() + static_cast<long>(terminal_start),
            terminal_indices.end());

        for (size_t i = 0; i < jobs_.size(); i++) {
            const auto& job = jobs_[i];
            bool terminal = is_terminal(job->status);

            if (terminal) {
                bool keep = false;
                for (size_t idx : keep_terminal) {
                    if (idx == i) { keep = true; break; }
                }
                if (!keep) continue;
            }

            nlohmann::json j;
            j["id"] = job->id;
            j["model_name"] = job->model_name;
            j["model_data"] = job->model_data;
            j["do_not_upgrade"] = job->do_not_upgrade;
            j["created_at"] = job->created_at;
            // Save Running jobs as Queued so they resume on restart.
            JobStatus saved = (job->status == JobStatus::Running) ? JobStatus::Queued : job->status;
            j["status"] = status_str(saved);
            j["started_at"] = job->started_at;
            j["finished_at"] = job->finished_at;
            j["error"] = job->error;
            arr.push_back(j);
        }
    }

    std::string tmp = persist_path_ + ".tmp";
    try {
        std::ofstream f(tmp);
        if (f) {
            f << arr.dump(2);
            f.close();
            fs::rename(tmp, persist_path_);
        }
    } catch (...) {
        // Persistence failure is non-fatal.
    }
}

void DownloadQueue::load_persisted() {
    try {
        std::ifstream f(persist_path_);
        if (!f) return;

        nlohmann::json arr = nlohmann::json::parse(f);
        if (!arr.is_array()) return;

        for (const auto& j : arr) {
            auto job = std::make_shared<DownloadJob>();
            job->id = j.value("id", "");
            job->model_name = j.value("model_name", "");
            job->model_data = j.value("model_data", nlohmann::json::object());
            job->do_not_upgrade = j.value("do_not_upgrade", false);
            job->created_at = j.value("created_at", int64_t(0));
            job->status = status_from_str(j.value("status", "failed"));
            job->started_at = j.value("started_at", int64_t(0));
            job->finished_at = j.value("finished_at", int64_t(0));
            job->error = j.value("error", "");

            if (job->id.empty() || job->model_name.empty()) continue;

            jobs_.push_back(job);

            // Re-run any downloads that were interrupted by a server restart.
            // model_manager_'s .partial file support will resume from where they left off.
            if (job->status == JobStatus::Queued) {
                LOG(INFO, "DownloadQueue") << "Resuming interrupted download: "
                    << job->model_name << " (" << job->id << ")" << std::endl;
                job->worker_thread = std::thread([this, job] { run_job(job); });
            }
        }
    } catch (...) {
        // Corrupt or missing file — start fresh.
    }
}

} // namespace lemon
