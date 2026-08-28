// Unit tests for the LLM residency pool floor: config plumbing, the
// ModelType-aware residency_limit() gate, and Router's generation-guarded
// reconcile applying (or, with autosize off, not applying) that floor at
// admission time. Companion to test_routing_helper_reconcile.cpp, which
// covers the sibling RoutingHelper reconciliation this mirrors.

#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/wrapped_server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;

namespace lemon {

// Standard-class, LLM-type stub — never spawns a subprocess. Distinct from
// test_routing_helper_reconcile.cpp's StubWrappedServer, which hardcodes
// ModelType::CLASSIFICATION for its helper-pool tests; the floor only applies
// to (Standard, LLM), so this one needs the right type to exercise it.
class StubLlmServer : public WrappedServer {
public:
    explicit StubLlmServer(const std::string& model_name)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(model_name, "", ModelType::LLM, DEVICE_CPU, RecipeOptions());
        set_residency_class(ResidencyClass::Standard);
        set_state(ModelState::READY);
    }

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override { unloaded_.store(true); }
    bool is_backend_alive() const override { return alive_.load(); }
    bool was_unloaded() const { return unloaded_.load(); }

private:
    std::atomic<bool> alive_{true};
    std::atomic<bool> unloaded_{false};
};

struct LlmPoolFloorTestHook {
    static StubLlmServer* add_server(Router& r, std::unique_ptr<StubLlmServer> s) {
        StubLlmServer* raw = s.get();
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.install_reclaim_notifier(raw);
        r.loaded_servers_.push_back(std::move(s));
        return raw;
    }

    static void reconcile_floor(Router& r, int floor) {
        static std::atomic<uint64_t> generation{0};
        r.reconcile_llm_candidate_floor(floor, ++generation);
    }

    // Same call load_model makes when admitting a new LLM: check capacity for
    // one more slot and evict the LRU if the pool (now floored) is full.
    static void ensure_capacity(Router& r, const std::string& incoming_model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.ensure_residency_capacity(ModelType::LLM, ResidencyClass::Standard, incoming_model_name);
    }

    static bool resident(Router& r, const std::string& model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        for (const auto& s : r.loaded_servers_) {
            if (s->get_model_name() == model_name) return true;
        }
        return false;
    }

    static int applied_floor(Router& r) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        return r.llm_candidate_floor_;
    }

    static int resident_count(Router& r, const std::vector<std::string>& names) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        int count = 0;
        for (const auto& name : names) {
            for (const auto& s : r.loaded_servers_) {
                if (s->get_model_name() == name) {
                    ++count;
                    break;
                }
            }
        }
        return count;
    }
};

}  // namespace lemon

using lemon::LlmPoolFloorTestHook;
using lemon::ModelState;
using lemon::ModelType;
using lemon::residency_limit;
using lemon::ResidencyClass;
using lemon::Router;
using lemon::RuntimeConfig;
using lemon::StubLlmServer;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static void test_config_default_is_true() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 1;
    cfg["log_level"] = "error";
    RuntimeConfig config(cfg);
    check("llm_pool_autosize defaults to true when unset", config.llm_pool_autosize());
}

static void test_config_can_be_disabled() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 1;
    cfg["log_level"] = "error";
    cfg["llm_pool_autosize"] = false;
    RuntimeConfig config(cfg);
    check("llm_pool_autosize honors an explicit false", !config.llm_pool_autosize());
}

// Each test below builds its own RuntimeConfig + Router rather than sharing
// one across tests: ensure_residency_capacity's eviction picks the pool's
// oldest resident, so a shared Router would let one test's leftover servers
// silently become the "victim" a later test meant to control. RuntimeConfig
// itself isn't copyable (it owns a shared_mutex), so this builds the json and
// lets each test construct its own RuntimeConfig directly rather than passing
// a RuntimeConfig around by value.
static json make_config_json(int max_loaded_models, bool autosize) {
    json cfg = json::object();
    cfg["max_loaded_models"] = max_loaded_models;
    cfg["log_level"] = "error";
    cfg["llm_pool_autosize"] = autosize;
    return cfg;
}

static void test_floor_raises_llm_capacity() {
    RuntimeConfig config(make_config_json(1, true));
    Router router(&config, nullptr, nullptr);
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("floor.a"));
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("floor.b"));
    LlmPoolFloorTestHook::reconcile_floor(router, 3);

    // max_loaded_models is 1; without the floor this would evict floor.a to
    // make room for a third LLM.
    LlmPoolFloorTestHook::ensure_capacity(router, "floor.c");

    check("floor keeps both existing LLM candidates resident when pool is under floor",
          LlmPoolFloorTestHook::resident(router, "floor.a") &&
              LlmPoolFloorTestHook::resident(router, "floor.b"));
}

