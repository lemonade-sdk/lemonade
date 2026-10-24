// Contract and unit tests for Model Sync and Auto-Update features.
// Tests:
// - RuntimeConfig auto_update_models setting and validation.
// - SyncService::should_auto_update resolution hierarchy.
// - The sync queue: dedup, attach, join, generations, cancellation, teardown.
//
// The queue is driven through SyncService with its own deps; the facade wires
// the same deps to the real scan and pull and adds nothing of its own.

#include "lemon/models/alias_manager.h"
#include "lemon/models/model_info.h"
#include "lemon/models/store.h"
#include "lemon/models/sync_service.h"
#include "lemon/recipe_options.h"
#include "lemon/runtime_config.h"
#include "support/test_state_isolation.h"

#include <lemon/utils/path_utils.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using lemon::json;
using lemon::models::AliasManager;
using lemon::models::ModelStore;
using lemon::models::SyncService;
using lemon::models::UpdateScanResult;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

// Routes worker phases to the one test that wants to park on them. Install and
// clear are guarded and the callback is copied out before it runs, so a gate can
// retire while the worker is between phases.
struct PhaseBus {
    std::function<void(const std::string&)> sink;
    mutable std::mutex m;

    void install(std::function<void(const std::string&)> fn) {
        std::lock_guard<std::mutex> lock(m);
        sink = std::move(fn);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m);
        sink = nullptr;
    }

    void operator()(const std::string& phase) const {
        std::function<void(const std::string&)> target;
        {
            std::lock_guard<std::mutex> lock(m);
            target = sink;
        }
        if (target) target(phase);
    }
};

// The queue's world: a canned check, a download recorder, and a store holding the rows it
// reads. No composer, so it never composes; no alias, so names resolve to themselves.
struct SyncFixture {
    std::function<UpdateScanResult(const std::vector<std::string>&)> check_fn =
        [](const std::vector<std::string>&) { return UpdateScanResult{}; };
    std::function<void(const std::string&, const json&, bool, lemon::DownloadProgressCallback)>
        download_fn =
            [](const std::string&, const json&, bool, lemon::DownloadProgressCallback) {};
    AliasManager aliases{""};
    ModelStore store{aliases};
    std::atomic<int> scan_calls{0};
    std::atomic<int> target_checks{0};
    PhaseBus bus;

    SyncService::Deps deps() {
        SyncService::Deps d;
        d.scan = [this](const std::vector<std::string>& targets) {
            ++scan_calls;
            if (targets.size() == 1) ++target_checks;
            return check_fn(targets);
        };
        d.download = [this](const std::string& name, const json& options, bool do_not_upgrade,
                            lemon::DownloadProgressCallback progress) {
            download_fn(name, options, do_not_upgrade, progress);
        };
        d.store = &store;
        d.phase = [this](const std::string& phase) { bus(phase); };
        return d;
    }

    SyncService service() { return SyncService(deps()); }
    std::unique_ptr<SyncService> make_service() { return std::make_unique<SyncService>(deps()); }

    // Published, not handed back: a flag set on a reference afterwards lands nowhere.
    void downloaded(const std::string& name, bool update_available = false) {
        lemon::ModelInfo info;
        info.model_name = name;
        info.downloaded = true;
        info.update_available = update_available;
        ModelStore::Write row{store};
        row.draft()[name] = std::move(info);
    }

    bool update_available(const std::string& name) const {
        const lemon::ModelInfo* row = store.read()->find(name);
        return row != nullptr && row->update_available;
    }
};

// Parks the sync worker at a named phase so a test can act while a generation is
// genuinely in flight, and takes itself off the bus on the way out.
//
// Declaration order is load-bearing: declare the gate BEFORE the SyncService whose
// worker it parks, and declare the service's own handle after it. Destruction runs
// in reverse, so the worker is released first, ~SyncService joins it while this
// gate's mutex and condition variable are still alive, and only then does the gate
// retire. Declaring the service first frees those primitives underneath a worker
// parked on them, and ~SyncService joins a thread that can never wake: the test
// hangs. Every scenario here was written that way, and it hung about once in forty
// runs.
class PhaseGate {
public:
    PhaseGate(PhaseBus& bus, std::string phase_prefix)
        : bus_(bus), phase_prefix_(std::move(phase_prefix)) {
        bus_.install([this](const std::string& phase) { arrive(phase); });
    }

