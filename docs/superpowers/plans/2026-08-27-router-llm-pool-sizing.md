# Router LLM Pool Sizing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Auto-raise the Standard LLM residency pool's effective capacity to fit the distinct local LLM candidates in active routing policies, so alternating between them doesn't thrash-reload subprocesses, with an off-switch to fall back to today's behavior.

**Architecture:** A new `llm_candidate_floor_` on `Router`, kept in sync with active policies through the same generation-guarded reconcile pattern already used for routing-helper models, feeds into `residency_limit()` (now `ModelType`-aware) so `ensure_residency_capacity()` only evicts an LLM when the pool exceeds `max(max_loaded_models, floor)`.

**Tech Stack:** C++17, nlohmann/json, existing `Router`/`Server`/`RuntimeConfig` classes, CTest (`cpp-ci` label), Python `unittest`-based e2e harness in `test/`.

**Spec:** `docs/superpowers/specs/2026-08-27-router-llm-pool-sizing-design.md`

## Global Constraints

- Do not push any branch or commit to a remote at any point in this work.
- Write code and comments in this repo's existing terse, why-not-what style — see `AGENTS.md`'s "Code Style" section and the tone of `model_residency.h`/`router.cpp`. No restating what a line does, no task/issue references in comments.
- Every new/modified C++ test must be registered through `add_cpp_ci_test(... CI ON ...)` inside an `if(BUILD_TESTING AND EXISTS ...)` guard — direct `add_test()` is disabled in this repo (see `AGENTS.md`).
- `ResidencyClass::RoutingHelper` pools, VRAM-pressure eviction, NPU exclusivity/FLM rejection, and non-LLM Standard pools (embedding/image/tts/reranking/transcription/classification) must be byte-for-byte unchanged.
- `llm_pool_autosize` defaults to `true`.

---

## Task 1: Router LLM candidate floor core

**Files:**
- Modify: `src/cpp/include/lemon/model_residency.h`
- Modify: `src/cpp/include/lemon/router.h`
- Modify: `src/cpp/server/router.cpp`
- Modify: `src/cpp/resources/defaults.json`
- Modify: `src/cpp/include/lemon/runtime_config.h`
- Modify: `src/cpp/server/runtime_config.cpp`
- Create: `test/cpp/test_llm_candidate_floor.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `ResidencyClass`, `ModelType`, `Router::loaded_servers_`, `Router::load_mutex_`, `Router::ensure_residency_capacity`, `RuntimeConfig::max_loaded_models()`.
- Produces:
  - `bool RuntimeConfig::llm_pool_autosize() const`
  - `int residency_limit(ResidencyClass residency_class, ModelType type, int standard_limit, int llm_candidate_floor)` (replaces the 2-arg overload everywhere)
  - `void Router::reconcile_llm_candidate_floor(int floor, uint64_t generation)`
  - `json Router::get_max_model_limits() const` — unchanged signature, changed `"llm"` value
  - Test-only: `struct LlmPoolFloorTestHook` in `router.h`, mirroring `RoutingHelperTestHook`

- [ ] **Step 1: Add the `llm_pool_autosize` config key**

Add the key to `src/cpp/resources/defaults.json`, alphabetically between `"llamacpp"` and `"log_file"` (this file's global keys are hand-maintained per its own `_generated` header comment — only per-recipe sections are script-generated):

```json
  "llm_pool_autosize": true,
```

Add the getter declaration to `src/cpp/include/lemon/runtime_config.h`, right after `int max_loaded_models() const;` (line 38):

```cpp
    int max_loaded_models() const;
    bool llm_pool_autosize() const;
```

Add the implementation to `src/cpp/server/runtime_config.cpp`, right after `RuntimeConfig::max_loaded_models()` (currently ends at line 416):

```cpp
bool RuntimeConfig::llm_pool_autosize() const {
    std::shared_lock lock(mutex_);
    return config_["llm_pool_autosize"].get<bool>();
}
```

Add validation in `RuntimeConfig::validate()` (`src/cpp/server/runtime_config.cpp`), next to the existing `auto_evict` branch (around line 877):

```cpp
    } else if (key == "auto_evict" || key == "llm_pool_autosize") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'" + key + "' must be a boolean");
        }