static void test_floor_still_evicts_past_capacity() {
    RuntimeConfig config(make_config_json(1, true));
    Router router(&config, nullptr, nullptr);
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("evict.old"));
    LlmPoolFloorTestHook::reconcile_floor(router, 1);

    // Pool is already at the floor (1); a second LLM must still evict the LRU.
    LlmPoolFloorTestHook::ensure_capacity(router, "evict.new");

    check("floor doesn't stop eviction once the pool is actually full",
          !LlmPoolFloorTestHook::resident(router, "evict.old"));
}

static void test_stale_generation_ignored() {
    RuntimeConfig config(make_config_json(1, true));
    Router router(&config, nullptr, nullptr);
    router.reconcile_llm_candidate_floor(2, 100);
    // An older generation arriving after a newer one must not undo it. Check
    // the stored value directly rather than through eviction side effects —
    // at floor=2 with 2 residents, adding a 3rd evicts one either way (floor
    // reverted to 0 or not), so that path can't distinguish the two cases.
    router.reconcile_llm_candidate_floor(0, 50);

    check("an out-of-order (older) reconcile is ignored",
          LlmPoolFloorTestHook::applied_floor(router) == 2);
}

static void test_unlimited_pool_ignores_floor() {
    // max_loaded_models == -1 means unlimited; a floor has nothing to raise.
    check("residency_limit leaves an unlimited pool unlimited",
          residency_limit(ResidencyClass::Standard, ModelType::LLM, -1, 100) == -1);
}

static void test_autosize_off_clamps_applied_floor() {
    RuntimeConfig config(make_config_json(1, false));
    Router router(&config, nullptr, nullptr);
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("off.old"));
    LlmPoolFloorTestHook::reconcile_floor(router, 5);

    // autosize is off — the floor is tracked but must not be applied, so
    // capacity reverts to max_loaded_models (1).
    LlmPoolFloorTestHook::ensure_capacity(router, "off.new");

    check("disabled autosize restores today's max_loaded_models behavior",
          !LlmPoolFloorTestHook::resident(router, "off.old"));
}

static void test_reconcile_converges_pool_down_when_floor_drops() {
    RuntimeConfig config(make_config_json(1, true));
    Router router(&config, nullptr, nullptr);
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("drop.a"));
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("drop.b"));
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("drop.c"));
    LlmPoolFloorTestHook::reconcile_floor(router, 3);
    const int before = LlmPoolFloorTestHook::resident_count(router, {"drop.a", "drop.b", "drop.c"});

    // A policy edit that drops candidates lowers the floor without any
    // admission ever happening; the pool must converge on its own rather
    // than waiting for future loads to evict one at a time.
    LlmPoolFloorTestHook::reconcile_floor(router, 1);

    check("reconcile converges an already-populated pool down when the floor drops",
          before == 3 &&
              LlmPoolFloorTestHook::resident_count(
                  router, {"drop.a", "drop.b", "drop.c"}) == 1);
}

static void test_enforce_llm_pool_capacity_reclaims_after_live_config_change() {
    RuntimeConfig config(make_config_json(1, true));
    Router router(&config, nullptr, nullptr);
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("cfg.a"));
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("cfg.b"));
    LlmPoolFloorTestHook::add_server(router, std::make_unique<StubLlmServer>("cfg.c"));
    LlmPoolFloorTestHook::reconcile_floor(router, 3);

    // Mirrors Server::apply_config_side_effects: a live /internal/set toggle,
    // not a policy change, so nothing but an explicit enforce call reclaims it.
    config.set({{"llm_pool_autosize", false}});
    router.enforce_llm_pool_capacity();

    check("disabling autosize live reclaims an already-populated pool immediately",
          LlmPoolFloorTestHook::resident_count(router, {"cfg.a", "cfg.b", "cfg.c"}) == 1);
}

int main() {
    // A background eviction-engine thread runs inside every Router below and
    // reads RuntimeConfig::global() for unrelated auto_evict bookkeeping; it
    // just needs a live, non-null target for the process's lifetime — it
    // never has to be the same RuntimeConfig a given test's Router was built
    // with, since Router itself reads its own config through the pointer
    // passed to its constructor.
    RuntimeConfig global_config(make_config_json(1, true));
    RuntimeConfig::set_global(&global_config);

    test_config_default_is_true();
    test_config_can_be_disabled();
    test_unlimited_pool_ignores_floor();
    test_floor_raises_llm_capacity();
    test_floor_still_evicts_past_capacity();
    test_stale_generation_ignored();
    test_autosize_off_clamps_applied_floor();
    test_reconcile_converges_pool_down_when_floor_drops();
    test_enforce_llm_pool_capacity_reclaims_after_live_config_change();

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll LLM candidate floor tests passed.\n");
    } else {
        std::printf("\n%d LLM candidate floor test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