    ~PhaseGate() {
        {
            std::lock_guard<std::mutex> lock(m_);
            dead_ = true;
            resume_ = true;
        }
        cv_.notify_all();
        bus_.clear();
    }

    PhaseGate(const PhaseGate&) = delete;
    PhaseGate& operator=(const PhaseGate&) = delete;

    // False means the worker never reached the phase: the scenario's premise is
    // gone, which is a failed check rather than a reason to go on waiting.
    bool wait_until_reached(std::chrono::seconds budget = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, budget, [this] { return reached_ || dead_; });
        return reached_;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_);
            resume_ = true;
        }
        cv_.notify_all();
    }

    int hits() const { return count(phase_prefix_); }

    // Arrivals seen since construction whose name starts with `phase_prefix`, so a
    // scenario can park on one phase and count a different one.
    int count(const std::string& phase_prefix) const {
        std::lock_guard<std::mutex> lock(m_);
        int total = 0;
        for (const auto& seen : seen_) {
            if (seen.first.rfind(phase_prefix, 0) == 0) total += seen.second;
        }
        return total;
    }

    // Wakes the worker at scope exit. Declare the returned guard AFTER the service
    // so it runs before ~SyncService; without it a parked worker is joined
    // unreleased and the run sits out the test's timeout instead of finishing.
    struct Releaser {
        PhaseGate& gate;
        ~Releaser() { gate.release(); }
    };

    Releaser auto_release() { return Releaser{*this}; }

private:
    void arrive(const std::string& phase) {
        std::unique_lock<std::mutex> lock(m_);
        ++seen_[phase];
        if (phase.rfind(phase_prefix_, 0) != 0) return;
        if (dead_) return;
        reached_ = true;
        cv_.notify_all();
        // Bounded: a gate nobody releases must fail the run, not park a worker for
        // the whole ctest timeout.
        cv_.wait_for(lock, std::chrono::seconds(30), [this] { return resume_ || dead_; });
    }

    PhaseBus& bus_;
    const std::string phase_prefix_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool reached_ = false;
    bool resume_ = false;
    bool dead_ = false;
    std::map<std::string, int> seen_;
};

// True when `needle` names the same model as one entry of `list`, on either side.
static bool contains(const std::vector<std::string>& list, const std::string& needle) {
    return std::find(list.begin(), list.end(), needle) != list.end();
}

