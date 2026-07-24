// Concurrency/residency test for routing-helper reconciliation.
//
// Exercises the durable-reconciliation core the router uses to reclaim routing
// helpers when a collection's policy changes: the stored needed-set, the
// non-blocking prune that skips busy/pinned helpers, and thread-safety of a
// policy update racing a helper going busy/idle.
//
// The full load_model interleaving (the load-completion validation guard) is an
// integration concern: it needs a real ModelManager (reads server_models.json),
// the compile-time backend registry, and a spawned subprocess, so it is not
// unit-testable here. That guard shares the exact predicate validated below
// (residency == RoutingHelper, not pinned, absent from needed_helper_models_),
// which this test covers via a StubWrappedServer injected through a friend hook.

#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/wrapped_server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;

namespace lemon {

// Minimal WrappedServer that never spawns a subprocess. Only load()/unload()
// are pure virtual; is_backend_alive() is overridden so an injected stub is
// treated as a live resident instead of a dead tombstone.
class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer(const std::string& model_name, ResidencyClass residency)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(model_name, "", ModelType::CLASSIFICATION, DEVICE_CPU,
                           RecipeOptions());
        set_residency_class(residency);
        set_state(ModelState::READY);
    }

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}

    void unload() override { unloaded_.store(true); }

    bool is_backend_alive() const override { return alive_.load(); }

    void set_alive(bool alive) { alive_.store(alive); }

    // Drive is_busy() the way the router's maintenance path does.
    void set_busy(bool busy) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        maintenance_in_progress_ = busy;
        state_cv_.notify_all();
    }

    bool was_unloaded() const { return unloaded_.load(); }

private:
    std::atomic<bool> alive_{true};
    std::atomic<bool> unloaded_{false};
};

// Friend seam declared in router.h. Gives the test direct access to the
// reconciliation internals without going through the ModelManager-backed
// load/reconcile entry points (which read JSON from the cache dir).
struct RoutingHelperTestHook {
    static StubWrappedServer* add_server(Router& r, std::unique_ptr<StubWrappedServer> s) {
        StubWrappedServer* raw = s.get();
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.loaded_servers_.push_back(std::move(s));
        return raw;
    }

    static void set_needed(Router& r, std::set<std::string> needed) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.needed_helper_models_ = std::move(needed);
    }

    static void prune(Router& r) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.prune_stale_routing_helpers_locked();
    }

    static bool has_helper(Router& r, const std::string& model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        for (const auto& s : r.loaded_servers_) {
            if (s->is_backend_alive() &&
                s->get_residency_class() == ResidencyClass::RoutingHelper &&
                s->get_model_name() == model_name) {
                return true;
            }
        }
        return false;
    }

    static bool has_any_model(Router& r, const std::string& model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        for (const auto& s : r.loaded_servers_) {
            if (s->get_model_name() == model_name) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace lemon

using lemon::ModelState;
using lemon::ResidencyClass;
using lemon::Router;
using lemon::RoutingHelperTestHook;
using lemon::RuntimeConfig;
using lemon::StubWrappedServer;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static std::unique_ptr<StubWrappedServer> make_helper(const std::string& name) {
    return std::make_unique<StubWrappedServer>(name, ResidencyClass::RoutingHelper);
}

static std::unique_ptr<StubWrappedServer> make_standard(const std::string& name) {
    return std::make_unique<StubWrappedServer>(name, ResidencyClass::Standard);
}

static void test_stale_idle_helper_evicted(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("stale.helper"));
    RoutingHelperTestHook::set_needed(router, {});
    RoutingHelperTestHook::prune(router);
    check("stale idle routing helper is evicted by prune",
          !RoutingHelperTestHook::has_helper(router, "stale.helper"));
}

static void test_needed_helper_survives(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("kept.helper"));
    RoutingHelperTestHook::set_needed(router, {"kept.helper"});
    RoutingHelperTestHook::prune(router);
    check("routing helper still referenced by a policy survives prune",
          RoutingHelperTestHook::has_helper(router, "kept.helper"));
}

