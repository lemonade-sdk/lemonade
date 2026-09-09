// Contract and unit tests for Model Sync and Auto-Update features.
// Tests:
// - RuntimeConfig auto_update_models setting and validation.
// - ModelInfo auto_update per-model override precedence.
// - ModelManager::should_auto_update resolution hierarchy.

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using lemon::json;

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

int main() {
    std::cout << "=== Model Sync & Auto-Update Unit Tests ===" << std::endl;

    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temp_cache =
        std::filesystem::temp_directory_path() / ("lemonade_sync_test_" + std::to_string(tick));
    std::error_code ec;
    std::filesystem::create_directories(temp_cache, ec);
    lemon::utils::set_cache_dir(temp_cache.string());
    lemon::utils::set_config_dir(temp_cache.string());

    const auto dummy_model_dir =
        temp_cache / "huggingface" / "hub" / "models--unsloth--gemma-3-270m-it-GGUF";
    std::filesystem::create_directories(dummy_model_dir / "refs", ec);
    std::filesystem::create_directories(dummy_model_dir / "snapshots" / "main", ec);
    {
        std::ofstream refs_file((dummy_model_dir / "refs" / "main").string());
        refs_file << "main\n";
    }
    {
        std::ofstream dummy_file((dummy_model_dir / "snapshots" / "main" / "gemma-3-270m-it-UD-IQ2_M.gguf").string(), std::ios::binary);
        dummy_file << "GGUF_dummy_content";
    }

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

    lemon::ModelManager mm;

    // Test 3: ModelManager::should_auto_update precedence
    // Case A: info.auto_update is std::nullopt -> uses global RuntimeConfig (false)
    lemon::ModelInfo info1;
    info1.model_name = "test-model-1";
    info1.auto_update = std::nullopt;

    bool should1 = mm.should_auto_update(info1);
    check("should_auto_update fallback to global (false)", should1 == false);

    // When global auto_update_models is true
    gcfg->set(json{{"auto_update_models", true}});
    bool should2 = mm.should_auto_update(info1);
    check("should_auto_update fallback to global (true)", should2 == true);

    // Case B: Explicit per-model override auto_update = true overrides global false
    gcfg->set(json{{"auto_update_models", false}});
    lemon::ModelInfo info2;
    info2.model_name = "test-model-2";
    info2.auto_update = true;
    bool should3 = mm.should_auto_update(info2);
    check("per-model auto_update=true overrides global false", should3 == true);

    // Case C: Explicit per-model override auto_update = false overrides global true
    gcfg->set(json{{"auto_update_models", true}});
    lemon::ModelInfo info3;
    info3.model_name = "test-model-3";
    info3.auto_update = false;
    bool should4 = mm.should_auto_update(info3);
    check("per-model auto_update=false overrides global true", should4 == false);

    // Test 4: ModelManager::get_sync_status and dry-run sync query
    mm.set_update_check_override_for_test([](const auto&) {
        lemon::ModelManager::UpdateCheckResult r;
        r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
        return r;
    });
    json status = mm.get_sync_status();
    check("get_sync_status returns idle status", status.value("status", "") == "idle");
    check("get_sync_status already_in_progress is false", status.value("already_in_progress", true) == false);

    json dry_run_res = mm.sync_models({}, /*dry_run=*/true);
    check("sync_models dry_run returns status", dry_run_res.contains("status") && dry_run_res.contains("checked_count"));

    // Test 5: RecipeOptions auto_update preservation
    json ro_json = {{"auto_update", true}};
    lemon::RecipeOptions ro("llamacpp", ro_json);
    check("RecipeOptions auto_update parsed successfully", ro.to_json().value("auto_update", false) == true);

    json ro_json_null = {{"auto_update", nullptr}};
    lemon::RecipeOptions ro_null("llamacpp", ro_json_null);
    check("RecipeOptions auto_update nullptr parsed successfully", ro_null.to_json().contains("auto_update") == false);

    // Test 6: ModelSyncState outcome structure in get_sync_status
    json status_fields = mm.get_sync_status();
    check("get_sync_status contains checked_count", status_fields.contains("checked_count"));
    check("get_sync_status contains updated_count", status_fields.contains("updated_count"));
    check("get_sync_status contains models_updated", status_fields.contains("models_updated"));
    check("get_sync_status contains models_up_to_date", status_fields.contains("models_up_to_date"));
    check("get_sync_status contains status field", status_fields.contains("status"));
    check("get_sync_status contains terminal_error field", status_fields.contains("terminal_error"));

    // Test 8: Single-pass sync execution
    lemon::ModelManager mm3;
    mm3.set_update_check_override_for_test([](const auto&) {
        lemon::ModelManager::UpdateCheckResult r;
        r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
        return r;
    });
    mm3.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});
    mm3.enqueue_sync({});
    json exec_res = mm3.execute_sync();
    std::string s_status = exec_res.value("status", "");
    check("execute_sync single pass returns valid status", s_status == "idle" || s_status == "success" || s_status == "failed");

    // Test 10: UpdateCheckResult registry failure handling
    lemon::ModelManager mm_fail;
    mm_fail.set_update_check_override_for_test([](const auto& t) {
        lemon::ModelManager::UpdateCheckResult r;
        for (const auto& m : t) {
            r.failed_models[m] = "Repository not found";
        }
        return r;
    });
    json dry_fail = mm_fail.sync_models({"non_existent_model_test_xyz"}, /*dry_run=*/true);
    check("registry failure populates failed_models", dry_fail.contains("failed_models") && dry_fail["failed_models"].contains("non_existent_model_test_xyz"));
    check("registry failure sets status to failed", dry_fail.value("status", "") == "failed");
    check("registry failure does not increment checked_count", dry_fail.value("checked_count", 0) == 0);

    // Test 11: Overlapping request target deduplication
    lemon::ModelManager mm_dedup;
    mm_dedup.set_update_check_override_for_test([](const auto& t) {
        lemon::ModelManager::UpdateCheckResult r;
        for (const auto& m : t) {
            r.failed_models[m] = "Repository not found";
        }
        return r;
    });
    mm_dedup.enqueue_sync({"non_existent_model_test_xyz"});
    auto second_enqueue = mm_dedup.enqueue_sync({"non_existent_model_test_xyz"});
    check("overlapping enqueue detects existing sync running", second_enqueue.already_running == true);

    // Test 12: Deterministic overlapping full check and targeted enqueue deduplication
    {
        lemon::ModelManager mm_overlap;
        mm_overlap.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        });
        mm_overlap.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        std::mutex pause_m;
        std::condition_variable pause_cv;
        bool full_check_done_hit = false;
        bool allow_resume = false;
        std::atomic<int> hook_invocations{0};
        std::atomic<int> target_check_invocations{0};

        mm_overlap.set_sync_phase_callback([&](const std::string& phase) {
            if (phase == "full_check_done") {
                hook_invocations++;
                std::unique_lock<std::mutex> lock(pause_m);
                full_check_done_hit = true;
                pause_cv.notify_all();
                pause_cv.wait(lock, [&]() { return allow_resume; });
            } else if (phase == "registry_check:Tiny-Test-Model-GGUF") {
                target_check_invocations++;
            }
        });

        mm_overlap.enqueue_sync({});
        std::thread worker([&]() {
            mm_overlap.execute_sync();
        });

        {
            std::unique_lock<std::mutex> lock(pause_m);
            pause_cv.wait(lock, [&]() { return full_check_done_hit; });
        }

        auto enqueued_during_run = mm_overlap.enqueue_sync({"Tiny-Test-Model-GGUF"});
        check("enqueue during active sync returns true", enqueued_during_run.already_running == true);

        {
            std::lock_guard<std::mutex> lock(pause_m);
            allow_resume = true;
            pause_cv.notify_all();
        }

        worker.join();

        json overlap_exec = mm_overlap.get_sync_status();
        std::string o_status = overlap_exec.value("status", "");
        check("overlap execution status is valid", o_status == "idle" || o_status == "success" || o_status == "failed");
        check("full check callback triggered exactly once", hook_invocations.load() == 1);
        check("target check triggered exactly once during sync", target_check_invocations.load() == 1);

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

    // Test 13: Synchronous sync_models joins active sync and waits for completion
    {
        lemon::ModelManager mm_sync_join;
        mm_sync_join.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        });
        mm_sync_join.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        std::mutex pause_m;
        std::condition_variable pause_c;
        bool hook_hit = false;
        bool proceed = false;

        mm_sync_join.set_sync_phase_callback([&](const std::string& phase) {
            if (phase == "full_check_done") {
                std::unique_lock<std::mutex> lock(pause_m);
                hook_hit = true;
                pause_c.notify_all();
                pause_c.wait(lock, [&]() { return proceed; });
            }
        });

        mm_sync_join.enqueue_sync({});
        std::thread worker([&]() {
            mm_sync_join.execute_sync();
        });

        {
            std::unique_lock<std::mutex> lock(pause_m);
            pause_c.wait(lock, [&]() { return hook_hit; });
        }

        std::atomic<bool> join_finished{false};
        json join_result;
        std::thread joiner([&]() {
            join_result = mm_sync_join.sync_models({"Tiny-Test-Model-GGUF"}, /*dry_run=*/false);
            join_finished = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check("synchronous joiner blocks while sync is running", join_finished.load() == false);

        {
            std::lock_guard<std::mutex> lock(pause_m);
            proceed = true;
            pause_c.notify_all();
        }

        worker.join();
        joiner.join();

        check("synchronous joiner finishes after sync completes", join_finished.load() == true);
        std::string j_status = join_result.value("status", "");
        check("synchronous joiner receives completed status not in_progress", j_status != "in_progress");
    }

    // Test 14: Generation ID tracking and historical status lookup
    {
        lemon::ModelManager mm_gen;
        mm_gen.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        });
        mm_gen.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        auto enq1 = mm_gen.enqueue_sync({});
        check("first sync enqueue allocates sync_id > 0", enq1.sync_id > 0);
        check("first sync enqueue already_running is false", enq1.already_running == false);

        auto enq2 = mm_gen.enqueue_sync({"non_existent_model_test_xyz"});
        check("overlapping sync enqueue returns same sync_id", enq2.sync_id == enq1.sync_id);
        check("overlapping sync enqueue already_running is true", enq2.already_running == true);

        json sync1_res = mm_gen.execute_sync();
        uint64_t completed_gen = mm_gen.get_sync_status().value("completed_sync_id", 0);
        check("completed_sync_id matches first sync_id", completed_gen == enq1.sync_id);

        json historical_status = mm_gen.get_sync_status(enq1.sync_id);
        check("historical status lookup returns completed generation result",
              historical_status.value("status", "") != "in_progress" &&
              historical_status.value("sync_id", 0) == enq1.sync_id);

        // Start a second sync job
        auto enq3 = mm_gen.enqueue_sync({});
        check("second sync enqueue allocates new sync_id", enq3.sync_id > enq1.sync_id);
        json hist_old = mm_gen.get_sync_status(enq1.sync_id);
        check("historical status for previous sync_id is preserved while new job is running",
              hist_old.value("status", "") != "in_progress" &&
              hist_old.value("sync_id", 0) == enq1.sync_id);
        mm_gen.execute_sync();
    }

    // Test 15: Stale update_available cache flag is ignored when fresh check fails
    {
        lemon::ModelManager mm_stale;
        mm_stale.set_model_update_available_for_test("Tiny-Test-Model-GGUF", true);
        lemon::ModelInfo cached = mm_stale.get_model_info("Tiny-Test-Model-GGUF");
        check("cached update_available flag is true before fresh check", cached.update_available == true);

        mm_stale.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            for (const auto& m : t) {
                r.failed_models[m] = "Remote registry check failed";
            }
            return r;
        });

        // Targeted sync on a model whose remote check fails
        auto enq_stale = mm_stale.enqueue_sync({"non_existent_model_test_xyz"});
        json stale_exec = mm_stale.execute_sync();
        check("failed fresh check populates failed_models",
              stale_exec.contains("failed_models") && stale_exec["failed_models"].contains("non_existent_model_test_xyz"));
        check("failed fresh check does not schedule update",
              stale_exec.contains("models_updated") && stale_exec["models_updated"].empty());
    }

    // Test 16: Empty full-sync request arriving during running targeted sync triggers full check
    {
        lemon::ModelManager mm_full_attach;
        std::mutex pause_m;
        std::condition_variable pause_cv;
        bool target_check_hit = false;
        bool allow_resume = false;
        std::atomic<int> full_check_count{0};

        mm_full_attach.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        });
        mm_full_attach.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        mm_full_attach.set_sync_phase_callback([&](const std::string& phase) {
            if (phase.rfind("check_model:", 0) == 0) {
                std::unique_lock<std::mutex> lock(pause_m);
                target_check_hit = true;
                pause_cv.notify_all();
                pause_cv.wait(lock, [&]() { return allow_resume; });
            } else if (phase == "full_check_done") {
                full_check_count++;
            }
        });

        // 1. Enqueue targeted sync
        mm_full_attach.enqueue_sync({"Tiny-Test-Model-GGUF"});
        std::thread worker([&]() {
            mm_full_attach.execute_sync();
        });

        {
            std::unique_lock<std::mutex> lock(pause_m);
            pause_cv.wait(lock, [&]() { return target_check_hit; });
        }

        // 2. While targeted sync is active, enqueue an empty sync (full sync)
        auto enq_empty = mm_full_attach.enqueue_sync({});
        check("empty sync enqueue during active sync returns already_running", enq_empty.already_running == true);

        {
            std::lock_guard<std::mutex> lock(pause_m);
            allow_resume = true;
            pause_cv.notify_all();
        }

        worker.join();

        // 3. Verify that full check was triggered and completed
        check("full check was executed following empty sync enqueue", full_check_count.load() == 1);
    }

    // Test 17: Synchronous execute_sync returns correct generation snapshot directly
    {
        lemon::ModelManager mm_exec_snap;
        mm_exec_snap.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            for (const auto& m : t) {
                r.failed_models[m] = "Remote registry error";
            }
            return r;
        });
        mm_exec_snap.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        auto enq = mm_exec_snap.enqueue_sync({"non_existent_model_test_xyz"});
        json res = mm_exec_snap.execute_sync();
        check("execute_sync returns completed snapshot not in_progress", res.value("status", "") != "in_progress");
        check("execute_sync snapshot sync_id matches generation", res.value("sync_id", 0) == enq.sync_id);
    }

    // Test 18: attach_if_running flag preserves targeted sync and prevents unexpected full sync expansion
    {
        lemon::ModelManager mm_attach_only;
        std::mutex pause_m;
        std::condition_variable pause_cv;
        bool target_check_hit = false;
        bool allow_resume = false;
        std::atomic<int> full_check_count{0};

        mm_attach_only.set_update_check_override_for_test([](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            return r;
        });
        mm_attach_only.set_download_model_override_for_test([](const auto&, const auto&, bool, auto) {});

        mm_attach_only.set_sync_phase_callback([&](const std::string& phase) {
            if (phase.rfind("check_model:", 0) == 0) {
                std::unique_lock<std::mutex> lock(pause_m);
                target_check_hit = true;
                pause_cv.notify_all();
                pause_cv.wait(lock, [&]() { return allow_resume; });
            } else if (phase == "full_check_done") {
                full_check_count++;
            }
        });

        // 1. Start targeted sync
        mm_attach_only.enqueue_sync({"Tiny-Test-Model-GGUF"});
        std::thread worker([&]() {
            mm_attach_only.execute_sync();
        });

        {
            std::unique_lock<std::mutex> lock(pause_m);
            pause_cv.wait(lock, [&]() { return target_check_hit; });
        }

        // 2. Attach only with empty targets (e.g. update-models --wait)
        auto enq_attach = mm_attach_only.enqueue_sync({}, /*attach_if_running=*/true);
        check("attach_if_running returns already_running true", enq_attach.already_running == true);

        {
            std::lock_guard<std::mutex> lock(pause_m);
            allow_resume = true;
            pause_cv.notify_all();
        }

        worker.join();

        // 3. Full check should NOT have run
        check("attach_if_running does not expand targeted sync to full sync", full_check_count.load() == 0);
    }

    // Test 19: Unknown or evicted sync_id queries return not_found status
    {
        lemon::ModelManager mm_not_found;
        json unk = mm_not_found.get_sync_status(999999);
        check("unknown sync_id returns not_found status", unk.value("status", "") == "not_found");
        check("unknown sync_id returns matching sync_id in response", unk.value("sync_id", 0) == 999999);
    }

    // Test 20: Attached full check authoritatively updates previously checked up-to-date model
    {
        lemon::ModelManager mm_fresh;
        std::atomic<bool> trigger_update{false};
        std::vector<std::string> downloaded_models;

        mm_fresh.set_update_check_override_for_test([&](const auto& t) {
            lemon::ModelManager::UpdateCheckResult r;
            if (trigger_update.load()) {
                r.updated_models = {"Tiny-Test-Model-GGUF"};
            } else {
                r.up_to_date_models = {"Tiny-Test-Model-GGUF"};
            }
            return r;
        });
        mm_fresh.set_download_model_override_for_test([&](const auto& m, const auto&, bool, auto) {
            downloaded_models.push_back(m);
        });

        std::mutex pause_m;
        std::condition_variable pause_cv;
        bool target_check_hit = false;
        bool allow_resume = false;

        mm_fresh.set_sync_phase_callback([&](const std::string& phase) {
            if (phase.rfind("target_check_done:", 0) == 0) {
                std::unique_lock<std::mutex> lock(pause_m);
                target_check_hit = true;
                pause_cv.notify_all();
                pause_cv.wait(lock, [&]() { return allow_resume; });
            }
        });

        // 1. Start targeted sync where model is initially up to date
        mm_fresh.enqueue_sync({"Tiny-Test-Model-GGUF"});
        std::thread worker([&]() {
            mm_fresh.execute_sync();
        });

        {
            std::unique_lock<std::mutex> lock(pause_m);
            pause_cv.wait(lock, [&]() { return target_check_hit; });
        }

        // Verify that initial targeted check recorded the model as up to date
        json initial_snap = mm_fresh.get_sync_status();
        check("initial targeted check records model up to date",
              initial_snap.contains("models_up_to_date") &&
              std::find(initial_snap["models_up_to_date"].begin(), initial_snap["models_up_to_date"].end(), "Tiny-Test-Model-GGUF") != initial_snap["models_up_to_date"].end());

        // 2. Model receives upstream update; full sync is enqueued
        trigger_update.store(true);
        mm_fresh.enqueue_sync({});

        {
            std::lock_guard<std::mutex> lock(pause_m);
            allow_resume = true;
            pause_cv.notify_all();
        }

        worker.join();

        json fresh_status = mm_fresh.get_sync_status();
        check("fresh full check authoritatively schedules model update",
              std::find(downloaded_models.begin(), downloaded_models.end(), "Tiny-Test-Model-GGUF") != downloaded_models.end());
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
