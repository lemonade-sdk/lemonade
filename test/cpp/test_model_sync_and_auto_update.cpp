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

    if (failures == 0) {
        std::cout << "All Model Sync & Auto-Update tests passed successfully!" << std::endl;
        return 0;
    } else {
        std::cout << failures << " test(s) failed." << std::endl;
        return 1;
    }
}
