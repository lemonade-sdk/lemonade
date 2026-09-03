# llama.cpp Backend: Pinned Runtime Defaults

Lemonade launches `llama-server` with a few forced defaults that change how its
own idle/caching features behave. This page documents those defaults and how
soft-idle downsizing (`auto_evict` + `downsize_idle_timeout`) maps onto them.
The authoritative `llama-server` versions are pinned in
`src/cpp/resources/backend_versions.json`; the behavior described here was
verified against the source at those pins.

## `--parallel 1` is forced

`LlamaCppOps::resolve_llamacpp_runtime_args()`
(`src/cpp/server/backends/llamacpp/llamacpp_server.cpp`) always appends
`--parallel 1` to the launch args unless a custom `--parallel`/`-np` is
supplied. Auto slot count (`--parallel -1`, llama-server's own default) would
silently divide the requested `ctx_size` across slots, a Lemonade `ctx_size`
request is meant to apply to a single conversation, not be split N ways.

The consequence: llama-server's `kv_unified` stays at its own default of
`false` (it's only forced `true` when `--parallel` is left at auto). With
`kv_unified=false`, per-slot idle features (`--cache-idle-slots`, on by
default) only publish a **RAM copy** of an idle slot's KV, they do not free
the slot's GPU allocation. So under Lemonade's actual launch config, none of
llama-server's per-slot idle handling reclaims VRAM.

## Soft-idle downsize maps to `--sleep-idle-seconds`

`--sleep-idle-seconds <seconds>` is llama-server's whole-server sleep
mechanism (independent of `kv_unified`/slot count). When idle time elapses,
llama-server drops the model, KV, and compute buffers entirely, releasing the
real GPU allocation. The next request transparently blocks until llama-server
reloads the model from disk, then re-prefills the prompt from scratch.

**Sleep does not preserve the RAM prompt cache, regardless of `--parallel`.**
Entering sleep calls `destroy()`, which tears down the slot's KV state
directly without calling `slot.prompt_save()` first; waking calls
`load_model()`, which recreates `server_prompt_cache` from scratch and so
discards anything the RAM prompt cache (`--cache-ram`, on by default
upstream) held before sleep. A wake is therefore always a cold re-prefill -
even a cache deliberately populated before sleep is gone afterward, and
raising `--parallel` would not change that. Empirically: waking a downsized
model always shows `timings.cache_n: 0` and a full prompt re-prefill, even
for an exact-repeat prompt, confirmed against a live request/response pair.
(While the server is *awake*, the RAM cache does function even under
`--parallel 1`: LRU slot reuse saves the displaced prompt to the cache before
loading a new one, and `cache_n > 0` on a repeat prompt to an always-awake
model is ordinary in-slot KV reuse - both unrelated to sleep/wake.)

`LlamaCppServer::downsize()` delegates to this: it is a logging no-op, and the
actual downsizing is `--sleep-idle-seconds` doing its job. The flag is only
passed at launch when `auto_evict` resolves `true` for the recipe (recipe
option, else `RuntimeConfig::global()->auto_evict()`); when `auto_evict` is
off (the default), no `--sleep-idle-seconds` flag is added and llama-server's
sleep behavior is fully disabled, matching pre-existing behavior.

The resolved `downsize_idle_timeout` (recipe option, else 60s default) becomes
the `--sleep-idle-seconds` value, with one quirk: a `downsize_idle_timeout` of
`0` ("downsize as soon as idle") is clamped up to `1`, since llama-server
rejects `--sleep-idle-seconds 0` at startup (`common/arg.cpp` throws
`invalid_argument`; valid values are `-1` = disabled or `>= 1`).

`--sleep-idle-seconds` is an overridable runtime default like `--parallel`
above: a user-supplied `llamacpp_args` value wins over the one Lemonade would
compute from `downsize_idle_timeout` (`append_runtime_arg_defaults` skips its
own default whenever the flag is already present). `LlamaCppServer::load()`
parses whatever value actually ended up in the resolved args - not just
whether the flag string is present - and stores it as
`sleep_idle_seconds_effective_`; `sleep_idle_enabled_` is `true` only when
that value is `>= 1`, matching llama-server's own definition of "enabled"
(an explicit `--sleep-idle-seconds -1` in a custom `llamacpp_args` is treated
as disabled, not as "the flag is present so downsize applies"). `EvictionEngine`
schedules its own idle check against this same effective value
(`WrappedServer::effective_downsize_idle_timeout_sec()`) instead of the
requested `downsize_idle_timeout`, so a custom `llamacpp_args` value that
overrides `--sleep-idle-seconds` to something other than what
`downsize_idle_timeout` requested doesn't leave `ModelState` reporting
`READY` long after (or attempting downsize long before) llama-server's own
timer actually fires.

`GET /health` against a sleeping backend still returns `200` without waking
it - its handler never checks sleep state or touches `ctx_server` at all,
it unconditionally returns `{"status": "ok"}`. `/props`, `/models`, and
`/metrics` take a different path to the same result: they explicitly check
`is_sleeping` and, if true, serve from cached responses populated when the
server entered sleep. Either way, Lemonade's backend-watchdog health polling
does not defeat sleep. Note that `GET /slots` is *not* on that bypass list
and wakes a sleeping server; Lemonade only forwards it on demand, never on a
timer.

## `ModelState::DOWNSIZED` is verified

Lemonade's `EvictionEngine` runs its own idle timer and, once it elapses, calls
`LlamaCppServer::downsize()` to decide whether to flip a model to
`ModelState::DOWNSIZED`. Rather than trusting that llama-server's independent
`--sleep-idle-seconds` timer fired just because Lemonade's own timer did,
`downsize()` checks the subprocess's own `GET /props` (`is_sleeping` field) and
only reports success once the backend confirms it's actually asleep:

- If `/props` says `is_sleeping: false` yet, `downsize()` returns `false`;
  `EvictionEngine::finish_downsize(false)` reverts the model to `READY`, and
  since a failed downsize doesn't touch the idle clock, the model remains a
  candidate and is retried on `EvictionEngine`'s next tick (every 5s by
  default) until llama-server's timer actually fires. `ModelState::DOWNSIZED`
  for llama.cpp is therefore ground truth, converging within one tick of the
  real sleep, not a guess.
- If the model was launched without `--sleep-idle-seconds` in the first place
  (`auto_evict` was off, or a pre-b7492 `system` backend hit the version gate
  in `resolve_llamacpp_runtime_args`), `downsize()` skips the `/props` check
  entirely and returns `true`, there's no backend-side sleep timer to verify,
  and retrying forever against a backend that can never sleep would just waste
  a `/props` round trip on every tick for no benefit.

### Downsize eligibility tracks what's baked in, not the live `auto_evict` value

`--sleep-idle-seconds` is fixed for the life of the `llama-server` subprocess:
it was baked in (or not) once, when `auto_evict` was resolved at `load()`
time. A later `/internal/set` or recipe update changes the live `auto_evict`
config, but can't add or remove a flag from an already-running process.

`EvictionEngine::evaluate_servers()` accounts for this by asking each server
`WrappedServer::downsize_effective_for_this_instance(bool auto_evict_config)`
whether downsize is actually eligible for *this instance*, and uses that
result, not the live `auto_evict` value, to decide whether to collect the
model for downsize. The default implementation just mirrors the passed-in
live config (`return auto_evict_config;`), so every non-llamacpp backend,
which has no launch-time state to diverge from, behaves exactly as if the
live value were used directly. `LlamaCppServer` overrides it to return
`sleep_idle_enabled_` instead, which reflects what was actually baked into
`--sleep-idle-seconds` at load time:

- If the model was loaded with `auto_evict=false` and later toggled on,
  `downsize_eligible` stays `false` (the flag was never baked in), so the
  model is never collected for downsize.
- If the model was loaded with `auto_evict=true` and later toggled off,
  `downsize_eligible` stays `true` (the flag is baked in and llama-server's
  timer is still running), so the model is still collected and verified via
  the `/props` check above, keeping `ModelState` in sync with the backend.

Hard-idle-eviction and VRAM-pressure eviction are gated purely on the live
`auto_evict` value instead, since unloading the process outright is a
genuinely config-driven action with no launch-time state to track.

### Pinned models never get `--sleep-idle-seconds` baked in

A pinned model must never sleep on its own, so `resolve_runtime_options()`
forces `sleep_idle_seconds = -1` whenever `pinned` is true, before even
considering `auto_evict`. `EvictionEngine` skipping pinned models is not
enough on its own to guarantee this - that skip only stops *Lemonade* from
downsizing it, it says nothing about llama-server's own independent timer.

Forcing `sleep_idle_seconds = -1` only prevents new `--sleep-idle-seconds`
from being added; it does not remove one already present in `custom_args`. This can happen when `resolve_effective_options()` re-resolves already-baked `llamacpp_args` (eg: `/internal/pin` reloads from the running server's resolved options) or when a user explicitly supplies `llamacpp_args="--sleep-idle-seconds N"` on a pinned load.

Therefore when `pinned` is true, `resolve_runtime_options()` must also strip `--sleep-idle-seconds` from custom_args via `lemon::utils::remove_custom_arg()`, ensuring the pinned invariant overrides explicit user values.

Like `auto_evict`, this is baked in at launch and can't change on a running
process. `/v1/load`'s existing reload-on-option-change check already handles
a `pinned` toggle for free, since it changes the resolved `llamacpp_args`.
`Router::set_model_pinned()` (`/internal/pin`) previously only flipped an
in-memory flag; it now checks whether the new pin state would change the
baked args and reloads the backend through `load_model()` if so, otherwise
staying a cheap in-memory flip.

`POST /internal/backend-props` forwards a model's `GET /props` verbatim, so
tests can assert `is_sleeping` as ground truth instead of trusting Lemonade's
own `pinned`/`status` fields.

The **wake** direction (`DOWNSIZED` → `READY`, in `WrappedServer::acquire_for_inference()`
calling `restore()`) is still optimistic for every backend, including
llama.cpp: `restore()` is a no-op, and Lemonade flips to `READY` before
forwarding the request that actually triggers llama-server's self-wake. This
residual gap is low-risk and intentionally left alone, it's a brief window
that self-corrects on the very next request/response, and the forwarded
request itself already blocks correctly until llama-server finishes waking
regardless of what `ModelState` claims in the meantime.

## Verifying VRAM is actually released

There is no in-band signal for VRAM usage in the OpenAI-compatible response.
Confirming VRAM drops during sleep and rises back on wake requires watching
the `llama-server` process's GPU memory counters directly (e.g.
`\GPU Process Memory(*)\*` on Windows). Measured on build `b10375` (vulkan),
with the sleep/wake code paths re-verified in source at the pinned build: a
downsized `LFM2-1.2B-GGUF` process held ~0.6 MB dedicated / ~18 MB total
committed GPU memory; after a wake request it rose to ~6.5 MB dedicated /
~1.07 GB total committed, then dropped back to the same ~18 MB baseline
after re-idling - confirming the fix actually frees VRAM, unlike the old
`erase`-loop (which left GPU memory flat despite reporting success).
