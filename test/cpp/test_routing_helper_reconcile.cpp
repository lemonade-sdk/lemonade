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
// Residency transitions and retry classification are tested through the same
// lock-held helpers used by load_model, with StubWrappedServer instances rather
// than a ModelManager-backed subprocess.

#include "lemon/cloud_provider_registry.h"
#include "lemon/model_manager.h"
#include "lemon/models/catalog.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"
#include "lemon/wrapped_server.h"
#include "support/test_state_isolation.h"

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
        : WrappedServer("stub", "error", nullptr, nullptr),
          instance_id_(next_instance_id()) {
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
    unsigned instance_id() const { return instance_id_; }

private:
    static unsigned next_instance_id() {
        static std::atomic<unsigned> next{0};
        return ++next;
    }

    std::atomic<bool> alive_{true};
    std::atomic<bool> unloaded_{false};
    unsigned instance_id_;
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

    static Router::RoutingHelperLease lease_transient(
        Router& r, std::set<std::string> canonical_models) {
        return r.lease_transient_routing_helpers_canonical(std::move(canonical_models));
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

    static unsigned instance_id(Router& r, const std::string& model_name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        auto* server = dynamic_cast<StubWrappedServer*>(
            r.find_server_by_model_name(model_name));
        return server ? server->instance_id() : 0;
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

    static void transition_residency(Router& r, const std::string& model_name,
                                     ResidencyClass residency) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        auto* server = r.find_server_by_model_name(model_name);
        r.transition_server_residency_locked(server, residency);
    }

    static bool should_skip_nuclear_retry(Router& r, const std::string& model_name,
                                          ResidencyClass residency) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        return r.should_skip_nuclear_retry_for_transient_helper_locked(model_name, residency);
    }

    static void set_load_in_progress(Router& r, bool loading) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.is_loading_ = loading;
        r.load_cv_.notify_all();
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
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::RecipeOptions;
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

static bool wait_for_helper_eviction(Router& router, const std::string& model_name) {
    for (int i = 0; i < 200; ++i) {
        if (!RoutingHelperTestHook::has_helper(router, model_name)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
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

static void test_transient_lease_survives_policy_reconcile(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("transient.helper"));
    bool survived_reconcile = false;
    {
        [[maybe_unused]] auto lease = RoutingHelperTestHook::lease_transient(
            router, {"transient.helper"});
        RoutingHelperTestHook::reconcile(router, {});
        survived_reconcile = RoutingHelperTestHook::has_helper(
            router, "transient.helper");
    }

    check("transient helper lease survives policy reconcile and releases on scope exit",
          survived_reconcile &&
              !RoutingHelperTestHook::has_helper(router, "transient.helper"));
}

static void test_overlapping_transient_leases(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("overlap.helper"));
    auto first = RoutingHelperTestHook::lease_transient(router, {"overlap.helper"});
    auto second = RoutingHelperTestHook::lease_transient(router, {"overlap.helper"});

    first = {};
    bool survived_first_release =
        RoutingHelperTestHook::has_helper(router, "overlap.helper");
    second = {};

    check("overlapping transient leases reclaim only after the final release",
          survived_first_release &&
              !RoutingHelperTestHook::has_helper(router, "overlap.helper"));
}

static void test_concurrent_transient_leases_and_reconcile(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("concurrent.helper"));
    auto anchor = RoutingHelperTestHook::lease_transient(
        router, {"concurrent.helper"});
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    auto exercise_lease = [&] {
        ++ready;
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 500; ++i) {
            auto lease = RoutingHelperTestHook::lease_transient(
                router, {"concurrent.helper"});
        }
    };

    std::thread first(exercise_lease);
    std::thread second(exercise_lease);
    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    start = true;
    for (int i = 0; i < 500; ++i) {
        RoutingHelperTestHook::reconcile(router, {});
    }
    first.join();
    second.join();
    bool survived_concurrent_updates =
        RoutingHelperTestHook::has_helper(router, "concurrent.helper");
    anchor = {};

    check("concurrent lease release and policy reconcile preserve then reclaim helper",
          survived_concurrent_updates &&
              !RoutingHelperTestHook::has_helper(router, "concurrent.helper"));
}

