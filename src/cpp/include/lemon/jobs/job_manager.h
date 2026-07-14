// The generic server-side job engine: a single persistent worker runs one
// client-posted job at a time, passing data forward through the job context,
// evaluating forward-only branches, and persisting every transition so a job
// survives client disconnect and server restart.
#pragma once

#include "lemon/jobs/job_ops.h"
#include "lemon/jobs/job_types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lemon {
namespace jobs {

class JobManager {
public:
    JobManager(std::string cache_dir, OpRegistry registry);
    ~JobManager();

    std::string create(const std::string& name, std::vector<StepRecord> steps, json inputs);
    json list() const;
    std::optional<json> get(const std::string& id) const;
    bool pause(const std::string& id);
    bool interrupt(const std::string& id);
    bool resume(const std::string& id);
    bool remove(const std::string& id, bool& active_out);

private:
    struct Control {
        std::atomic<bool> pause_requested{false};
        std::atomic<bool> interrupt_requested{false};
        CancelFlag cancel{false};
    };

    void worker_main();
    void execute(const std::string& id, const std::shared_ptr<Control>& ctrl);
    void persist_locked();
    void load_from_disk();
    void enqueue_locked(const std::string& id);
    std::shared_ptr<Control> control_for_locked(const std::string& id);

    std::string storage_path_;
    OpRegistry registry_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Job> jobs_;
    std::map<std::string, std::shared_ptr<Control>> controls_;
    std::vector<std::string> order_;  // newest first
    std::deque<std::string> queue_;
    std::string active_id_;
    uint64_t id_counter_ = 0;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace jobs
}  // namespace lemon
