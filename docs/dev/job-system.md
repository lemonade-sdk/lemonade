# Server-side job system

The job engine runs **client-posted sequences of server operations** on lemond:
each job is an ordered list of steps that pass data forward through a shared
context, branch on results, and have a pause / interrupt / resume / delete /
query lifecycle that survives client disconnect and server restart.

It exists so multi-step server work — the motivating case is the AutoOpt
benchmark methodology (repeatedly load a model with a config, time a
completion, unload) — runs **on the server**, coordinated with model
management, instead of being driven from a browser that a page reload would
kill. The engine is generic and domain-agnostic: the client owns the recipe and
any synthesis of the results; the server just executes the recipe durably.

## Concepts

- **Job** — a named list of steps plus an `inputs` object, an accumulating
  `context`, a `status`, and a `cursor` (the id of the current/next step).
- **Step** — one operation (`op`) with `params`, an optional `when` guard,
  optional `extract` mappings, and optional forward branches. Steps have their
  own status (`pending`/`running`/`completed`/`failed`/`skipped`).
- **Context** — a JSON object, the data bus. After a step runs, its raw output
  is stored under `context[<step id>]`, and any `extract` mappings copy fields
  to top-level keys. Job `inputs` are available under `context.inputs`.
- **Op** — a named server operation. `params` are reference-resolved against the
  context before the op runs.

## Operations

| op            | exclusive | output |
|---------------|-----------|--------|
| `system_info` | no  | the `system-info` snapshot |
| `system_stats`| no  | cpu/gpu/vram/npu/memory sample |
| `models`      | no  | model list, or `{id}` metadata when `params.id` is set |
| `sleep`       | no  | `{}` after `params.ms` (cancellable) |
| `load`        | yes | `{loaded, model, backend, ctx_size}`; loads with `params` = `{model, llamacpp_backend, ctx_size, llamacpp_args, merge_args, save_options, pinned}` (same shape as `POST /load`) |
| `unload`      | yes | `{}`; unloads `params.model` if set and loaded, else all |
| `chat`        | yes | the backend chat response verbatim (`timings`/`usage` included); `params` is a chat/completions request |

