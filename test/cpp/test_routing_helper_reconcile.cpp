// Concurrency/residency test for routing-helper reconciliation.
//
// Exercises the durable-reconciliation core the router uses to reclaim routing
// helpers when a collection's policy changes: the published needed-set, the
// non-blocking prune that skips busy/pinned helpers, and thread-safety of a
// policy update racing a helper going busy/idle. Cases drive the production
// reconcile entry (apply_routing_helper_reconcile) rather than the prune in
// isolation, so a busy helper is reclaimed by a subsequent policy reconcile once
// idle — the real transition, not a hand-invoked second prune.
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
#include <chrono>
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
        r.install_reclaim_notifier(raw);
        r.loaded_servers_.push_back(std::move(s));
        return raw;
    }

    // Drive the production reconcile core directly. Names are pre-canonicalized
    // (the test never touches ModelManager::resolve_model_name, which reads the
    // cache dir), so this exercises the real publish-then-prune transition a
    // policy change triggers — not the prune in isolation. Each call carries a
    // strictly increasing generation, matching the production ordering guard.
    static void reconcile(Router& r, std::set<std::string> needed) {
        static std::atomic<uint64_t> generation{0};
        r.apply_routing_helper_reconcile(std::move(needed), ++generation);
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

    // Drive the private release-triggered reclaim directly (as the executor
    // worker would), so a test can observe it waiting for the residency slot.
    static void reclaim_now(Router& r, const std::string& model_name) {
        r.reclaim_stale_helper_if_idle(model_name);
    }

    // Drive the residency-transition core with an already-canonical name (the
    // test's Router has no ModelManager, so it must skip resolve_model_name).
    static bool ensure_residency(Router& r, const std::string& model_name,
                                 lemon::LoadPurpose load_purpose) {
        return r.ensure_loaded_model_residency_canonical(model_name, load_purpose);
    }

    static ResidencyClass residency_of(Router& r, const std::string& model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        for (const auto& s : r.loaded_servers_) {
            if (s->get_model_name() == model_name) {
                return s->get_residency_class();
            }
        }
        return ResidencyClass::Standard;
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
    RoutingHelperTestHook::reconcile(router, {});
    check("stale idle routing helper is evicted on policy change",
          !RoutingHelperTestHook::has_helper(router, "stale.helper"));
}

static void test_needed_helper_survives(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("kept.helper"));
    RoutingHelperTestHook::reconcile(router, {"kept.helper"});
    check("routing helper still referenced by a policy survives reconcile",
          RoutingHelperTestHook::has_helper(router, "kept.helper"));
}

static void test_pinned_stale_helper_survives(Router& router) {
    auto helper = make_helper("pinned.helper");
    helper->set_pinned(true);
    RoutingHelperTestHook::add_server(router, std::move(helper));
    RoutingHelperTestHook::reconcile(router, {});
    check("user-pinned stale routing helper survives reconcile",
          RoutingHelperTestHook::has_helper(router, "pinned.helper"));
}

static void test_standard_model_untouched(Router& router) {
    RoutingHelperTestHook::add_server(router, make_standard("user.model"));
    RoutingHelperTestHook::reconcile(router, {});
    check("standard (non-helper) model is never reclaimed by reconcile",
          RoutingHelperTestHook::has_any_model(router, "user.model"));
}

// Reviewer's busy-helper concern: a helper busy during the policy change is
// skipped (never blocks on an eviction timeout) and durably reclaimed by the
// next policy reconcile once it goes idle.
static void test_busy_helper_reclaimed_when_idle(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("busy.helper"));
    helper->set_busy(true);

    RoutingHelperTestHook::reconcile(router, {});
    bool survived_while_busy = RoutingHelperTestHook::has_helper(router, "busy.helper");

    // The request finishes, then a later policy reconcile reclaims the now-idle
    // helper (reloading the policy is the durable reclamation path).
    helper->set_busy(false);
    RoutingHelperTestHook::reconcile(router, {});
    bool reclaimed_when_idle = !RoutingHelperTestHook::has_helper(router, "busy.helper");

    check("busy stale routing helper survives, reclaimed by later reconcile once idle",
          survived_while_busy && reclaimed_when_idle);
}