```

(This replaces the existing single-key `auto_evict` branch — folding `llm_pool_autosize` into it since both take the exact same bool check, rather than adding a near-duplicate `else if`.)

- [ ] **Step 2: Make `residency_limit` ModelType-aware**

In `src/cpp/include/lemon/model_residency.h`, replace:

```cpp
// Standard pools honor max_loaded_models. A RoutingHelper pool is scoped to
// one distinct helper model, so the per-pool limit stays one while multiple
// helper models required by a policy can remain resident together.
inline int residency_limit(ResidencyClass residency_class, int standard_limit) {
    return residency_class == ResidencyClass::RoutingHelper ? 1 : standard_limit;
}
```

with:

```cpp
// Standard pools honor max_loaded_models, except the LLM pool, which also
// gets floored at llm_candidate_floor so a policy alternating between several
// local candidates doesn't reload one on every switch. -1 (unlimited) is left
// alone — nothing to floor. A RoutingHelper pool is scoped to one distinct
// helper model, so the per-pool limit stays one while multiple helper models
// required by a policy can remain resident together.
inline int residency_limit(ResidencyClass residency_class, ModelType type,
                           int standard_limit, int llm_candidate_floor) {
    if (residency_class == ResidencyClass::RoutingHelper) {
        return 1;
    }
    if (type == ModelType::LLM && standard_limit != -1) {
        return std::max(standard_limit, llm_candidate_floor);
    }
    return standard_limit;
}
```

Add `#include <algorithm>` to the top of `model_residency.h` (for `std::max`) — it currently only includes `<string>`.

- [ ] **Step 3: Thread the floor through `Router`**

In `src/cpp/include/lemon/router.h`, add near `needed_helper_models_` / `last_reconcile_generation_` (around line 313-316):

```cpp
    // Union of distinct local LLM candidates across active routing policies,
    // used to floor the Standard/LLM pool so alternating between them doesn't
    // reload a candidate on every switch. Guarded by load_mutex_. Kept
    // separately from needed_helper_models_/last_reconcile_generation_ even
    // though both are stamped from the same policy-change generation — they're
    // independent state, and reusing one counter would make the second
    // reconcile call of a pair look like a stale duplicate of the first.
    int llm_candidate_floor_ = 0;
    uint64_t last_llm_floor_generation_ = 0;
```

Add the public method declaration right after `reconcile_routing_helpers` (line 183):

```cpp
    // Raise the Standard/LLM pool's effective capacity to `floor` so active
    // policies' local candidates can stay resident together. Never evicts
    // here — a lower floor just means the next admission enforces the new
    // ceiling, the same way a lowered max_loaded_models already behaves.
    // Generation-guarded the same way as reconcile_routing_helpers, but with
    // its own counter (see llm_candidate_floor_ above).
    void reconcile_llm_candidate_floor(int floor, uint64_t generation);
```

Add the test-hook friend declaration next to `friend struct RoutingHelperTestHook;` (line 130):

```cpp
    friend struct RoutingHelperTestHook;
    friend struct LlmPoolFloorTestHook;
```

In `src/cpp/server/router.cpp`:

Replace the `ensure_residency_capacity` body (lines 308-326) — change only the `limit` line:

```cpp
void Router::ensure_residency_capacity(
    ModelType type,
    ResidencyClass residency_class,
    const std::string& model_name) {
    const int applied_floor = config_->llm_pool_autosize() ? llm_candidate_floor_ : 0;
    const int limit = residency_limit(residency_class, type, config_->max_loaded_models(),
                                      applied_floor);
    if (limit == -1 || count_servers_in_pool(type, residency_class, model_name) < limit) {
        return;
    }

    WrappedServer* lru = find_lru_server_in_pool(type, residency_class, model_name);
    if (!lru) {
        throw SlotsPinnedException(residency_pool_to_string(type, residency_class));
    }

    LOG(INFO, "Router") << "Slot limit reached for pool "
                         << residency_pool_to_string(type, residency_class)
                         << ", evicting LRU: " << lru->get_model_name() << std::endl;
    evict_server(lru);
}
```

(`ensure_residency_capacity` is only ever called with `load_mutex_` already held by its callers — `transition_server_residency_locked` and `load_model` — so reading `llm_candidate_floor_` here needs no extra lock.)

Replace `get_max_model_limits()` (lines 1381-1392):

```cpp
json Router::get_max_model_limits() const {
    int max = config_->max_loaded_models();
    int llm_limit = max;
    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        const int applied_floor = config_->llm_pool_autosize() ? llm_candidate_floor_ : 0;
        llm_limit = residency_limit(ResidencyClass::Standard, ModelType::LLM, max, applied_floor);
    }
    return {
        {"llm", llm_limit},
        {"embedding", max},
        {"reranking", max},
        {"transcription", max},
        {"image", max},
        {"tts", max},
        {"classification", max}
    };
}
```

