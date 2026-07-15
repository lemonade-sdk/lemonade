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
  step boundary and (if exclusive) releases the slot.
- **interrupt** — cancel the *current step now* (kills an in-flight `load`); the
  step returns to `pending`, the job goes `interrupted`. An interrupted
  exclusive job unloads whatever it left resident before releasing the slot.
- **resume** — `paused` continues at the next step; `interrupted` re-runs the
  pending step. Steps must be idempotent on re-run.
- **delete** — removes the job (interrupting it first if running).
- **query** — the full job record (status, per-step state, context).

There is no rollback: an op commits only on completion, so an interrupted step
never took effect and "before the step" is automatic.

## Exclusivity and queuing

`load` / `unload` / `chat` require the **model slot**. While a job containing any
exclusive step runs, it holds a Router-level exclusive gate: all normal
inference and load traffic **queues** behind it until the job finishes or is
paused (pause is the escape hatch — it releases the slot so queued traffic
drains, and resume re-acquires). The gate is keyed by the worker thread, so the
job's own ops pass through while every other request waits. A job with only
read-only ops (e.g. `system_info`, `sleep`) never takes the gate.

## Persistence

Jobs persist to `<cache_dir>/jobs.json` (atomic write, cap 50, oldest terminal
evicted first). On startup a job left `running`/`queued` by a crash is marked
`interrupted` ("server restarted while the job was active") but keeps its cursor,
so it can be resumed from where it stopped.

## API

Registered under all four prefixes (`/api/v0`, `/api/v1`, `/v0`, `/v1`).

| method | path | purpose |
|--------|------|---------|
| POST   | `jobs` | create `{name, definition:{steps} \| steps, inputs}` → `202 {id}`; `400` on an invalid graph |
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

## Source

`src/cpp/include/lemon/jobs/` (`job_types.h`, `job_expr.h`, `job_graph.h`,
`job_ops.h`, `job_manager.h`) and `src/cpp/server/jobs/`. Tests:
`test/cpp/test_job_expr.cpp`, `test/cpp/test_job_graph.cpp`,
`test/server_jobs.py`.
