# Router: size the local-LLM residency pool to policy candidates

- **Issue:** [lemonade-sdk/lemonade#2960](https://github.com/lemonade-sdk/lemonade/issues/2960)
- **Status:** Approved design, pre-implementation
- **Base commit:** `f0c5bee0` (origin/main, 2026-08-27)

## Problem

`max_loaded_models` defaults to 1. A `collection.router` policy with multiple
*local* LLM candidates causes reload thrash: alternating between candidates
evicts the LRU one via subprocess termination, forcing a 30-60s cold reload
and full KV-cache re-prefill on every switch. Cloud candidates never hit this
because they're unmetered (no local process, no residency slot). This is
exactly the routing pattern the router is meant to support, so the thrash is
self-inflicted by the current fixed pool size.

`ResidencyClass::RoutingHelper` already solves the analogous problem for
classifier/helper models (`model_residency.h`): each distinct helper model
gets its own single-slot pool, so multiple helpers stay resident together
without competing for one type-wide slot. `RoutePolicy::candidates` (the
user-facing routing targets) has no equivalent treatment — candidates load as
`ResidencyClass::Standard` and share the single global `max_loaded_models()`
slot count with every other Standard-class model of the same `ModelType`.

## Goal

Auto-raise the effective capacity of the Standard **LLM** residency pool to
fit the number of distinct local LLM candidates referenced by active routing
policies, without changing capacity for any other `ModelType`'s Standard
pool, and without weakening any existing safety mechanism (NPU exclusivity,
FLM eviction rejection, VRAM-pressure eviction, pinning).

Ship with an off-switch so this can be disabled to restore today's exact
behavior.

## Non-goals

- No change to `ResidencyClass::RoutingHelper` pool behavior.
- No change to VRAM-pressure eviction, idle eviction, or downsizing
  (`eviction_engine.cpp`) — these already operate independently of the
  count-based limit and continue to override it unconditionally.
- No per-policy isolation of capacity (see "Floor aggregation" below) — the
  Standard LLM pool remains one shared pool.
- Fallback option (b) from the issue (detect-and-warn only, no auto-raise) is
  superseded by shipping option (a) with an off-switch: turning the switch
  off degenerates to (b)'s warn-only behavior for free (see §6).

## Design

### 1. Floor aggregation semantics

The Standard LLM pool is a single shared pool, not scoped per policy
(`same_residency_pool` keys Standard entries by `ModelType` alone). The floor
is therefore the size of the **union of distinct local (non-cloud) LLM
candidates across every currently active policy** — mirroring exactly how
`Server::active_policy_helper_models()` already unions `helper_models` across
all policies today (`server.cpp`).

Rationale: a per-policy-only floor would under-count when two policies are
both reachable and could be alternated between within the eviction window
(nothing prevents a client switching which `collection.router` model it
addresses request-to-request); summing per-policy counts would over-count
when policies share a candidate. Union is the value that actually reflects
"how many distinct local Standard-LLM processes could legitimately need to be
resident at once."

A per-policy breakdown is still computed and retained (not just the total)
so logs and `/health` can show *why* the floor is what it is — e.g. "policy
`support-router` needs 3, policy `code-router` needs 2, union across all
active policies is 4" — but only the union count feeds the actual capacity
decision.

### 2. `residency_limit` becomes `ModelType`-aware

Current signature (`model_residency.h`):

```cpp
inline int residency_limit(ResidencyClass residency_class, int standard_limit);
```

New signature:

```cpp
inline int residency_limit(ResidencyClass residency_class, ModelType type,
                            int standard_limit, int llm_candidate_floor);
```

Behavior: identical to today for every case except
`residency_class == Standard && type == ModelType::LLM`, where the result is
`standard_limit == -1 ? -1 : std::max(standard_limit, llm_candidate_floor)`
(`-1` means "unlimited" and stays unlimited — the floor never needs to raise
an already-unlimited pool). Every other `(ResidencyClass, ModelType)`
combination — embedding, image, tts, reranking, transcription,
classification Standard pools, and all RoutingHelper pools — gets exactly
`standard_limit` as before, byte-for-byte unchanged.

`Router::ensure_residency_capacity()` (router.cpp:308) is the sole call site
and already has `type` in scope, so this is a mechanical signature change
plus threading one new member through, not a broader refactor.

`Router` gains:

```cpp
int llm_candidate_floor_ = 0;  // default: identical to today's behavior
```

set only via the reconcile call in §3. A freshly constructed `Router` (or one
with no active policies) behaves exactly as it does today.

### 3. Wiring — mirrors `reconcile_routing_helpers` exactly

- **`Server::active_policy_llm_candidate_floor()`** (new, `server.cpp`,
  alongside `active_policy_helper_models()`): walks
  `model_manager_->get_supported_models()`, reads each `route_policy->candidates`,
  resolves each candidate's `ModelInfo` via `model_manager_`, excludes cloud/
  unmetered candidates using the existing `is_unmetered_recipe()` check
  (same helper already used in `router.cpp`'s pool counting/LRU selection),
  excludes non-LLM-type candidates (candidates aren't schema-restricted to
  `ModelType::LLM`, unlike classifier models), unions the survivors across
  all policies, and returns both the union count and the per-policy
  breakdown (for logging/`/health`).

- **`Router::reconcile_llm_candidate_floor(int floor, uint64_t generation)`**
  (new, `router.h`/`router.cpp`): same generation-guarded, mutex-protected
  update pattern as `reconcile_routing_helpers` — stamps `llm_candidate_floor_`
  under `load_mutex_`, guarded by the same monotonic-generation check so a
  stale/racing policy reload can't clobber a newer one. No new threads, no
  new locks.

- **Call sites** (`server.cpp`): the same two places `reconcile_routing_helpers`
  is already called — the startup seed (right after `seed_needed`/
  `seed_generation` are computed) and inside the existing
  `model_manager_->set_models_changed_callback(...)` lambda, right next to
  the existing `reconcile_routing_helpers` call, sharing the same
  `generation` value so both reconciliations are atomic with respect to the
  same policy-change event.

### 4. Off-switch config

New top-level scalar in `defaults.json`, following the existing flat
convention (`max_loaded_models`, `auto_evict`):

```json
"llm_pool_autosize": true
```

`RuntimeConfig::llm_pool_autosize() const` getter, same pattern as
`auto_evict()`. Runtime-settable via the existing `/internal/set` config
mechanism (same path `max_loaded_models` itself already uses), no restart
required.

When `false`: `Server::active_policy_llm_candidate_floor()` and
`Router::reconcile_llm_candidate_floor()` still run unconditionally (so
`/health` can always show "would need N" for observability/warn-only value —
this is what makes option (b) redundant rather than a separate code path),
but `Router::ensure_residency_capacity()` reads
`config_->llm_pool_autosize() ? llm_candidate_floor_ : 0` when calling
`residency_limit()`, so actual admitted capacity reverts to exactly today's
`max_loaded_models()` behavior when the switch is off.

### 5. `/health` — `Router::get_max_model_limits()`

Current (`router.cpp:1381`): returns the same `config_->max_loaded_models()`
scalar for every `ModelType` key. Change: the `"llm"` entry becomes the
effective limit (`max(max_loaded_models(), applied_floor)`, where
`applied_floor` respects the off-switch as in §4); every other key is
unchanged. Additionally surface, alongside the existing limits object, the
raw candidate-floor diagnostics: the union count, whether autosize is
currently enabled, and the per-policy breakdown from §1 — so an operator can
see both "the limit that's actually in effect" and "why."

### 6. Constraint verification

- **NPU exclusivity / FLM eviction** — `should_reject_residency_displacement()`
  and the same-type-FLM rejection path (router.cpp:927-974) key off
  `ResidencyClass`, not the numeric limit. Untouched by this change.
- **VRAM-pressure eviction precedence** — `EvictionEngine::evaluate_servers()`
  (`eviction_engine.cpp`) is a fully separate code path (idle timeout +
  VRAM-pressure scoring) that evicts independently of
  `ensure_residency_capacity()`'s count-based LRU eviction, and a raised floor
  cannot bypass it when it runs. **However, this backstop is off by default:**
  `RuntimeConfig::auto_evict()` returns `false` when the `auto_evict` key is
  absent, `auto_evict` is not set in `defaults.json`, and
  `EvictionEngine::evaluate_servers()` skips every server when it's false. So
  on a stock install, the count-based floor this feature adds is the *only*
  residency governor for LLM candidates, and it's unbounded — a policy with N
  local candidates permits N concurrent subprocesses with no VRAM safety net.
  `Router::reconcile_llm_candidate_floor()` logs a one-line warning when a
  raised floor exceeds `max_loaded_models()` with `auto_evict` disabled, but
  does not clamp the floor or change `auto_evict`'s default — enabling
  `auto_evict` (or bounding `max_loaded_models`) is how an operator gets an
  actual backstop.
- **Cloud candidates excluded** — via `is_unmetered_recipe()`, reused as-is.
- **Residency transitions unaffected** — `transition_server_residency_locked()`
  calls `ensure_residency_capacity()` exactly as before; it now simply reads
  a possibly-larger effective limit for `(Standard, LLM)`, no logic change.

## Testing

- **Unit** — new `test/cpp/test_llm_candidate_floor.cpp`, same
  `StubWrappedServer` harness pattern as `test_routing_helper_reconcile.cpp`
  (no real subprocess). Covers: union computation across ≥2 policies with
  overlapping candidates, cloud-candidate exclusion, non-LLM-candidate
  exclusion, generation-guarded reconcile racing a concurrent policy update,
  off-switch clamping the applied floor to 0 while the diagnostic value
  still reflects the true count, and the `-1` (unlimited) passthrough.
- **e2e** — new or extended Python test (`test/server_router.py` or a new
  `test/server_router_residency.py`): register a policy with N local LLM
  candidates, alternate routing across all N, assert the underlying
  subprocess PIDs never change (the acceptance criterion from the issue).
  Also assert `/health` reflects the raised limit, and that toggling
  `llm_pool_autosize` off via `/internal/set` restores thrash (PID churn)
  without a restart.

## Acceptance criteria (from the issue, mapped to this design)

- [ ] e2e test verifying stable subprocess PIDs during alternating routing — §Testing
- [ ] Unit tests covering floor computation and edge cases — §Testing
- [ ] `/health` shows effective limits when raised — §5
- [ ] Disabled off-switch restores current behavior — §4