(This method previously took no lock at all; it now needs one to read `llm_candidate_floor_` safely, since that field is written from a different thread under `load_mutex_` during policy reconciliation.)

Add `reconcile_llm_candidate_floor` right after `apply_routing_helper_reconcile` ends — that function starts at line 415 and ends around line 445, immediately before `void Router::prune_stale_routing_helpers_locked()` at line 447. Insert between the two:

```cpp
void Router::reconcile_llm_candidate_floor(int floor, uint64_t generation) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (generation <= last_llm_floor_generation_) {
        return;
    }
    last_llm_floor_generation_ = generation;
    llm_candidate_floor_ = floor;
}
```

- [ ] **Step 4: Write the test file**

Create `test/cpp/test_llm_candidate_floor.cpp`:

```cpp
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

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll LLM candidate floor tests passed.\n");
    } else {
        std::printf("\n%d LLM candidate floor test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 5: Register the test in CMakeLists.txt**

In `CMakeLists.txt`, right after the `test_routing_helper_reconcile` block (ends around line 3304, just before the "Launch command bookkeeping" comment):

```cmake
# LLM candidate pool floor: config default/override, the ModelType-aware
# residency_limit gate, and generation-guarded reconcile applying (or, with
# autosize off, not applying) the floor at admission time.
set(_LLM_CANDIDATE_FLOOR_TEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/test_llm_candidate_floor.cpp")
if(BUILD_TESTING AND EXISTS "${_LLM_CANDIDATE_FLOOR_TEST_SRC}")
    add_executable(test_llm_candidate_floor test/cpp/test_llm_candidate_floor.cpp)
    target_link_libraries(test_llm_candidate_floor PRIVATE lemonade-server-core)
    add_cpp_ci_test(LlmCandidateFloorTest CI ON COMMAND test_llm_candidate_floor)
endif()
```

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset default --target test_llm_candidate_floor
./build/test_llm_candidate_floor
```

Expected: all `[PASS]` lines, `All LLM candidate floor tests passed.`, exit code 0. Then confirm the rest of the tree still compiles clean (the `residency_limit` signature change touches every caller — there are exactly two: `ensure_residency_capacity` and `get_max_model_limits`, both edited above):

```bash
cmake --build --preset default --target lemond
```

Then re-run the pre-existing routing-helper test to confirm nothing there regressed:

```bash
cmake --build --preset default --target test_routing_helper_reconcile
./build/test_routing_helper_reconcile
```

- [ ] **Step 7: Commit**

```bash
git add src/cpp/include/lemon/model_residency.h src/cpp/include/lemon/router.h \
        src/cpp/server/router.cpp src/cpp/resources/defaults.json \
        src/cpp/include/lemon/runtime_config.h src/cpp/server/runtime_config.cpp \
        test/cpp/test_llm_candidate_floor.cpp CMakeLists.txt
git commit -m "router: floor the LLM residency pool to active policies' local candidates"
```

---

## Task 2: Server wiring — compute and apply the floor from live policies

**Files:**
- Modify: `src/cpp/include/lemon/server.h`
- Modify: `src/cpp/server/server.cpp`

**Interfaces:**
- Consumes: `Router::reconcile_llm_candidate_floor(int, uint64_t)` and `Router::get_max_model_limits()` from Task 1; `ModelManager::get_supported_models()`, `ModelManager::get_model_info(const std::string&)`, `ModelManager::resolve_model_name(const std::string&)`; `RoutePolicy::candidates` (`routing_policy.h`, already included via `server.h`); `backends::descriptor_for(recipe)` / `SlotPolicy::Unmetered` (`backend_descriptor_registry.h` / `backend_descriptor.h`, already included by `server.cpp`); `RuntimeConfig::llm_pool_autosize()` from Task 1.
- Produces:
  - `struct Server::LlmCandidateFloorInfo { std::set<std::string> models; std::map<std::string, int> per_policy_counts; }`
  - `Server::LlmCandidateFloorInfo Server::active_policy_llm_candidate_floor()`
  - `/health` response gains a top-level `"llm_pool_autosize"` object.

- [ ] **Step 1: Declare the struct and method**

In `src/cpp/include/lemon/server.h`, right after `active_policy_helper_models()` (line 167):

```cpp
    std::set<std::string> active_policy_helper_models();
    // Union of distinct local (non-cloud) LLM candidates across every active
    // router collection's policy, plus a per-policy breakdown for /health.
    // Only the union feeds Router::reconcile_llm_candidate_floor — the pool
    // it floors is shared, not scoped per policy.
    struct LlmCandidateFloorInfo {
        std::set<std::string> models;
        std::map<std::string, int> per_policy_counts;
    };
    LlmCandidateFloorInfo active_policy_llm_candidate_floor();
```

- [ ] **Step 2: Implement it**

In `src/cpp/server/server.cpp`, right after `active_policy_helper_models()` (currently lines 3864-3867):

```cpp
Server::LlmCandidateFloorInfo Server::active_policy_llm_candidate_floor() {
    LlmCandidateFloorInfo result;
    for (const auto& [name, info] : model_manager_->get_supported_models()) {
        if (!info.route_policy) {
            continue;
        }
        int local_llm_count = 0;
        for (const auto& candidate : info.route_policy->candidates) {
            ModelInfo candidate_info;
            try {
                candidate_info = model_manager_->get_model_info(candidate);
            } catch (const std::exception&) {
                continue;  // candidate no longer resolvable; not this floor's problem
            }
            if (candidate_info.type != ModelType::LLM) {
                continue;
            }
            const auto* desc = backends::descriptor_for(candidate_info.recipe);
            if (desc && desc->slot_policy == SlotPolicy::Unmetered) {
                continue;  // cloud candidate — no local process, no slot to floor
            }
            result.models.insert(model_manager_->resolve_model_name(candidate));
            ++local_llm_count;
        }
        if (local_llm_count > 0) {
            result.per_policy_counts[name] = local_llm_count;
        }
    }
    return result;
}
```

- [ ] **Step 3: Wire it into both reconcile call sites**

In `src/cpp/server/server.cpp`, the `set_models_changed_callback` lambda (lines 406-408):

```cpp
    model_manager_->set_models_changed_callback([this](uint64_t generation) {
        router_->reconcile_routing_helpers(active_policy_helper_models(), generation);
        router_->reconcile_llm_candidate_floor(
            static_cast<int>(active_policy_llm_candidate_floor().models.size()), generation);
    });
```

And the startup seed (lines 418-420):

```cpp
    const uint64_t seed_generation = model_manager_->next_notify_generation();
    const std::set<std::string> seed_needed = active_policy_helper_models();
    router_->reconcile_routing_helpers(seed_needed, seed_generation);
    router_->reconcile_llm_candidate_floor(
        static_cast<int>(active_policy_llm_candidate_floor().models.size()), seed_generation);
```

(Two separate `get_supported_models()` walks per event — one inside `active_policy_helper_models()`, one inside `active_policy_llm_candidate_floor()` — same as how helper-model reconciliation already works today; not worth merging into one combined walk for a startup/policy-reload-frequency path.)

- [ ] **Step 4: Surface it on `/health`**

In `src/cpp/server/server.cpp`, `Server::handle_health` (right after the `"max_models"` line, currently `response["max_models"] = router_->get_max_model_limits();`):

```cpp
    // Add max model limits
    response["max_models"] = router_->get_max_model_limits();

    // Candidate-floor diagnostics: what raised (or would raise) the LLM limit
    // above, and whether autosize is actually applying it right now.
    {
        auto floor_info = active_policy_llm_candidate_floor();
        response["llm_pool_autosize"] = {
            {"enabled", config_->llm_pool_autosize()},
            {"candidate_floor", static_cast<int>(floor_info.models.size())},
            {"policies", floor_info.per_policy_counts}
        };
    }
```

- [ ] **Step 5: Build**

```bash
cmake --build --preset default --target lemond
```

Expected: clean build. No dedicated C++ unit test for this task — `active_policy_helper_models()` (the pattern this mirrors) has none either; both are exercised end-to-end in Task 3.

- [ ] **Step 6: Commit**

```bash
git add src/cpp/include/lemon/server.h src/cpp/server/server.cpp
git commit -m "server: compute the active-policy LLM candidate floor and reconcile it into the router"
```

---

## Task 3: End-to-end verification

**Files:**
- Modify: `test/server_router.py`

**Interfaces:**
- Consumes: the existing `RouterTests` fixture (`COLLECTION_NAME`, `POLICY`, `DEFAULT_MODEL`, `CAPABLE_MODEL`, `self._route(...)`, `self.base_url`) already registered by `_ensure_setup()`; `set_server_config` / `get_config` from `utils.server_base` (already imported at the top of `server_router.py`).
- Produces: three new test methods on `RouterTests`.

`POLICY` (already registered by every test in this file, `test/server_router.py:188-217`) already declares `DEFAULT_MODEL` and `CAPABLE_MODEL` as its two local candidates — exactly the multi-local-candidate policy this feature targets. `max_loaded_models` defaults to 1 in the test server, so today (pre-fix) alternating between them reloads one every time; after this feature, `/health`'s `max_models.llm` should read at least 2 while `COLLECTION_NAME` is registered, and the PIDs should stop changing.

- [ ] **Step 1: Add a PID helper**

In `test/server_router.py`, add a small helper method to `RouterTests`, next to `_trace_map` (around line 308):

```python
    def _model_pid(self, model):
        resp = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200, resp.text[:1000])
        for loaded in resp.json().get("all_models_loaded", []):
            if loaded.get("model_name") == model:
                return loaded.get("pid")
        return None
```

- [ ] **Step 2: Write the PID-stability test**

Add after `test_606_short_conversation_falls_through` (before `test_610_cloud_candidate_routing`, around line 540):

```python
    def test_607_alternating_local_candidates_keep_stable_pids(self):
        """Routing back and forth between the two local candidates must not
        reload either one — the reload-thrash issue #2960 fixes."""
        self._route("Give me a fun fact about otters.")  # -> DEFAULT_MODEL
        default_pid = self._model_pid(DEFAULT_MODEL)
        self.assertTrue(default_pid, f"{DEFAULT_MODEL} did not report a pid")

        self._route("Write a Python function to reverse a linked list.")  # -> CAPABLE_MODEL
        capable_pid = self._model_pid(CAPABLE_MODEL)
        self.assertTrue(capable_pid, f"{CAPABLE_MODEL} did not report a pid")

        for _ in range(3):
            self._route("Give me a fun fact about otters.")
            self.assertEqual(
                self._model_pid(DEFAULT_MODEL),
                default_pid,
                f"{DEFAULT_MODEL} was reloaded while alternating candidates",
            )
            self._route("Write a Python function to reverse a linked list.")
            self.assertEqual(
                self._model_pid(CAPABLE_MODEL),
                capable_pid,
                f"{CAPABLE_MODEL} was reloaded while alternating candidates",
            )
        print("[OK] alternating local candidates kept both subprocesses stable")

    def test_608_health_reports_raised_llm_limit(self):
        """/health surfaces the raised limit and why it was raised."""
        resp = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200, resp.text[:1000])
        body = resp.json()

        self.assertGreaterEqual(
            body.get("max_models", {}).get("llm", 0),
            2,
            "max_models.llm should be floored to at least the two local candidates",
        )

        autosize = body.get("llm_pool_autosize", {})
        self.assertTrue(autosize.get("enabled"))
        self.assertGreaterEqual(autosize.get("candidate_floor", 0), 2)
        self.assertGreaterEqual(
            autosize.get("policies", {}).get(COLLECTION_NAME, 0), 2
        )
        print("[OK] /health reflects the raised llm limit and its cause")

    def test_609_autosize_off_restores_thrash(self):
        """Disabling llm_pool_autosize reverts to today's reload-per-switch
        behavior, proving the off-switch actually gates the new capacity."""
        set_server_config({"llm_pool_autosize": False})
        try:
            resp = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
            self.assertEqual(resp.status_code, 200, resp.text[:1000])
            self.assertFalse(resp.json().get("llm_pool_autosize", {}).get("enabled"))
            self.assertEqual(resp.json().get("max_models", {}).get("llm"), 1)

            self._route("Give me a fun fact about otters.")  # -> DEFAULT_MODEL
            default_pid = self._model_pid(DEFAULT_MODEL)
            self._route("Write a Python function to reverse a linked list.")  # -> CAPABLE_MODEL loads, evicts DEFAULT_MODEL
            self._route("Give me a fun fact about otters.")  # -> DEFAULT_MODEL reloads
            self.assertNotEqual(
                self._model_pid(DEFAULT_MODEL),
                default_pid,
                "with autosize off, max_loaded_models=1 should still thrash",
            )
        finally:
            set_server_config({"llm_pool_autosize": True})
        print("[OK] disabling llm_pool_autosize restores max_loaded_models=1 thrash")
```

- [ ] **Step 3: Run**

```bash
python test/server_router.py --wrapped-server llamacpp --backend cpu
```

(Match whatever `--wrapped-server`/`--backend` flags the rest of this suite's CI job uses — check `.github/workflows/cpp_server_build_test_release.yml` for the exact invocation if unsure. `DEFAULT_MODEL`/`CAPABLE_MODEL` are pulled automatically by `_ensure_setup()` if not already cached.)

Expected: `test_607_*`, `test_608_*`, `test_609_*` pass alongside the existing `RouterTests` cases.

- [ ] **Step 4: Commit**

```bash
git add test/server_router.py
git commit -m "test(router): verify stable PIDs, raised /health limit, and the autosize off-switch"
```
