#include "lemon/jobs/job_manager.h"

#include "lemon/jobs/job_expr.h"
#include "lemon/jobs/job_graph.h"
#include "lemon/utils/path_utils.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace lemon {
namespace jobs {

namespace {

constexpr size_t kMaxJobs = 50;

std::string iso_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

bool is_terminal(JobStatus s) {
    return s == JobStatus::Completed || s == JobStatus::Failed;
}

StepRecord* find_step(Job& job, const std::string& id) {
    for (auto& s : job.steps)
        if (s.id == id) return &s;
    return nullptr;
}

int step_index(const Job& job, const std::string& id) {
    for (int i = 0; i < (int)job.steps.size(); ++i)
        if (job.steps[i].id == id) return i;
    return -1;
}

std::string next_after_success(const Job& job, const StepRecord& from) {
    for (const Case& c : from.branch)
        if (eval_condition(c.when, job.context)) return c.goto_id;
    if (!from.on_done.empty()) return from.on_done;
    const int idx = step_index(job, from.id);
    if (idx >= 0 && idx + 1 < (int)job.steps.size()) return job.steps[idx + 1].id;
    return "";
}

std::string next_in_list(const Job& job, const std::string& id) {
    const int idx = step_index(job, id);
    if (idx >= 0 && idx + 1 < (int)job.steps.size()) return job.steps[idx + 1].id;
    return "";
}

bool job_needs_exclusive(const Job& job, const OpRegistry& registry) {
    for (const auto& s : job.steps) {
        const OpHandler* h = registry.find(s.op);
        if (h && h->exclusive) return true;
    }
    return false;
}

}

JobManager::JobManager(std::string cache_dir, OpRegistry registry)
    : storage_path_((fs::path(cache_dir) / "jobs.json").string()),
      registry_(std::move(registry)) {
    load_from_disk();
    worker_ = std::thread(&JobManager::worker_main, this);
}

JobManager::~JobManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        for (auto& kv : controls_) {
            kv.second->interrupt_requested.store(true);
            kv.second->cancel.store(true);
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void JobManager::load_from_disk() {
    json doc;
    {
        std::ifstream in(lemon::utils::path_from_utf8(storage_path_));
        if (!in) return;
        try {
            doc = json::parse(in);
        } catch (const std::exception& e) {
            LOG(WARNING, "Jobs") << "Could not load " << storage_path_ << ": " << e.what()
                                 << std::endl;
            return;
        }
    }
    try {
        int loaded = 0, recovered = 0, dropped = 0;
        for (const auto& jj : doc.value("jobs", json::array())) {
            Job job = Job::from_json(jj);
            if (job.id.empty()) continue;
            if (job.deleted) {
                dropped++;
                LOG(INFO, "Jobs") << "dropping tombstoned job " << job.id << std::endl;
                continue;
            }
            if (job.status == JobStatus::Running || job.status == JobStatus::Queued) {
                job.status = JobStatus::Interrupted;
                job.error = "server restarted while the job was active";
                if (StepRecord* s = find_step(job, job.cursor))
                    if (s->status == StepStatus::Running) s->status = StepStatus::Pending;
                recovered++;
                LOG(WARNING, "Jobs") << "recovered active job " << job.id
                                     << " as interrupted (resumable at step '" << job.cursor
                                     << "')" << std::endl;
            }
            const std::string id = job.id;
            const size_t dash = id.rfind('-');
            if (dash != std::string::npos && dash + 1 < id.size()) {
                char* end = nullptr;
                const unsigned long long suffix = std::strtoull(id.c_str() + dash + 1, &end, 10);
                if (end && *end == '\0' && suffix > id_counter_) id_counter_ = suffix;
            }
            controls_[id] = std::make_shared<Control>();
            order_.push_back(id);
            jobs_.emplace(id, std::move(job));
            loaded++;
        }
        if (loaded)
            LOG(INFO, "Jobs") << "loaded " << loaded << " job(s) from " << storage_path_ << " ("
                              << recovered << " recovered as interrupted)" << std::endl;
        if (dropped) {
            std::lock_guard<std::mutex> lock(mutex_);
            persist_locked();
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "Jobs") << "Could not restore jobs from " << storage_path_ << ": "
                             << e.what() << std::endl;
    }
}

void JobManager::persist_locked() {
    json arr = json::array();
    for (const auto& id : order_) {
        auto it = jobs_.find(id);
        if (it != jobs_.end()) arr.push_back(it->second.to_json());
    }
    json doc = {{"version", 1}, {"jobs", arr}};
    const std::string tmp = storage_path_ + ".tmp";
    bool write_ok = false;
    {
        std::ofstream out(lemon::utils::path_from_utf8(tmp), std::ios::trunc);
        out << doc.dump(2);
        out.flush();
        write_ok = out.good();
    }
    if (!write_ok) {
        LOG(WARNING, "Jobs") << "Could not write jobs to " << tmp
                             << " (keeping previous state on disk)" << std::endl;
        std::error_code rm;
        fs::remove(lemon::utils::path_from_utf8(tmp), rm);
        return;
    }
    std::error_code ec;
    lemon::utils::atomic_replace_file(lemon::utils::path_from_utf8(tmp),
                                      lemon::utils::path_from_utf8(storage_path_), ec);
    if (ec) LOG(WARNING, "Jobs") << "Could not persist jobs: " << ec.message() << std::endl;
}

void JobManager::enqueue_locked(const std::string& id) {
    queue_.push_back(id);
}

std::shared_ptr<JobManager::Control> JobManager::control_for_locked(const std::string& id) {
    auto it = controls_.find(id);
    if (it != controls_.end()) return it->second;
    auto ctrl = std::make_shared<Control>();
    controls_[id] = ctrl;
    return ctrl;
}

std::string JobManager::create(const std::string& name, std::vector<StepRecord> steps,
                               json inputs) {
    const std::string err = validate_steps(steps, registry_.names());
    if (!err.empty()) throw JobError(400, err);

    std::lock_guard<std::mutex> lock(mutex_);

    size_t live = 0;
    bool evictable = false;
    for (const auto& existing : order_) {
        auto jit = jobs_.find(existing);
        if (jit == jobs_.end() || jit->second.deleted) continue;
        live++;
        if (is_terminal(jit->second.status)) evictable = true;
    }
    if (live >= kMaxJobs && !evictable)
        throw JobError(429, "job limit (" + std::to_string(kMaxJobs)
                            + ") reached and every existing job is still active or resumable; "
                              "delete a job first");

    Job job;
    char stamp[24];
    const std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_utc);
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%06llu", (unsigned long long)(++id_counter_));
    job.id = std::string("job-") + stamp + "-" + suffix;
    job.name = name;
    job.status = JobStatus::Queued;
    job.inputs = inputs.is_null() ? json::object() : inputs;
    job.context = {{"inputs", job.inputs}};
    job.steps = std::move(steps);
    job.cursor = job.steps.front().id;
    job.created_at = iso_now();

    const std::string id = job.id;
    const size_t n_steps = job.steps.size();
    order_.insert(order_.begin(), id);
    controls_[id] = std::make_shared<Control>();
    jobs_.emplace(id, std::move(job));

    while (order_.size() > kMaxJobs) {
        bool evicted = false;
        for (auto rit = order_.rbegin(); rit != order_.rend(); ++rit) {
            auto jit = jobs_.find(*rit);
            if (jit != jobs_.end() && is_terminal(jit->second.status)) {
                const std::string victim = *rit;
                jobs_.erase(victim);
                controls_.erase(victim);
                order_.erase(std::next(rit).base());
                evicted = true;
                break;
            }
        }
        if (!evicted) break;
    }

    enqueue_locked(id);
    persist_locked();
    cv_.notify_all();
    LOG(INFO, "Jobs") << "created job " << id << " '" << name << "' (" << n_steps << " steps)"
                      << std::endl;
    return id;
}

json JobManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto& id : order_) {
        auto it = jobs_.find(id);
        if (it != jobs_.end() && !it->second.deleted) out.push_back(it->second.to_summary_json());
    }
    return out;
}