// Reviewer's guaranteed-lifecycle ask: a helper busy at policy-change time must
// be reclaimed the moment its last request releases it, WITHOUT any follow-up
// reconcile. prune marks it pending-stale; release_inference then hands it to
// the reclaim on a background thread (a helper must never be unloaded on its own
// release call stack).
static void test_busy_helper_reclaimed_on_release(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("release.helper"));
    // An in-flight request holds the helper busy via the real request counter.
    helper->acquire_for_inference();

    RoutingHelperTestHook::reconcile(router, {});
    bool survived_while_busy = RoutingHelperTestHook::has_helper(router, "release.helper");

    // The final request releases; no second reconcile. The helper self-reclaims
    // on a background thread, so poll for the eviction to land.
    helper->release_inference();

    bool reclaimed = false;
    for (int i = 0; i < 200; ++i) {
        if (!RoutingHelperTestHook::has_helper(router, "release.helper")) {
            reclaimed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    check("busy stale routing helper self-reclaims on final release (no reconcile)",
          survived_while_busy && reclaimed);
}

// Policy update racing a helper toggling busy/idle plus concurrent reconcile
// passes. The needed-set always includes the helper during the concurrent phase
// so it is never evicted (avoiding a use-after-free on the raw pointer the
// busy-toggler holds); the assertion is that nothing deadlocks or crashes and
// the helper is still resident. A final reconcile with an empty needed-set then
// confirms it is reclaimed once idle.
static void test_concurrent_policy_update(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("race.helper"));
    RoutingHelperTestHook::reconcile(router, {"race.helper"});

    constexpr int kIterations = 2000;

    std::thread busy_toggler([&] {
        for (int i = 0; i < kIterations; ++i) {
            helper->set_busy(i % 2 == 0);
        }
        helper->set_busy(false);
    });

    std::vector<std::thread> policy_writers;
    for (int t = 0; t < 3; ++t) {
        policy_writers.emplace_back([&, t] {
            for (int i = 0; i < kIterations; ++i) {
                // Always keep race.helper needed so no reconcile evicts it mid-race.
                RoutingHelperTestHook::reconcile(
                    router, {"race.helper", "churn." + std::to_string((i + t) % 8)});
            }
        });
    }

    busy_toggler.join();
    for (auto& w : policy_writers) {
        w.join();
    }

    bool survived_race = RoutingHelperTestHook::has_helper(router, "race.helper");

    helper->set_busy(false);
    RoutingHelperTestHook::reconcile(router, {});
    bool reclaimed_after_race = !RoutingHelperTestHook::has_helper(router, "race.helper");

    check("concurrent policy update + busy toggling keeps needed helper resident",
          survived_race);
    check("helper reclaimed by reconcile after the race once no policy needs it",
          reclaimed_after_race);
}

// Reviewer's TOCTOU concern: the final request finishing between the busy check
// and arming the reclaim. mark_pending_stale_if_busy fuses one lock to both
// decide and arm, so an idle helper returns false (caller evicts now) and a busy
// one returns true (self-reclaims later) — no lost wakeup.
static void test_mark_pending_stale_atomic_decision(Router& router) {
    StubWrappedServer* idle =
        RoutingHelperTestHook::add_server(router, make_helper("atomic.idle"));
    bool idle_deferred = idle->mark_pending_stale_if_busy();

    StubWrappedServer* busy =
        RoutingHelperTestHook::add_server(router, make_helper("atomic.busy"));
    busy->acquire_for_inference();
    bool busy_deferred = busy->mark_pending_stale_if_busy();
    // Cancel the armed intent while still holding the request. Otherwise the idle
    // release below dispatches a real reclaim (this stub is stale — no policy needs
    // it), and the executor worker can evict and free it concurrently with the raw
    // pointer still in use here. Production only clears/evicts under load_mutex_,
    // where the server cannot be freed out from under a caller; this direct-pointer
    // test must cancel before going idle.
    busy->clear_pending_stale();
    busy->release_inference();

    check("mark_pending_stale_if_busy defers only a busy helper (no check/install race)",
          !idle_deferred && busy_deferred);
}

// Reviewer's ask #3: a stale helper becoming idle through maintenance completion
// (not just request release) must fire the deferred reclaim. Marked pending
// while a downsize is in flight, it self-reclaims when finish_downsize lands.
static void test_maintenance_completion_reclaims_helper(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("downsize.helper"));
    helper->set_busy(true);

    RoutingHelperTestHook::reconcile(router, {});
    bool survived_during_maintenance =
        RoutingHelperTestHook::has_helper(router, "downsize.helper");

    // Maintenance finishes; the busy->idle edge dispatches the reclaim.
    helper->finish_downsize(true);

    bool reclaimed = false;
    for (int i = 0; i < 200; ++i) {
        if (!RoutingHelperTestHook::has_helper(router, "downsize.helper")) {
            reclaimed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    check("stale helper self-reclaims when maintenance completes",
          survived_during_maintenance && reclaimed);
}

// Reviewer's ask #5: a deferred reclaim firing during a load / exclusive session
// must not give up — it waits for the residency slot to clear, then evicts. Here
// the reclaim runs on a worker thread while the main thread holds an exclusive
// session; it blocks until end_exclusive, then completes.
static void test_deferred_reclaim_waits_for_slot(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("slot.helper"));
    helper->set_busy(true);
    RoutingHelperTestHook::reconcile(router, {});  // marks pending-stale (busy)
    helper->set_busy(false);                       // idle + pending-stale, not needed

    bool acquired = router.begin_exclusive();

    std::thread worker(
        [&] { RoutingHelperTestHook::reclaim_now(router, "slot.helper"); });

    // The reclaim must block while the exclusive session owns the slot.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool blocked_during_session = RoutingHelperTestHook::has_helper(router, "slot.helper");

    router.end_exclusive();
    worker.join();
    bool reclaimed_after_session = !RoutingHelperTestHook::has_helper(router, "slot.helper");

    check("deferred reclaim waits for the exclusive slot, then evicts",
          acquired && blocked_during_session && reclaimed_after_session);
}