"Exclusive" ops require the model slot — see [Exclusivity](#exclusivity-and-queuing).
This is the current set, not a fixed one; ops for other modalities are additive —
see [Extending with new ops](#extending-with-new-ops-any-modality).

## Recipe: steps

```jsonc
{
  "id": "run_a",                 // unique within the job
  "op": "chat",
  "params": { "...": "${refs allowed}" },
  "when": "boolean expression",  // optional; skip the step if false
  "extract": { "a_tps": "timings.predicted_per_second" },  // output path -> context key
  "branch": [ { "when": "expr", "goto": "later_step_id" } ],
  "on_done": "later_step_id",    // where to go on success (default: next in list)
  "on_fail": "abort"             // abort (default) | continue | <later step id>
}
```

References (`${path}`) and the `when`/`branch` expression grammar are documented
in [job-expression-language.md](job-expression-language.md).

### Control flow (forward-only, no loops)

On success the next step is: the first `branch` case whose `when` is true, else
`on_done`, else the next step in list order (end when none). On failure `on_fail`
decides: `abort` fails the job, `continue` proceeds to the next step, or a step
id jumps there — **failure is a first-class branch**, which is how a `load` that
OOMs can jump to a smaller-config `load` (test-by-failure). Every branch /
`on_done` / `on_fail` target must reference a **later** step; the graph is
validated at creation, so execution is acyclic and always terminates.

## Lifecycle

States: `queued → running → { paused | interrupted | completed | failed }`.
A single worker runs one job at a time.

- **pause** — stop *after the current step*; the job goes `paused` at the next
  step boundary and (if exclusive) releases the slot. Pausing a still-`queued`
  job takes effect immediately: it is removed from the queue and persisted as
  `paused` before the call returns. Pause does **not** commit the job's model
  side-effects: models the job loaded stay resident for the resume, and the
  job keeps owning them (see below).
- **interrupt** — cancel the *current step now* (kills an in-flight `load`, and
  aborts an in-flight `chat` at the HTTP layer rather than waiting for the
  backend to reply); the step returns to `pending`, the job goes `interrupted`.
  Interrupting a still-`queued` job likewise dequeues and persists it as
  `interrupted` immediately. An interrupted exclusive job unloads only the
  model(s) it **owns**: ownership is tracked explicitly per job and is scoped
  to the *specific residency* the job created (the backend instance its own
  `load` op produced), not to the model name — a model the job's `unload` op
  released, or one whose job-created instance was replaced by an external
  client's later load of the same name, is no longer job-owned. Pre-job
  residents are protected separately: the first slot acquisition snapshots
  them with their pin state, and a pause/resume cycle re-baselines neither
  ownership nor the snapshot. Models loaded by external clients — including
  while the job is paused or interrupted and the gate is open — are never
  touched by the job's cleanup. One caveat: an external `load` that merely
  re-confirms the job's own still-resident instance (same model, matching
  options — the router deduplicates instead of creating a new instance) does
  not transfer ownership; that instance remains job-owned and is still cleaned
  up. Clients that need a model to outlive a job should load it before the job
  starts or after it ends. Reconcile unloads the owned instances
  (recording their configuration for resume, see below), unpins any the job
  pinned, and restores the recorded pin state of surviving pre-job models. A
  job that ends `completed` or `failed` commits its model side-effects instead
  (ownership and snapshot are discarded, resident models stay).
  **Guarantee scope:** a model that was resident before the job is preserved
  only if it is *still resident* at reconcile time. If a job `load` evicted it
  (loaded-model cap) or replaced it with different options, the reconcile does
  not reload it or restore its previous options — clients that need the exact
  prior state must reload it themselves.
- **resume** — `paused` continues at the next step; `interrupted` re-runs the
  pending step. Steps must be idempotent on re-run. Before an interrupted
  exclusive job re-runs, the engine **restores the job-owned models** that the
  interrupt cleanup unloaded (reloading each with its recorded options and pin
  state), so a step that depends on an earlier completed `load` — e.g. a chat
  interrupted mid-generation — finds its model again instead of failing. If an
  external client loaded the same model while the job was interrupted, the
  restore *adopts* that residency instead of replacing it: the external
  instance is left untouched (options and pin included) and stops being
  job-owned. Restore also survives a server restart: the in-memory captured
  state is gone after a crash, so the engine reconstructs the job's model
  state from its persisted record — the completed `load`/`unload` steps
  (parameters resolved against the persisted context, in execution order)
  describe exactly what should be resident, and resuming a recovered job
  reloads that before re-running the pending step. If a restore fails, the job
  still resumes and the dependent step fails with its normal `on_fail`
  semantics.
- **delete** — removes the job. Deleting an active job persists a deletion
  tombstone *before* the call returns, then interrupts it and defers the actual
  removal until the worker has finished cleanup (reconcile unload), so a deleted
  exclusive job never leaks a resident model. Deleting a `paused` or
  `interrupted` job takes the same tombstone-then-worker-cleanup path, so a
  model introduced before a pause is still unloaded. The tombstone makes the
  deletion durable: a crash between the acknowledgement and the final removal
  does not resurrect the job on restart, and a tombstoned job is already
  invisible to `GET`/list.
- **query** — the full job record (status, per-step state, context).

Chat interrupts are genuine aborts: the chat op passes its cancel flag through
`Router::chat_completion` to the backend `WrappedServer`, and `forward_request`
hands it to the HTTP client, which tears down the in-flight connection (curl
`XFERINFO` abort, fired at least once per second even when no bytes move). The
exclusive slot is therefore released promptly instead of being held until a slow
or stuck backend responds.

There is no rollback: an op commits only on completion, so an interrupted step
never took effect and "before the step" is automatic.

## Exclusivity and queuing

`load` / `unload` / `chat` require the **model slot**. While a job containing any
exclusive step runs, it holds a Router-level exclusive gate: all normal
inference and load traffic **queues** behind it until the job finishes or is
paused (pause is the escape hatch — it releases the slot so queued traffic
drains, and resume re-acquires). Acquiring the gate first *drains* in-flight
work: it waits for any active load and for every in-flight request on loaded
backends to finish, so an external chat that started just before the job cannot
overlap the exclusive session. The drain is cancellable — interrupting or
deleting the job while it waits behind a long-running request aborts the
acquisition and marks the job `interrupted` instead of blocking indefinitely.
Every model-touching Router path — inference, streaming, tokenize, slots,
pinning, load/unload — checks the same gate, and the idle-eviction/downsize
engine suspends itself (and abandons any already-marked eviction) while an
exclusive session is active, so background eviction cannot pull a model out
from under a job step. The gate is keyed by the worker thread, so the job's
own ops pass through while every other request waits. A job with only
read-only ops (e.g. `system_info`, `sleep`) never takes the gate. Read-only
status queries (model list, health, telemetry) are not gated.

## Persistence

Jobs persist to `<cache_dir>/jobs.json` (atomic write, cap 50, oldest terminal
evicted first). The cap is enforced at creation: when all 50 retained jobs are
still active or resumable (nothing `completed`/`failed` to evict), `POST jobs`
is rejected with `429` until a job is deleted or finishes. On startup a job left
`running`/`queued` by a crash is marked `interrupted` ("server restarted while
the job was active") but keeps its cursor, so it can be resumed from where it
stopped; tombstoned (deleted-while-active) jobs are dropped.

A job summary's `progress.completed` counts steps that no longer need work:
`completed`, `skipped`, and failed steps whose failure was *handled* by
`on_fail: continue` or a recovery branch — so a recovery job that completes
reports full progress even though one of its steps is marked `failed`.

## API

Registered under all four prefixes (`/api/v0`, `/api/v1`, `/v0`, `/v1`).

| method | path | purpose |
|--------|------|---------|
| POST   | `jobs` | create `{name, definition:{steps} \| steps, inputs}` → `202 {id}`; `400` on an invalid graph; `429` when the job store is full of non-evictable jobs |
| GET    | `jobs` | `{jobs:[summaries]}` |
| GET    | `jobs/{id}` | full record, or `404` |
| POST   | `jobs/{id}/pause` | `200` / `404` |
| POST   | `jobs/{id}/interrupt` | `200` / `404` |
| POST   | `jobs/{id}/resume` | `200` / `404` |
| DELETE | `jobs/{id}` | `200` / `404` |

## Example: a bench sweep

Two configs, each timed, then a branch on the measured throughput — the shape a
benchmark/AutoOpt client posts (it reads `context` afterward and synthesizes the
recommendation itself):

```jsonc
{ "name": "bench",
  "inputs": { "model": "Qwen3-0.6B", "backend": "vulkan" },
  "steps": [
    { "id": "u0", "op": "unload" },
    { "id": "load_a", "op": "load",
      "params": { "model": "${inputs.model}", "llamacpp_backend": "${inputs.backend}",
                  "ctx_size": 4096, "llamacpp_args": "" },
      "on_fail": "load_a_lo" },
    { "id": "run_a", "op": "chat",
      "params": { "model": "${inputs.model}", "messages": [{"role":"user","content":"hi"}],
                  "temperature": 0, "max_completion_tokens": 32 },
      "extract": { "a_tps": "timings.predicted_per_second" } },
    { "id": "u_a", "op": "unload" },
    { "id": "load_a_lo", "op": "load",
      "params": { "model": "${inputs.model}", "llamacpp_backend": "${inputs.backend}",
                  "ctx_size": 2048 }, "on_fail": "abort" },
    /* …second config, then a `decide` step branching on ${a_tps} >= ${b_tps}… */
  ] }
```

## Extending with new ops (any modality)

The engine is modality-agnostic — `chat` is only the first *inference* op wired
up. Ops are a pluggable registry, not a fixed set, so image generation, TTS,
transcription, or a 3D-mesh op are additive: no changes to the engine, graph,
context, or expression language.

An op is a handler in the op registry (`OpRegistry`):

```cpp
struct OpHandler {
    std::function<json(const json& params, const json& context, CancelFlag& cancel)> run;
    bool exclusive = false;   // true if it holds the model slot (see Exclusivity)
};
```

The signature assumes nothing about text or LLMs: `params` and the return value
are arbitrary JSON, and any op that loads a model sets `exclusive = true` (like
`load`/`unload`/`chat`). Because every step's output lands in the shared context,
later steps consume it via `${step_id.field}` references — that is the data-flow
a pipeline needs.

To add one, register a provider in `OpProviders` that forwards to the relevant
already-existing Router backend (`SdServer`, `KokoroServer`, `WhisperServer`, …)
and `register_op` it. A `text → LLM enhancer → image → 3D` pipeline is then just
three registered ops passing data through the context:

```jsonc
{ "steps": [
    { "id": "enhance", "op": "chat", "params": { "...": "..." },
      "extract": { "prompt": "choices.0.message.content" } },
    { "id": "image",   "op": "generate_image",
      "params": { "prompt": "${enhance.prompt}" },
      "extract": { "path": "data.0.path" } },
    { "id": "model3d", "op": "generate_3d",
      "params": { "image": "${image.path}" } }
  ] }
```
