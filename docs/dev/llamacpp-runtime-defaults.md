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

`GET /health` against a sleeping backend still returns `200` without waking
it (llama-server serves `/health`, `/props`, `/models`, and `/metrics` from
cached responses during sleep) — Lemonade's backend-watchdog health polling
does not defeat sleep. Note that `GET /slots` is *not* on that bypass list
and wakes a sleeping server; Lemonade only forwards it on demand, never on a
timer.

## Verifying VRAM is actually released

There is no in-band signal for VRAM usage in the OpenAI-compatible response.
Confirming VRAM drops during sleep and rises back on wake requires watching
the `llama-server` process's GPU memory counters directly (e.g.
`\GPU Process Memory(*)\*` on Windows). Measured on build `b10375` (vulkan),
with the sleep/wake code paths re-verified in source at the pinned build: a
downsized `LFM2-1.2B-GGUF` process held ~0.6 MB dedicated / ~18 MB total
committed GPU memory; after a wake request it rose to ~6.5 MB dedicated /
~1.07 GB total committed, then dropped back to the same ~18 MB baseline
after re-idling — confirming the fix actually frees VRAM, unlike the old
`erase`-loop (which left GPU memory flat despite reporting success).