std::optional<json> JobManager::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.deleted) return std::nullopt;
    return it->second.to_json();
}

bool JobManager::pause(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.deleted) return false;
    if (it->second.status == JobStatus::Queued) {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
        it->second.status = JobStatus::Paused;
        persist_locked();
        LOG(INFO, "Jobs") << "paused queued job " << id << std::endl;
        return true;
    }
    if (it->second.status != JobStatus::Running) return false;
    control_for_locked(id)->pause_requested.store(true);
    LOG(INFO, "Jobs") << "pause requested for job " << id << " (takes effect at next step)"
                      << std::endl;
    return true;
}

bool JobManager::interrupt(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.deleted) return false;
    if (it->second.status == JobStatus::Queued) {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
        it->second.status = JobStatus::Interrupted;
        persist_locked();
        LOG(INFO, "Jobs") << "interrupted queued job " << id << std::endl;
        return true;
    }
    if (it->second.status != JobStatus::Running) return false;
    auto ctrl = control_for_locked(id);
    ctrl->interrupt_requested.store(true);
    ctrl->cancel.store(true);
    LOG(INFO, "Jobs") << "interrupt requested for job " << id << std::endl;
    return true;
}

bool JobManager::resume(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.deleted) return false;
    if (it->second.status != JobStatus::Paused && it->second.status != JobStatus::Interrupted)
        return false;
    auto ctrl = control_for_locked(id);
    ctrl->pause_requested.store(false);
    ctrl->interrupt_requested.store(false);
    ctrl->cancel.store(false);
    it->second.status = JobStatus::Queued;
    it->second.error.clear();
    enqueue_locked(id);
    persist_locked();
    cv_.notify_all();
    LOG(INFO, "Jobs") << "resumed job " << id << " at step '" << it->second.cursor << "'"
                      << std::endl;
    return true;
}