// Reviewer's lost-wakeup ask: a request that rescues the helper after the
// pending intent was already consumed (the reclaim dispatched but not yet
// committed) must not strand it. The reclaim must re-arm when its commit is
// refused, so the rescuer's own release reclaims the helper WITHOUT another
// reconcile.
static void test_reclaim_rearms_when_rescued_before_commit(Router& router) {
    StubWrappedServer* helper =
        RoutingHelperTestHook::add_server(router, make_helper("rescue.helper"));

    // Take the residency slot first, while the helper is still idle, so
    // begin_exclusive grants immediately. (begin_exclusive drains all in-flight
    // requests before granting, so it can never be acquired while this helper
    // already holds one — the reclaim must be parked behind an exclusive session
    // that began idle, then rescued mid-session, exactly as in production.)
    bool acquired = router.begin_exclusive();

    // A request is in flight and the policy drops the helper: prune arms the
    // release-triggered reclaim.
    helper->acquire_for_inference();
    RoutingHelperTestHook::reconcile(router, {});
    bool armed_survives = RoutingHelperTestHook::has_helper(router, "rescue.helper");

    // The release consumes the pending intent (take_pending_reclaim_if_idle
    // clears it) and dispatches the reclaim, which now blocks on the held slot.
    // The helper is thus in the exact state the bug required: intent consumed,
    // reclaim in flight but not yet committed.
    helper->release_inference();
    bool consumed_stays_resident = RoutingHelperTestHook::has_helper(router, "rescue.helper");

    // A second request arrives before that parked reclaim commits.
    helper->acquire_for_inference();

    // Releasing the slot lets the parked reclaim run. It finds the helper busy
    // again, so its commit is refused; with the intent already consumed, it must
    // re-arm rather than strand the helper — so it stays resident while busy.
    router.end_exclusive();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool resident_while_busy = RoutingHelperTestHook::has_helper(router, "rescue.helper");

    // The re-armed intent makes the second request's release reclaim the helper
    // on its own — no further reconcile.
    helper->release_inference();
    bool reclaimed = false;
    for (int i = 0; i < 200; ++i) {
        if (!RoutingHelperTestHook::has_helper(router, "rescue.helper")) {
            reclaimed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    check("reclaim re-arms on a refused commit; the rescuer's release evicts it",
          armed_survives && acquired && consumed_stays_resident &&
          resident_while_busy && reclaimed);
}

// Reviewer's promotion-path ask: an already-loaded Standard model must not be
// durably promoted to a routing helper by an in-flight request carrying an
// obsolete policy. Reconcile publishes a needed-set that excludes the model,
// then a RoutingDependency residency request arrives (as it would while
// evaluating a stale copied policy). The backend must stay resident and remain
// Standard rather than becoming a stale RoutingHelper no active policy needs.
static void test_stale_policy_does_not_promote_standard(Router& router) {
    RoutingHelperTestHook::add_server(router, make_standard("promote.model"));
    RoutingHelperTestHook::reconcile(router, {});

    bool ensured = RoutingHelperTestHook::ensure_residency(
        router, "promote.model", lemon::LoadPurpose::RoutingDependency);
    bool still_resident = RoutingHelperTestHook::has_any_model(router, "promote.model");
    bool stayed_standard =
        RoutingHelperTestHook::residency_of(router, "promote.model") ==
        ResidencyClass::Standard;

    check("stale in-flight policy cannot promote a Standard model to a helper",
          ensured && still_resident && stayed_standard);
}

// Reviewer's ask #4: router shutdown while a deferred reclaim is pending. The
// reclaim is posted to the router-owned executor and left blocked on the slot;
// destroying the Router must wake it, join the worker, and not hang or crash.
static void test_shutdown_with_pending_reclaim(RuntimeConfig& config) {
    bool completed = false;
    {
        Router router(&config, nullptr, nullptr);
        StubWrappedServer* helper =
            RoutingHelperTestHook::add_server(router, make_helper("shutdown.helper"));
        helper->set_busy(true);
        RoutingHelperTestHook::reconcile(router, {});  // marks pending-stale
        helper->set_busy(false);                       // idle + pending-stale

        // Hold the slot so the dispatched reclaim blocks on the executor worker.
        router.begin_exclusive();
        helper->finish_downsize(true);  // busy->idle edge posts the reclaim
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Scope exit destroys the Router without end_exclusive: it must wake the
        // blocked reclaim, join the executor, and tear down cleanly.
    }
    completed = true;
    check("router shutdown wakes and joins a pending deferred reclaim", completed);
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
        test_busy_helper_reclaimed_on_release(router);
        test_concurrent_policy_update(router);
        test_mark_pending_stale_atomic_decision(router);
        test_maintenance_completion_reclaims_helper(router);
        test_deferred_reclaim_waits_for_slot(router);
        test_reclaim_rearms_when_rescued_before_commit(router);
        test_stale_policy_does_not_promote_standard(router);
    }

    test_shutdown_with_pending_reclaim(config);

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll routing-helper reconcile tests passed.\n");
    } else {
        std::printf("\n%d routing-helper reconcile test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