static void test_transient_lease_preserves_registered_helper(Router& router) {
    RoutingHelperTestHook::add_server(router, make_helper("registered.helper"));
    auto lease = RoutingHelperTestHook::lease_transient(
        router, {"registered.helper"});
    RoutingHelperTestHook::reconcile(router, {"registered.helper"});
    lease = {};
    bool survived_lease_release =
        RoutingHelperTestHook::has_helper(router, "registered.helper");

    RoutingHelperTestHook::reconcile(router, {});
    check("registered policy keeps a helper resident after transient lease release",
          survived_lease_release &&
              !RoutingHelperTestHook::has_helper(router, "registered.helper"));
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

    bool reclaimed = wait_for_helper_eviction(router, "release.helper");

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

    bool reclaimed = wait_for_helper_eviction(router, "downsize.helper");

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

// A transient validation lease protects helpers loaded for the request, but it
// must not change the residency of a model that was already loaded by the user.
static void test_transient_lease_preserves_standard_model(Router& router) {
    RoutingHelperTestHook::add_server(router, make_standard("promote.model"));
    RoutingHelperTestHook::reconcile(router, {});

    bool ensured = false;
    {
        [[maybe_unused]] auto lease = RoutingHelperTestHook::lease_transient(
            router, {"promote.model"});
        ensured = RoutingHelperTestHook::ensure_residency(
            router, "promote.model", lemon::LoadPurpose::RoutingDependency);
    }
    bool still_resident = RoutingHelperTestHook::has_any_model(router, "promote.model");
    bool stayed_standard =
        RoutingHelperTestHook::residency_of(router, "promote.model") ==
        ResidencyClass::Standard;

    check("transient validation lease preserves a pre-existing Standard model",
          ensured && still_resident && stayed_standard);
}

static void test_second_load_check_preserves_standard_model(Router& router) {
    auto* standard = RoutingHelperTestHook::add_server(
        router, make_standard("concurrent.standard"));
    const unsigned original_instance = standard->instance_id();
    RoutingHelperTestHook::reconcile(router, {});

    {
        [[maybe_unused]] auto lease = RoutingHelperTestHook::lease_transient(
            router, {"concurrent.standard"});
        RoutingHelperTestHook::transition_residency(
            router, "concurrent.standard", ResidencyClass::RoutingHelper);
    }

    const bool still_resident =
        RoutingHelperTestHook::has_any_model(router, "concurrent.standard");
    const bool same_instance = RoutingHelperTestHook::instance_id(
        router, "concurrent.standard") == original_instance;
    const bool stayed_standard =
        RoutingHelperTestHook::residency_of(router, "concurrent.standard") ==
        ResidencyClass::Standard;

    check("second load check preserves the same Standard model instance",
          still_resident && same_instance && stayed_standard);
}

static void test_transient_only_failure_classification(Router& router) {
    RoutingHelperTestHook::reconcile(router, {});
    bool standard_load_keeps_normal_retry =
        !RoutingHelperTestHook::should_skip_nuclear_retry(
            router, "failure.helper", ResidencyClass::Standard);

    bool transient_failure_skips_global_retry = false;
    {
        [[maybe_unused]] auto lease = RoutingHelperTestHook::lease_transient(
            router, {"failure.helper"});
        transient_failure_skips_global_retry =
            RoutingHelperTestHook::should_skip_nuclear_retry(
                router, "failure.helper", ResidencyClass::RoutingHelper);
        RoutingHelperTestHook::reconcile(router, {"failure.helper"});
        transient_failure_skips_global_retry =
            transient_failure_skips_global_retry &&
            !RoutingHelperTestHook::should_skip_nuclear_retry(
                router, "failure.helper", ResidencyClass::RoutingHelper);
    }

    check("only transiently leased helper failures skip the global retry",
          standard_load_keeps_normal_retry && transient_failure_skips_global_retry);
}

static void test_transient_load_failure_skips_global_retry_and_allows_later_load(
    Router& router) {
    const std::string model_name = "transient.failure";
    auto* standard = RoutingHelperTestHook::add_server(
        router, make_standard("resident.standard"));
    const unsigned original_instance = standard->instance_id();
    auto lease = RoutingHelperTestHook::lease_transient(router, {model_name});

    ModelInfo failing_model;
    failing_model.model_name = model_name;
    failing_model.recipe = "cloud";
    failing_model.checkpoints["main"] = "fixture-model";
    std::string failure_message;
    try {
        router.load_model(model_name, failing_model, RecipeOptions(), true,
                          false, std::nullopt,
                          lemon::LoadPurpose::RoutingDependency);
    } catch (const std::runtime_error& error) {
        failure_message = error.what();
    }
    const bool non_file_not_found_startup_failure =
        failure_message.find("missing the 'cloud_provider' field") !=
        std::string::npos;

    const bool global_retry_was_skipped =
        RoutingHelperTestHook::instance_id(router, "resident.standard") ==
        original_instance;

    ModelInfo retry_model = failing_model;
    retry_model.cloud_provider = "fixture";
    bool subsequent_load_succeeded = false;
    try {
        router.load_model(model_name, retry_model, RecipeOptions(), true,
                          false, std::nullopt,
                          lemon::LoadPurpose::RoutingDependency);
        subsequent_load_succeeded =
            RoutingHelperTestHook::has_helper(router, model_name);
    } catch (const std::exception&) {
        subsequent_load_succeeded = false;
    }

    check("transient startup failure skips global retry and permits a later load",
          non_file_not_found_startup_failure && global_retry_was_skipped &&
              subsequent_load_succeeded);
}

static void test_last_transient_lease_release_waits_for_load(Router& router) {
    const std::string model_name = "lease.load-slot";
    RoutingHelperTestHook::add_server(router, make_helper(model_name));
    {
        auto lease = RoutingHelperTestHook::lease_transient(router, {model_name});
        RoutingHelperTestHook::set_load_in_progress(router, true);
    }
    const bool resident_during_load =
        RoutingHelperTestHook::has_helper(router, model_name);
    RoutingHelperTestHook::set_load_in_progress(router, false);
    const bool reclaimed_after_load = wait_for_helper_eviction(router, model_name);

    check("last transient lease released during load reclaims after the slot clears",
          resident_during_load && reclaimed_after_load);
}

static void test_last_transient_lease_release_waits_for_exclusive(Router& router) {
    const std::string model_name = "lease.exclusive-slot";
    RoutingHelperTestHook::add_server(router, make_helper(model_name));
    auto lease = RoutingHelperTestHook::lease_transient(router, {model_name});
    const bool acquired = router.begin_exclusive();

    std::thread releaser([lease = std::move(lease)]() mutable {
        static_cast<void>(lease);
    });
    releaser.join();
    const bool resident_during_exclusive =
        RoutingHelperTestHook::has_helper(router, model_name);
    router.end_exclusive();
    const bool reclaimed_after_exclusive = wait_for_helper_eviction(router, model_name);

    check("last transient lease released during an exclusive session reclaims after it ends",
          acquired && resident_during_exclusive && reclaimed_after_exclusive);
}

static void test_last_transient_lease_release_waits_for_inference_and_can_be_rescued(
    Router& router) {
    const std::string model_name = "lease.inference-rescue";
    StubWrappedServer* helper = RoutingHelperTestHook::add_server(
        router, make_helper(model_name));
    helper->acquire_for_inference();
    {
        auto lease = RoutingHelperTestHook::lease_transient(router, {model_name});
    }

    const bool resident_while_busy =
        RoutingHelperTestHook::has_helper(router, model_name);
    bool resident_after_rescue = false;
    {
        auto rescued_lease =
            RoutingHelperTestHook::lease_transient(router, {model_name});
        helper->release_inference();
        resident_after_rescue =
            RoutingHelperTestHook::has_helper(router, model_name);
    }
    const bool reclaimed_after_final_release =
        !RoutingHelperTestHook::has_helper(router, model_name);

    check("a new transient lease rescues a busy helper until its final release",
          resident_while_busy && resident_after_rescue &&
              reclaimed_after_final_release);
}

static void test_last_transient_lease_release_waits_for_inference_without_rescue(
    Router& router) {
    const std::string model_name = "lease.inference-no-rescue";
    StubWrappedServer* helper = RoutingHelperTestHook::add_server(
        router, make_helper(model_name));
    helper->acquire_for_inference();
    {
        auto lease = RoutingHelperTestHook::lease_transient(router, {model_name});
    }

    const bool resident_while_busy =
        RoutingHelperTestHook::has_helper(router, model_name);
    helper->release_inference();
    const bool reclaimed_after_inference = wait_for_helper_eviction(router, model_name);

    check("last transient lease release while busy reclaims without rescue or reconcile",
          resident_while_busy && reclaimed_after_inference);
}

static void test_registered_policy_promotes_standard_model(Router& router) {
    RoutingHelperTestHook::add_server(router, make_standard("registered.standard"));
    RoutingHelperTestHook::reconcile(router, {"registered.standard"});

    bool ensured = false;
    {
        [[maybe_unused]] auto lease = RoutingHelperTestHook::lease_transient(
            router, {"registered.standard"});
        ensured = RoutingHelperTestHook::ensure_residency(
            router, "registered.standard", lemon::LoadPurpose::RoutingDependency);
    }
    bool stayed_helper = RoutingHelperTestHook::has_helper(
        router, "registered.standard");

    check("registered policy still promotes and retains a Standard model",
          ensured && stayed_helper);
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
    cfg["disable_model_filtering"] = false;
    cfg["offline"] = false;

    RuntimeConfig config(cfg);
    RuntimeConfig::set_global(&config);

    // Each Router owns its own monitor/eviction threads; scope them so they are
    // torn down before the shared global config is cleared.
    {
        Router router(&config, nullptr, nullptr);
        test_stale_idle_helper_evicted(router);
        test_needed_helper_survives(router);
        test_transient_lease_survives_policy_reconcile(router);
        test_overlapping_transient_leases(router);
        test_concurrent_transient_leases_and_reconcile(router);
        test_transient_lease_preserves_registered_helper(router);
        test_pinned_stale_helper_survives(router);
        test_standard_model_untouched(router);
        test_busy_helper_reclaimed_when_idle(router);
        test_busy_helper_reclaimed_on_release(router);
        test_concurrent_policy_update(router);
        test_mark_pending_stale_atomic_decision(router);
        test_maintenance_completion_reclaims_helper(router);
        test_deferred_reclaim_waits_for_slot(router);
        test_reclaim_rearms_when_rescued_before_commit(router);
        test_transient_lease_preserves_standard_model(router);
        test_second_load_check_preserves_standard_model(router);
        test_transient_only_failure_classification(router);
        test_registered_policy_promotes_standard_model(router);
    }

    {
        const auto scratch = lemon::test::make_scratch_dir("routing_helper_load_failure");
        lemon::utils::set_models_dir((scratch / "models").string());

        ModelManagerConfig model_manager_config;
        model_manager_config.cache_dir = scratch.string();
        auto transient_model_catalog =
            std::make_shared<lemon::models::EnumeratedCatalog>(
                lemon::models::CatalogDescriptor{"routing-helper-test"},
                [](const lemon::models::CatalogAccumulator&) {
                    ModelInfo info;
                    info.model_name = "transient.failure";
                    info.recipe = "cloud";
                    info.cloud_provider = "fixture";
                    info.checkpoints["main"] = "fixture-model";
                    info.suggested = true;
                    info.labels = {"chat"};
                    return std::vector<ModelInfo>{std::move(info)};
                });
        model_manager_config.extra_catalogs = {transient_model_catalog};
        model_manager_config.availability =
            [](const std::map<std::string, ModelInfo>&) {
                lemon::models::BackendAvailability availability;
                availability.system_ram_gb = 64.0;
                availability.largest_mem_pool_gb = 48.0;
                return availability;
            };
        ModelManager model_manager(std::move(model_manager_config));
        lemon::CloudProviderRegistry cloud_registry;
        cloud_registry.install("fixture", "https://fixture.invalid");
        Router router(&config, &model_manager, nullptr);
        router.set_cloud_registry(&cloud_registry);
        test_transient_load_failure_skips_global_retry_and_allows_later_load(router);
    }

    {
        Router router(&config, nullptr, nullptr);
        test_last_transient_lease_release_waits_for_load(router);
        test_last_transient_lease_release_waits_for_exclusive(router);
        test_last_transient_lease_release_waits_for_inference_and_can_be_rescued(
            router);
        test_last_transient_lease_release_waits_for_inference_without_rescue(
            router);
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