bool JobManager::remove(const std::string& id, bool& active_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_out = false;
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.deleted) return false;
    if (id == active_id_) {
        active_out = true;
        it->second.deleted = true;
        persist_locked();
        auto ctrl = control_for_locked(id);
        ctrl->interrupt_requested.store(true);
        ctrl->cancel.store(true);
        ctrl->delete_requested.store(true);
        LOG(INFO, "Jobs") << "delete requested for active job " << id
                          << " (tombstoned; erased after cleanup)" << std::endl;
        return true;
    }
    if (it->second.status == JobStatus::Paused || it->second.status == JobStatus::Interrupted) {
        it->second.deleted = true;
        control_for_locked(id)->delete_requested.store(true);
        persist_locked();
        enqueue_locked(id);
        cv_.notify_all();
        LOG(INFO, "Jobs") << "delete requested for " << to_string(it->second.status)
                          << " job " << id << " (tombstoned; erased after reconcile)"
                          << std::endl;
        return true;
    }
    jobs_.erase(id);
    controls_.erase(id);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
    persist_locked();
    LOG(INFO, "Jobs") << "deleted job " << id << std::endl;
    return true;
}

void JobManager::worker_main() {
    while (true) {
        std::string id;
        std::shared_ptr<Control> ctrl;
        bool exclusive = false;
        bool cleanup_only = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            if (queue_.empty()) continue;
            id = queue_.front();
            queue_.pop_front();
            auto it = jobs_.find(id);
            if (it == jobs_.end()) continue;
            ctrl = control_for_locked(id);
            if (it->second.deleted) {
                cleanup_only = true;
            } else if (it->second.status != JobStatus::Queued) {
                continue;
            } else {
                it->second.status = JobStatus::Running;
                if (it->second.started_at.empty()) it->second.started_at = iso_now();
                active_id_ = id;
                exclusive = job_needs_exclusive(it->second, registry_);
                persist_locked();
                LOG(INFO, "Jobs") << "running job " << id << " from step '" << it->second.cursor
                                  << "'" << std::endl;
            }
        }

        if (cleanup_only) {
            if (registry_.reconcile_unload) registry_.reconcile_unload(id);
            if (registry_.discard_exclusive) registry_.discard_exclusive(id);
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.erase(id);
            controls_.erase(id);
            order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
            queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
            persist_locked();
            LOG(INFO, "Jobs") << "erased deleted job " << id << " after reconcile" << std::endl;
            continue;
        }

        struct ExclusiveGuard {
            const OpRegistry& reg;
            const std::string& id;
            bool held = false;
            bool begin(CancelFlag* cancel) {
                if (reg.begin_exclusive && !reg.begin_exclusive(id, cancel)) return false;
                held = true;
                LOG(INFO, "Jobs") << "job " << id << " acquired exclusive slot" << std::endl;
                return true;
            }
            ~ExclusiveGuard() {
                if (held && reg.end_exclusive) reg.end_exclusive();
                if (held) LOG(INFO, "Jobs") << "job " << id << " released exclusive slot" << std::endl;
            }
        } guard{registry_, id};

        bool ready = !exclusive || guard.begin(&ctrl->cancel);
        if (ready && guard.held && registry_.restore_exclusive) {
            json manifest = json::object();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = jobs_.find(id);
                if (it != jobs_.end()) {
                    const Job& job = it->second;
                    for (const auto& s : job.steps) {
                        if (s.status != StepStatus::Completed) continue;
                        try {
                            if (s.op == "load") {
                                json params = resolve_refs(s.params, job.context);
                                if (params.contains("model") && params["model"].is_string())
                                    manifest[params["model"].get<std::string>()] = params;
                            } else if (s.op == "unload") {
                                json params = resolve_refs(s.params, job.context);
                                if (params.contains("model") && params["model"].is_string())
                                    manifest.erase(params["model"].get<std::string>());
                                else
                                    manifest = json::object();
                            }
                        } catch (const std::exception& e) {
                            LOG(WARNING, "Jobs")
                                << "job " << id << " restore manifest skipped step '" << s.id
                                << "': " << e.what() << std::endl;
                        }
                    }
                }
            }
            ready = registry_.restore_exclusive(id, manifest, &ctrl->cancel);
        }
        if (ready) {
            execute(id, ctrl);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it != jobs_.end()) {
                it->second.status = JobStatus::Interrupted;
                persist_locked();
            }
            LOG(INFO, "Jobs") << "job " << id
                              << " interrupted while waiting for the exclusive slot" << std::endl;
        }
        bool interrupted = false;
        bool terminal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it != jobs_.end()) {
                interrupted = it->second.status == JobStatus::Interrupted;
                terminal = is_terminal(it->second.status);
            }
            active_id_.clear();
        }

        if (interrupted && registry_.reconcile_unload) {
            registry_.reconcile_unload(id);
            LOG(INFO, "Jobs") << "job " << id << " interrupted — reconciled job-loaded model(s)"
                              << std::endl;
        }
        if (terminal && registry_.discard_exclusive) registry_.discard_exclusive(id);

        if (ctrl->delete_requested.load()) {
            if (registry_.discard_exclusive) registry_.discard_exclusive(id);
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.erase(id);
            controls_.erase(id);
            order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
            queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
            persist_locked();
            LOG(INFO, "Jobs") << "erased deleted job " << id << " after cleanup" << std::endl;
        }
    }
}