int main() {
    std::cout << "=== Model Sync & Auto-Update Unit Tests ===" << std::endl;

    const std::filesystem::path temp_cache = lemon::test::state_dir();
    std::error_code ec;
    set_env_var("LEMONADE_CACHE_DIR", temp_cache.string());
    set_env_var("HF_HOME", (temp_cache / "huggingface").string());

    // Test 1: RuntimeConfig default for auto_update_models is false
    json initial_config = {
        {"offline", false},
        {"disable_model_filtering", true},
        {"enable_dgpu_gtt", false},
        {"no_fetch_executables", true},
        {"auto_check_model_updates", true},
        {"auto_update_models", false}
    };
    lemon::RuntimeConfig cfg(initial_config);
    lemon::RuntimeConfig::set_global(&cfg);
    check("RuntimeConfig default auto_update_models is false", cfg.auto_update_models() == false);

    // Test 2: RuntimeConfig set auto_update_models boolean validation
    try {
        cfg.set(json{{"auto_update_models", true}});
        check("RuntimeConfig set auto_update_models=true succeeds", cfg.auto_update_models() == true);
    } catch (const std::exception& e) {
        std::printf("Exception in test 2: %s\n", e.what());
        check("RuntimeConfig set auto_update_models=true succeeds", false);
    } catch (...) {
        check("RuntimeConfig set auto_update_models=true succeeds", false);
    }

    try {
        cfg.set(json{{"auto_update_models", "not_a_boolean"}});
        check("RuntimeConfig set invalid type for auto_update_models throws", false);
    } catch (const std::invalid_argument&) {
        check("RuntimeConfig set invalid type for auto_update_models throws", true);
    } catch (...) {
        check("RuntimeConfig set invalid type for auto_update_models throws", false);
    }

    lemon::RuntimeConfig* gcfg = lemon::RuntimeConfig::global();
    if (!gcfg) {
        gcfg = &cfg;
    }
    gcfg->set(json{{"auto_update_models", false}});

    // Test 3: should_auto_update precedence
    SyncFixture plain;
    SyncService auto_update_svc = plain.service();

    // Case A: info.auto_update is std::nullopt -> uses global RuntimeConfig (false)
    lemon::ModelInfo info1;
    info1.model_name = "test-model-1";
    info1.auto_update = std::nullopt;
    check("should_auto_update fallback to global (false)",
          auto_update_svc.should_auto_update(info1) == false);

    gcfg->set(json{{"auto_update_models", true}});
    check("should_auto_update fallback to global (true)",
          auto_update_svc.should_auto_update(info1) == true);

    // Case B: Explicit per-model override auto_update = true overrides global false
    gcfg->set(json{{"auto_update_models", false}});
    lemon::ModelInfo info2;
    info2.model_name = "test-model-2";
    info2.auto_update = true;
    check("per-model auto_update=true overrides global false",
          auto_update_svc.should_auto_update(info2) == true);

    // Case C: Explicit per-model override auto_update = false overrides global true
    gcfg->set(json{{"auto_update_models", true}});
    lemon::ModelInfo info3;
    info3.model_name = "test-model-3";
    info3.auto_update = false;
    check("per-model auto_update=false overrides global true",
          auto_update_svc.should_auto_update(info3) == false);

    gcfg->set(json{{"auto_update_models", false}});

    // Test 4: idle status and a dry run that finds nothing to do
    SyncFixture idle_fixture;
    idle_fixture.downloaded("Tiny-Test-Model-GGUF");
    idle_fixture.check_fn = [](const std::vector<std::string>&) {
        UpdateScanResult r;
        r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
        return r;
    };
    SyncService idle = idle_fixture.service();

    json status = idle.status();
    check("status returns idle status", status.value("status", "") == "idle");
    check("status already_in_progress is false",
          status.value("already_in_progress", true) == false);

    json dry_run_res = idle.sync({}, /*dry_run=*/true);
    check("sync dry_run returns status",
          dry_run_res.contains("status") && dry_run_res.contains("checked_count"));

    // Test 5: RecipeOptions auto_update preservation
    json ro_json = {{"auto_update", true}};
    lemon::RecipeOptions ro("llamacpp", ro_json);
    check("RecipeOptions auto_update parsed successfully", ro.to_json().value("auto_update", false) == true);

    json ro_json_null = {{"auto_update", nullptr}};
    lemon::RecipeOptions ro_null("llamacpp", ro_json_null);
    check("RecipeOptions auto_update nullptr parsed successfully", ro_null.to_json().contains("auto_update") == false);

    // Test 6: status carries the whole sync-state contract
    json status_fields = idle.status();
    check("status contains checked_count", status_fields.contains("checked_count"));
    check("status contains updated_count", status_fields.contains("updated_count"));
    check("status contains models_updated", status_fields.contains("models_updated"));
    check("status contains models_up_to_date", status_fields.contains("models_up_to_date"));
    check("status contains status field", status_fields.contains("status"));
    check("status contains terminal_error field", status_fields.contains("terminal_error"));

    // Test 8: Single-pass sync execution
    SyncFixture pass_fixture;
    pass_fixture.downloaded("Tiny-Test-Model-GGUF");
    pass_fixture.check_fn = [](const std::vector<std::string>&) {
        UpdateScanResult r;
        r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
        return r;
    };
    SyncService pass = pass_fixture.service();
    auto pass_enq = pass.enqueue({});
    json exec_res = pass.wait_for(pass_enq.sync_id);
    std::string s_status = exec_res.value("status", "");
    check("a queued sync finishes with a valid status",
          s_status == "idle" || s_status == "success" || s_status == "failed");

    // Test 10: UpdateScanResult registry failure handling
    SyncFixture fail_fixture;
    fail_fixture.check_fn = [](const std::vector<std::string>& t) {
        UpdateScanResult r;
        for (const auto& m : t) {
            r.failed_models[m] = "Repository not found";
        }
        return r;
    };
    SyncService fail_svc = fail_fixture.service();
    json dry_fail = fail_svc.sync({"non_existent_model_test_xyz"}, /*dry_run=*/true);
    check("registry failure populates failed_models",
          dry_fail.contains("failed_models") &&
              dry_fail["failed_models"].contains("non_existent_model_test_xyz"));
    check("registry failure sets status to failed", dry_fail.value("status", "") == "failed");
    check("registry failure does not increment checked_count",
          dry_fail.value("checked_count", 0) == 0);

    // Test 11: Overlapping request target deduplication
    SyncFixture dedup_fixture;
    dedup_fixture.check_fn = [](const std::vector<std::string>& t) {
        UpdateScanResult r;
        for (const auto& m : t) {
            r.failed_models[m] = "Repository not found";
        }
        return r;
    };
    PhaseGate during_full_check(dedup_fixture.bus, "full_check_done");
    SyncService dedup = dedup_fixture.service();
    auto release_on_exit = during_full_check.auto_release();
    dedup.enqueue({});
    check("the worker parked in the full check", during_full_check.wait_until_reached());

    auto second_enqueue = dedup.enqueue({"non_existent_model_test_xyz"});
    check("overlapping enqueue detects existing sync running", second_enqueue.already_running == true);

    during_full_check.release();
    dedup.wait_until_idle();

    // Test 12: Deterministic overlapping full check and targeted enqueue deduplication
    {
        SyncFixture overlap_fixture;
        overlap_fixture.downloaded("Tiny-Test-Model-GGUF");
        overlap_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        PhaseGate full_check(overlap_fixture.bus, "full_check_done");
        SyncService overlap = overlap_fixture.service();
        auto release_on_exit = full_check.auto_release();

        overlap.enqueue({});
        check("the worker parked at the end of the full check",
              full_check.wait_until_reached());

        auto enqueued_during_run = overlap.enqueue({"Tiny-Test-Model-GGUF"});
        check("enqueue during active sync returns true", enqueued_during_run.already_running == true);

        full_check.release();
        overlap.wait_until_idle();

        json overlap_exec = overlap.status();
        std::string o_status = overlap_exec.value("status", "");
        check("overlap execution status is valid",
              o_status == "idle" || o_status == "success" || o_status == "failed");
        check("full check callback triggered exactly once",
              full_check.count("full_check_done") == 1);
        check("the attached target reuses the running pass, it triggers no check of its own",
              overlap_fixture.target_checks.load() == 0);

        int target_count = 0;
        if (overlap_exec.contains("models_up_to_date") && overlap_exec["models_up_to_date"].is_array()) {
            for (const auto& item : overlap_exec["models_up_to_date"]) {
                if (item == "Tiny-Test-Model-GGUF") target_count++;
            }
        }
        if (overlap_exec.contains("models_updated") && overlap_exec["models_updated"].is_array()) {
            for (const auto& item : overlap_exec["models_updated"]) {
                if (item == "Tiny-Test-Model-GGUF") target_count++;
            }
        }
        if (overlap_exec.contains("failed_models") && overlap_exec["failed_models"].is_object()) {
            if (overlap_exec["failed_models"].contains("Tiny-Test-Model-GGUF")) {
                target_count++;
            }
        }
        check("overlapping target checked and processed exactly once", target_count == 1);
    }

    // Test 13: Synchronous sync joins active sync and waits for completion
    {
        SyncFixture join_fixture;
        join_fixture.downloaded("Tiny-Test-Model-GGUF");
        join_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        PhaseGate full_check(join_fixture.bus, "full_check_done");
        SyncService sync_join = join_fixture.service();
        auto release_on_exit = full_check.auto_release();

        sync_join.enqueue({});
        check("the worker parked at the end of the full check", full_check.wait_until_reached());

        std::atomic<bool> join_finished{false};
        json join_result;
        std::thread joiner([&]() {
            join_result = sync_join.sync({"Tiny-Test-Model-GGUF"}, /*dry_run=*/false);
            join_finished = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check("synchronous joiner blocks while sync is running", join_finished.load() == false);

        full_check.release();
        sync_join.wait_until_idle();
        joiner.join();

        check("synchronous joiner finishes after sync completes", join_finished.load() == true);
        std::string j_status = join_result.value("status", "");
        check("synchronous joiner receives completed status not in_progress", j_status != "in_progress");
    }

    // Test 14: Generation ID tracking and historical status lookup
    {
        SyncFixture gen_fixture;
        gen_fixture.downloaded("Tiny-Test-Model-GGUF");
        gen_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        PhaseGate during_full_check(gen_fixture.bus, "full_check_done");
        SyncService gen = gen_fixture.service();
        auto release_on_exit = during_full_check.auto_release();

        auto enq1 = gen.enqueue({});
        check("first sync enqueue allocates sync_id > 0", enq1.sync_id > 0);
        check("first sync enqueue already_running is false", enq1.already_running == false);
        check("the worker parked in the full check", during_full_check.wait_until_reached());

        auto enq2 = gen.enqueue({"non_existent_model_test_xyz"});
        check("overlapping sync enqueue returns same sync_id", enq2.sync_id == enq1.sync_id);
        check("overlapping sync enqueue already_running is true", enq2.already_running == true);

        during_full_check.release();
        gen.wait_until_idle();
        uint64_t completed_gen = gen.status().value("completed_sync_id", 0);
        check("completed_sync_id matches first sync_id", completed_gen == enq1.sync_id);

        json historical_status = gen.status(enq1.sync_id);
        check("historical status lookup returns completed generation result",
              historical_status.value("status", "") != "in_progress" &&
                  historical_status.value("sync_id", 0) == enq1.sync_id);

        // Start a second sync job
        auto enq3 = gen.enqueue({});
        check("second sync enqueue allocates new sync_id", enq3.sync_id > enq1.sync_id);
        json hist_old = gen.status(enq1.sync_id);
        check("historical status for previous sync_id is preserved while new job is running",
              hist_old.value("status", "") != "in_progress" &&
                  hist_old.value("sync_id", 0) == enq1.sync_id);
        gen.wait_until_idle();
    }

    // Test 15: A model whose fresh check fails is not updated, whatever its
    // cached update_available flag once said
    {
        SyncFixture stale_fixture;
        stale_fixture.downloaded("Tiny-Test-Model-GGUF", /*update_available=*/true);
        stale_fixture.check_fn = [](const std::vector<std::string>& t) {
            UpdateScanResult r;
            for (const auto& m : t) {
                r.failed_models[m] = "Remote registry check failed";
            }
            return r;
        };
        std::mutex download_m;
        std::vector<std::string> downloaded_models;
        stale_fixture.download_fn = [&](const std::string& name, const json&, bool,
                                        lemon::DownloadProgressCallback) {
            std::lock_guard<std::mutex> lock(download_m);
            downloaded_models.push_back(name);
        };
        SyncService stale = stale_fixture.service();

        check("the stale flag starts out set on the model under test",
              stale_fixture.update_available("Tiny-Test-Model-GGUF"));

        auto enq = stale.enqueue({"Tiny-Test-Model-GGUF"});
        json stale_exec = stale.wait_for(enq.sync_id);
        check("failed fresh check populates failed_models",
              stale_exec.contains("failed_models") &&
                  stale_exec["failed_models"].contains("Tiny-Test-Model-GGUF"));
        check("failed fresh check does not schedule update",
              stale_exec.contains("models_updated") && stale_exec["models_updated"].empty());
        std::lock_guard<std::mutex> lock(download_m);
        check("a stale flag never downloads a model on its own", downloaded_models.empty());
    }

    // Test 16: Empty full-sync request arriving during running targeted sync triggers full check
    {
        SyncFixture full_fixture;
        full_fixture.downloaded("Tiny-Test-Model-GGUF");
        full_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        PhaseGate model_check(full_fixture.bus, "check_model:");
        SyncService full_attach = full_fixture.service();
        auto release_on_exit = model_check.auto_release();

        // 1. Enqueue targeted sync
        full_attach.enqueue({"Tiny-Test-Model-GGUF"});
        check("the worker parked inside a model check", model_check.wait_until_reached());

        // 2. While targeted sync is active, enqueue an empty sync (full sync)
        auto enq_empty = full_attach.enqueue({});
        check("empty sync enqueue during active sync returns already_running",
              enq_empty.already_running == true);

        model_check.release();
        full_attach.wait_until_idle();

        // 3. Verify that full check was triggered and completed
        check("full check was executed following empty sync enqueue",
              model_check.count("full_check_done") == 1);
    }

    // Test 17: Waiting for a queued sync returns that generation's snapshot
    {
        SyncFixture snap_fixture;
        snap_fixture.check_fn = [](const std::vector<std::string>& t) {
            UpdateScanResult r;
            for (const auto& m : t) {
                r.failed_models[m] = "Remote registry error";
            }
            return r;
        };
        SyncService exec_snap = snap_fixture.service();

        auto enq = exec_snap.enqueue({"non_existent_model_test_xyz"});
        json res = exec_snap.wait_for(enq.sync_id);
        check("a completed sync reports a terminal status", res.value("status", "") != "in_progress");
        check("the completed snapshot carries its own generation id",
              res.value("sync_id", 0) == enq.sync_id);
    }

    // Test 18: attach_if_running flag preserves targeted sync and prevents unexpected full sync expansion
    {
        SyncFixture attach_fixture;
        attach_fixture.downloaded("Tiny-Test-Model-GGUF");
        attach_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        PhaseGate model_check(attach_fixture.bus, "check_model:");
        SyncService attach_only = attach_fixture.service();
        auto release_on_exit = model_check.auto_release();

        // 1. Start targeted sync
        attach_only.enqueue({"Tiny-Test-Model-GGUF"});
        check("the worker parked inside a model check", model_check.wait_until_reached());

        // 2. Attach only with empty targets (e.g. update-models --wait)
        auto enq_attach = attach_only.enqueue({}, /*attach_if_running=*/true);
        check("attach_if_running returns already_running true", enq_attach.already_running == true);

        model_check.release();
        attach_only.wait_until_idle();

        // 3. Full check should NOT have run
        check("attach_if_running does not expand targeted sync to full sync",
              model_check.count("full_check_done") == 0);
    }

    // Test 19: Unknown or evicted sync_id queries return not_found status
    {
        SyncFixture empty_fixture;
        SyncService not_found = empty_fixture.service();
        json unk = not_found.status(999999);
        check("unknown sync_id returns not_found status", unk.value("status", "") == "not_found");
        check("unknown sync_id returns matching sync_id in response",
              unk.value("sync_id", 0) == 999999);
    }

    // Test 20: Attached full check authoritatively updates previously checked up-to-date model
    {
        SyncFixture fresh_fixture;
        fresh_fixture.downloaded("Tiny-Test-Model-GGUF");
        std::atomic<bool> trigger_update{false};
        std::mutex downloaded_m;
        std::vector<std::string> downloaded_models;

        fresh_fixture.check_fn = [&](const std::vector<std::string>&) {
            UpdateScanResult r;
            if (trigger_update.load()) {
                r.updated_models = {"Tiny-Test-Model-GGUF"};
            } else {
                r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            }
            return r;
        };
        fresh_fixture.download_fn = [&](const std::string& m, const json&, bool,
                                        lemon::DownloadProgressCallback) {
            std::lock_guard<std::mutex> lock(downloaded_m);
            downloaded_models.push_back(m);
        };
        PhaseGate target_check(fresh_fixture.bus, "target_check_done:");
        SyncService fresh = fresh_fixture.service();
        auto release_on_exit = target_check.auto_release();

        // 1. Start targeted sync where model is initially up to date
        fresh.enqueue({"Tiny-Test-Model-GGUF"});
        check("the worker parked after the targeted check", target_check.wait_until_reached());

        // Verify that initial targeted check recorded the model as up to date
        json initial_snap = fresh.status();
        check("initial targeted check records model up to date",
              initial_snap.contains("models_up_to_date") &&
                  contains(initial_snap["models_up_to_date"], "Tiny-Test-Model-GGUF"));

        // 2. Model receives upstream update; full sync is enqueued
        trigger_update.store(true);
        fresh.enqueue({});

        target_check.release();
        fresh.wait_until_idle();

        std::lock_guard<std::mutex> lock(downloaded_m);
        check("fresh full check authoritatively schedules model update",
              contains(downloaded_models, "Tiny-Test-Model-GGUF"));
    }

    // A service must be able to do work the instant it exists. The worker thread
    // starts during construction and members initialize in declaration order, so
    // a member the worker reads that is declared after the thread reads
    // indeterminate memory -- and a worker that thereby believed it had already
    // been told to stop exited at once, leaving the generation running forever.
    {
        SyncFixture boot_fixture;
        boot_fixture.downloaded("Tiny-Test-Model-GGUF");
        boot_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };

        bool every_service_ran = true;
        int stuck_attempt = -1;
        for (int attempt = 0; attempt < 200 && every_service_ran; ++attempt) {
            SyncService boot = boot_fixture.service();
            const auto enq = boot.enqueue({});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool terminal = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const std::string st = boot.status(enq.sync_id).value("status", "");
                if (st != "in_progress" && st != "not_found") {
                    terminal = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!terminal) {
                every_service_ran = false;
                stuck_attempt = attempt;
            }
        }
        check("every freshly constructed service runs its first generation", every_service_ran);
        if (!every_service_ran) std::printf("       stuck on attempt %d\n", stuck_attempt);
    }

    // Test 21: Teardown during a sync joins the worker before the store dies
    {
        SyncFixture teardown_fixture;
        teardown_fixture.downloaded("Tiny-Test-Model-GGUF");
        teardown_fixture.check_fn = [](const std::vector<std::string>&) {
            UpdateScanResult r;
            r.updated_models = {"Tiny-Test-Model-GGUF"};
            return r;
        };
        std::atomic<int> downloads_started{0};
        std::atomic<int> downloads_finished{0};
        std::atomic<bool> saw_cancel{false};

        // Stands in for a download: it keeps reporting progress until the sync
        // tells it to stop, which is the only way cancellation reaches a worker
        // already transferring bytes.
        teardown_fixture.download_fn = [&](const std::string&, const json&, bool,
                                           lemon::DownloadProgressCallback progress) {
            ++downloads_started;
            for (int i = 0; i < 2000; ++i) {
                if (!progress(lemon::DownloadProgress{})) {
                    saw_cancel = true;
                    // The cleanup a real transfer does after aborting: work
                    // still in flight, so it must not outlive the queue.
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    ++downloads_finished;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };

        auto service = teardown_fixture.make_service();
        service->enqueue({});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (downloads_started.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check("the sync worker runs the download", downloads_started.load() > 0);

        // Cancellation has to reach a worker already transferring bytes, not
        // just one that has not started.
        service->cancel();
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!saw_cancel.load() && std::chrono::steady_clock::now() < cancel_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check("cancellation reaches the worker mid-transfer", saw_cancel.load());

        const auto cancel_started = std::chrono::steady_clock::now();
        service.reset();
        const auto teardown_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - cancel_started)
                .count();
        check("teardown joins the worker promptly", teardown_ms < 5000);
        check("nothing the worker does outlives the queue", downloads_finished.load() == 1);
    }

    lemon::utils::set_cache_dir("");
    lemon::utils::set_config_dir("");
    std::filesystem::remove_all(temp_cache, ec);

    if (failures == 0) {
        std::cout << "All Model Sync & Auto-Update tests passed successfully!" << std::endl;
        return 0;
    } else {
        std::cout << failures << " test(s) failed." << std::endl;
        return 1;
    }
}