static void test_pinned_stale_helper_survives(Router& router) {
    auto helper = make_helper("pinned.helper");
    helper->set_pinned(true);
    RoutingHelperTestHook::add_server(router, std::move(helper));
    RoutingHelperTestHook::set_needed(router, {});
    RoutingHelperTestHook::prune(router);
    check("user-pinned stale routing helper survives prune",
          RoutingHelperTestHook::has_helper(router, "pinned.helper"));
}

static void test_standard_model_untouched(Router& router) {
    RoutingHelperTestHook::add_server(router, make_standard("user.model"));
    RoutingHelperTestHook::set_needed(router, {});
    RoutingHelperTestHook::prune(router);
    check("standard (non-helper) model is never reclaimed by helper prune",
          RoutingHelperTestHook::has_any_model(router, "user.model"));
}

// Reviewer's busy-helper concern: a helper busy during the policy change is
// skipped (never blocks on an eviction timeout) and durably reclaimed on the
// next prune once it goes idle.
static void test_busy_helper_reclaimed_when_idle(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("busy.helper"));
    helper->set_busy(true);
    RoutingHelperTestHook::set_needed(router, {});

    RoutingHelperTestHook::prune(router);
    bool survived_while_busy = RoutingHelperTestHook::has_helper(router, "busy.helper");

    helper->set_busy(false);
    RoutingHelperTestHook::prune(router);
    bool reclaimed_when_idle = !RoutingHelperTestHook::has_helper(router, "busy.helper");

    check("busy stale routing helper survives prune, reclaimed once idle",
          survived_while_busy && reclaimed_when_idle);
}

// Policy update racing a helper toggling busy/idle plus concurrent prune passes.
// The needed-set always includes the helper during the concurrent phase so it is
// never evicted (avoiding a use-after-free on the raw pointer the busy-toggler
// holds); the assertion is that nothing deadlocks or crashes and the helper is
// still resident. A final single-threaded prune with an empty needed-set then
// confirms it is reclaimed once idle.
static void test_concurrent_policy_update(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("race.helper"));
    RoutingHelperTestHook::set_needed(router, {"race.helper"});

    constexpr int kIterations = 2000;
    std::atomic<bool> stop{false};

    std::thread busy_toggler([&] {
        for (int i = 0; i < kIterations; ++i) {
            helper->set_busy(i % 2 == 0);
        }
        helper->set_busy(false);
    });

    std::vector<std::thread> pruners;
    for (int t = 0; t < 3; ++t) {
        pruners.emplace_back([&] {
            while (!stop.load()) {
                RoutingHelperTestHook::prune(router);
            }
        });
    }

    std::thread policy_writer([&] {
        for (int i = 0; i < kIterations; ++i) {
            // Always keep race.helper needed so no pruner can evict it mid-race.
            RoutingHelperTestHook::set_needed(
                router, {"race.helper", "churn." + std::to_string(i % 8)});
        }
    });

    busy_toggler.join();
    policy_writer.join();
    stop.store(true);
    for (auto& p : pruners) {
        p.join();
    }

    bool survived_race = RoutingHelperTestHook::has_helper(router, "race.helper");

    helper->set_busy(false);
    RoutingHelperTestHook::set_needed(router, {});
    RoutingHelperTestHook::prune(router);
    bool reclaimed_after_race = !RoutingHelperTestHook::has_helper(router, "race.helper");

    check("concurrent policy update + busy toggling keeps needed helper resident",
          survived_race);
    check("helper reclaimed by prune after the race once no policy needs it",
          reclaimed_after_race);
}

int main() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 4;
    cfg["log_level"] = "error";

    RuntimeConfig config(cfg);
    RuntimeConfig::set_global(&config);

    // Each Router owns its own monitor/eviction threads; scope them so they are
    // torn down before the shared global config is cleared.
    {
        Router router(&config, nullptr, nullptr);
        test_stale_idle_helper_evicted(router);
        test_needed_helper_survives(router);
        test_pinned_stale_helper_survives(router);
        test_standard_model_untouched(router);
        test_busy_helper_reclaimed_when_idle(router);
        test_concurrent_policy_update(router);
    }

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll routing-helper reconcile tests passed.\n");
    } else {
        std::printf("\n%d routing-helper reconcile test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
