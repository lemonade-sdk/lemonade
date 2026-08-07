// Contract and unit tests for Model Sync and Auto-Update features.
// Tests:
// - RuntimeConfig auto_update_models setting and validation.
// - ModelInfo auto_update per-model override precedence.
// - ModelManager::should_auto_update resolution hierarchy.

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"

#include <cstdio>
#include <iostream>
#include <cassert>

using lemon::json;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

int main() {
    std::cout << "=== Model Sync & Auto-Update Unit Tests ===" << std::endl;

    // Test 1: RuntimeConfig default for auto_update_models is false
    json initial_config = json::object();
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
    mm3.enqueue_sync({});
    json exec_res = mm3.execute_sync();
    std::string s_status = exec_res.value("status", "");
    check("execute_sync single pass returns valid status", s_status == "idle" || s_status == "success" || s_status == "failed");


    // Test 10: UpdateCheckResult registry failure handling
    lemon::ModelManager mm_fail;
    json dry_fail = mm_fail.sync_models({"non_existent_model_test_xyz"}, /*dry_run=*/true);
    check("registry failure populates failed_models", dry_fail.contains("failed_models") && dry_fail["failed_models"].contains("non_existent_model_test_xyz"));
    check("registry failure sets status to failed", dry_fail.value("status", "") == "failed");
    check("registry failure does not increment checked_count", dry_fail.value("checked_count", 0) == 0);

    // Test 11: Overlapping request target deduplication
    lemon::ModelManager mm_dedup;
    mm_dedup.enqueue_sync({"non_existent_model_test_xyz"});
    bool second_enqueue = mm_dedup.enqueue_sync({"non_existent_model_test_xyz"});
    check("overlapping enqueue detects existing sync running", second_enqueue == true);

    // Test 12: Overlapping full check and targeted enqueue deduplication
    lemon::ModelManager mm_overlap;
    mm_overlap.enqueue_sync({});
    mm_overlap.enqueue_sync({"Tiny-Test-Model-GGUF"});
    json overlap_exec = mm_overlap.execute_sync();
    std::string o_status = overlap_exec.value("status", "");
    check("overlap execution status is valid", o_status == "idle" || o_status == "success" || o_status == "failed");

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
    check("overlapping target checked and queued at most once", target_count <= 1);



    if (failures == 0) {
        std::cout << "All Model Sync & Auto-Update tests passed successfully!" << std::endl;
        return 0;
    } else {
        std::cout << failures << " test(s) failed." << std::endl;
        return 1;
    }
}