void JobManager::execute(const std::string& id, const std::shared_ptr<Control>& ctrl) {
    using clock = std::chrono::steady_clock;

    while (true) {
        json params;
        json context_snapshot;
        const OpHandler* handler = nullptr;
        std::string step_id;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return;
            Job& job = it->second;

            if (ctrl->interrupt_requested.load()) {
                if (StepRecord* s = find_step(job, job.cursor)) s->status = StepStatus::Pending;
                job.status = JobStatus::Interrupted;
                persist_locked();
                LOG(INFO, "Jobs") << "job " << id << " interrupted at step '" << job.cursor << "'"
                                  << std::endl;
                return;
            }
            if (ctrl->pause_requested.load()) {
                job.status = JobStatus::Paused;
                persist_locked();
                LOG(INFO, "Jobs") << "job " << id << " paused before step '" << job.cursor << "'"
                                  << std::endl;
                return;
            }
            if (job.cursor.empty()) {
                job.status = JobStatus::Completed;
                for (auto& st : job.steps)
                    if (st.status == StepStatus::Pending) st.status = StepStatus::Skipped;
                if (job.finished_at.empty()) job.finished_at = iso_now();
                persist_locked();
                LOG(INFO, "Jobs") << "job " << id << " completed" << std::endl;
                return;
            }

            StepRecord* s = find_step(job, job.cursor);
            if (!s) {
                job.status = JobStatus::Failed;
                job.error = "cursor points to unknown step '" + job.cursor + "'";
                job.finished_at = iso_now();
                persist_locked();
                return;
            }

            bool skip = false;
            try {
                skip = !eval_condition(s->when, job.context);
            } catch (const std::exception& e) {
                s->status = StepStatus::Failed;
                s->error = e.what();
                job.status = JobStatus::Failed;
                job.error = e.what();
                job.finished_at = iso_now();
                persist_locked();
                return;
            }
            if (skip) {
                s->status = StepStatus::Skipped;
                LOG(DEBUG, "Jobs") << "job " << id << " step '" << s->id
                                   << "' skipped (when=false)" << std::endl;
                job.cursor = next_in_list(job, s->id);
                persist_locked();
                continue;
            }

            handler = registry_.find(s->op);
            if (!handler) {
                s->status = StepStatus::Failed;
                s->error = "unknown op '" + s->op + "'";
                job.status = JobStatus::Failed;
                job.error = s->error;
                job.finished_at = iso_now();
                persist_locked();
                return;
            }

            try {
                params = resolve_refs(s->params, job.context);
            } catch (const std::exception& e) {
                s->status = StepStatus::Failed;
                s->error = e.what();
                job.status = JobStatus::Failed;
                job.error = e.what();
                job.finished_at = iso_now();
                persist_locked();
                return;
            }

            s->status = StepStatus::Running;
            step_id = s->id;
            context_snapshot = job.context;
            ctrl->cancel.store(false);
            persist_locked();
            LOG(DEBUG, "Jobs") << "job " << id << " running step '" << s->id << "' (op " << s->op
                               << ")" << std::endl;
        }

        json output;
        std::string run_error;
        bool ok = true;
        const auto t0 = clock::now();
        try {
            output = handler->run(params, context_snapshot, ctrl->cancel);
        } catch (const std::exception& e) {
            ok = false;
            run_error = e.what();
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return;
            Job& job = it->second;
            StepRecord* s = find_step(job, step_id);
            if (!s) return;
            s->duration_ms = ms;

            if (ctrl->interrupt_requested.load()) {
                s->status = StepStatus::Pending;
                s->error.clear();
                job.status = JobStatus::Interrupted;
                persist_locked();
                LOG(INFO, "Jobs") << "job " << id << " interrupted during step '" << step_id << "'"
                                  << std::endl;
                return;
            }

            if (ok) {
                s->output = output;
                job.context[s->id] = output;
                try {
                    for (auto it2 = s->extract.begin(); it2 != s->extract.end(); ++it2)
                        job.context[it2.key()] =
                            expr_detail::resolve_ref_path(it2.value().get<std::string>(), output);
                    const std::string next = next_after_success(job, *s);
                    s->status = StepStatus::Completed;
                    job.cursor = next;
                    LOG(DEBUG, "Jobs") << "job " << id << " step '" << step_id << "' completed in "
                                       << ms << "ms -> "
                                       << (next.empty() ? "end" : "'" + next + "'") << std::endl;
                } catch (const std::exception& e) {
                    s->status = StepStatus::Failed;
                    s->error = e.what();
                    job.status = JobStatus::Failed;
                    job.error = e.what();
                    job.finished_at = iso_now();
                    persist_locked();
                    LOG(ERROR, "Jobs") << "job " << id << " failed at step '" << step_id
                                       << "' (extract/branch): " << e.what() << std::endl;
                    return;
                }
            } else {
                s->status = StepStatus::Failed;
                s->error = run_error;
                if (s->on_fail == kOnFailAbort) {
                    job.status = JobStatus::Failed;
                    job.error = run_error;
                    job.finished_at = iso_now();
                    persist_locked();
                    LOG(ERROR, "Jobs") << "job " << id << " failed at step '" << step_id << "' (op "
                                       << s->op << "): " << run_error << std::endl;
                    return;
                } else if (s->on_fail == kOnFailContinue) {
                    s->failure_handled = true;
                    job.cursor = next_in_list(job, s->id);
                    LOG(WARNING, "Jobs") << "job " << id << " step '" << step_id
                                         << "' failed (" << run_error << "), continuing" << std::endl;
                } else {
                    s->failure_handled = true;
                    job.cursor = s->on_fail;
                    LOG(WARNING, "Jobs") << "job " << id << " step '" << step_id << "' failed ("
                                         << run_error << "), branching to '" << s->on_fail << "'"
                                         << std::endl;
                }
            }
            persist_locked();
        }
    }
}

}
}
