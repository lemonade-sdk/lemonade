// AutoOpt run lifecycle: registry, worker thread, staged pipeline, and
// server-side persistence (<cache_dir>/autoopt_runs.json).
#pragma once

#include "lemon/auto_opt_probes.h"
#include "lemon/auto_opt_types.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lemon {

class Router;
class ModelManager;

namespace autoopt {

struct AutoOptError : public std::runtime_error {
    int status;
    json body;
    AutoOptError(int status_, json body_)
        : std::runtime_error(body_.value("error", "autoopt error")),
          status(status_), body(std::move(body_)) {}
};

struct StageRecord {
    std::string name;
    std::string status = "pending";   // pending|running|completed|failed|skipped
    int64_t duration_ms = 0;
    std::string error;
    json data = json::object();
};

struct AutoOptRun {
    std::string id;
    std::string model;
    std::string checkpoint;
    BudgetTier budget = BudgetTier::Quick;
    WizardAnswers answers;
    bool allow_unload = false;
    std::string status = "queued";    // queued|running|completed|failed|cancelled
    std::string created_at;
    std::string finished_at;
    std::string summary;
    std::string error;
    std::vector<StageRecord> stages;
    std::vector<FitEstimate> fit_measurements;
    std::vector<BenchPoint> bench_measurements;
    std::optional<AutoOptResult> result;
    int stage_index = 0;
    std::string progress_detail;

    json to_summary_json() const;
    json to_json() const;
    static AutoOptRun from_json(const json& j);
};

class AutoOptManager {
public:
    AutoOptManager(Router* router, ModelManager* model_manager, std::string cache_dir,
                   std::unique_ptr<MeasurementProvider> provider = nullptr);
    ~AutoOptManager();

    // Returns the new run id; throws AutoOptError(409) when a run is active or
    // models are loaded without unload consent (standard/thorough only).
    std::string start(const std::string& model, BudgetTier budget, WizardAnswers answers,
                      bool allow_unload);
    json list_runs() const;
    std::optional<json> get_run(const std::string& id) const;
    bool cancel(const std::string& id);
    // Returns false when the run is active (delete refused) — 409 at the route.
    bool delete_run(const std::string& id, bool& active);
    json apply(const std::string& id, size_t preset_index);

private:
    void worker_main(std::string run_id);
    void persist_locked();
    void load_from_disk();
    StageRecord& stage_locked(AutoOptRun& run, const std::string& name);
    void finish_stage(const std::string& run_id, const std::string& name,
                      const std::string& status, const std::string& error = "");
    void set_progress(const std::string& run_id, const std::string& detail);

    Router* router_;
    ModelManager* model_manager_;
    std::string storage_path_;
    std::unique_ptr<MeasurementProvider> provider_;

    mutable std::mutex mutex_;
    std::map<std::string, AutoOptRun> runs_;
    std::vector<std::string> order_;   // newest first
    std::string active_run_id_;
    std::thread worker_;
    CancelFlag cancel_requested_{false};
};

}  // namespace autoopt
}  // namespace lemon
