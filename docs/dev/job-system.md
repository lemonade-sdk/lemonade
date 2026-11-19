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
| `image_generations` | yes | backend image response with `data[i].b64_json` and a top-level decoded-byte `size`; `params` is an images/generations request |
| `image_edits` | yes | image response as above; accepts base64 `image` and optional `mask` |
| `image_variations` | yes | image response as above; accepts base64 `image` |
| `audio_speech` | yes | `{data, mime_type, size, response_format}` with base64 audio and optional format `headers`; `params` is an audio/speech request |
| `audio_generations` | yes | base64 audio result as above; `params` is an audio/generations request |
| `model_3d_generations` | yes | `{data, mime_type, size, response_format}` with base64 GLB in `data`; `params` includes `model` and base64 `image` |

"Exclusive" ops require the model slot — see [Exclusivity](#exclusivity-and-queuing).
For the implementation walkthrough, see [How to add a new op](how-to-add-new-job-ops.md).

## Media inputs and outputs

Job image inputs use `image` and `mask`, each a bare base64 string or a `data:`
URI. A later step can reuse an earlier image with `"image": "${a.data.0.b64_json}"`.
Image edits and variations map these fields to `image_data` and `mask_data`,
strip data URI prefixes, and supply missing filename and image content type
metadata. Explicit backend fields take precedence. `image_files` passes through
untouched. 3D generation uses `image` and also accepts `image_data` when `image`
is absent.

Image results carry base64 image bytes in `data[i].b64_json`; their top-level
`size` is the total decoded image size in bytes. Audio and 3D results carry
base64 bytes in `data`, with `size`, `mime_type`, and `response_format`. A 3D
result uses `mime_type: "model/gltf-binary"` and `response_format: "glb"`.

Audio jobs drop `stream` and `stream_format` because a job always buffers its
result. Speech jobs also drop an empty `voice`, like the HTTP speech route.
When the model is resident and supplies a supported-format list, audio ops
negotiate `response_format` against that list. With no resident model, the list
is empty: absent an explicit format, speech defaults to `mp3` and audio
generation to `wav`. An explicit known format can also pass without a backend
support check when the list is empty. The recorded `mime_type` reflects this
selection, which may have used a default rather than backend capabilities.
Load the model in an earlier step to negotiate against its supported list.

PCM outputs use the backend's content type when available and preserve its
format headers in the result's `headers` object (for example, `X-MOSS-Sample-Rate` and
`X-MOSS-Channels`). Results omit `headers` when the backend supplies none.

Each media result has an 8 MiB limit. Image results count decoded bytes across
all `b64_json` images and gain a `size` field when they contain image bytes.
A job has a 16 MiB media budget. The budget counts media retained in step
outputs plus the payload bytes of context copies extracted from a media
result. The job tracks these copies per context key across steps and recounts
the budget for each step, adjusting the size when a key is replaced and dropping
an entry when a step's own output overwrites that context key. Extracts
from non-media steps are unrestricted; overwriting a key removes its previous
media-copy size from the budget. The per-result limit rejects the result
with an error message before the engine stores it.
The constants and accounting helpers live in `lemon/jobs/job_media_limits.h`.

A backend failure sent as an SSE error event fails the step with the backend's
error message, including when media bytes arrived before the error.
Once the sum passes the limit, the job fails that step
with "would exceed the per-job media store limit",
subject to the step's `on_fail` policy.
Speech/3D byte streams pass the request cancellation into the HTTP transfer, so
an interrupt during backend silence stops the transfer instead of holding the
exclusive slot.
ACE-Step passes the request token into its submit, poll and result HTTP calls, so
an interrupt aborts an in-flight backend call instead of waiting for its timeout.

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

### Step errors

If an operation throws, the engine records the exception message in the step's
`error` field and applies `on_fail`. It does not expose `JobError::status` or a
step error code. Only the job-create path interprets that status, choosing
between HTTP 400 for an invalid graph and HTTP 429 for a full job store.

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

The operations marked exclusive in the [operations table](#operations) require
the **model slot**. While a job containing any exclusive step runs, it holds a Router-level exclusive gate: all normal
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

Jobs persist to `<config_dir>/jobs.json` (atomic write, cap 50, oldest terminal
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

## Example: text to image to 3D

With `Qwen3-4B-Instruct-2507-GGUF`, `Flux-2-Klein-4B`, and `TRELLIS-3D` downloaded and their
backends installed, post this body to `POST /api/v1/jobs`. The inference ops load
their models on demand. Read `GET /api/v1/jobs/{id}` until the job completes.

```json
{
  "name": "text-to-3d",
  "inputs": {
    "text_model": "Qwen3-4B-Instruct-2507-GGUF",
    "image_model": "Flux-2-Klein-4B",
    "model_3d": "TRELLIS-3D"
  },
  "steps": [
    {
      "id": "enhance",
      "op": "chat",
      "params": {
        "model": "${inputs.text_model}",
        "messages": [
          {"role": "user", "content": "Write a short image prompt for a toy robot, isolated on a white background. Return only the prompt."}
        ],
        "max_completion_tokens": 128,
        "stream": false
      }
    },
    {
      "id": "image",
      "op": "image_generations",
      "params": {
        "model": "${inputs.image_model}",
        "prompt": "${enhance.choices.0.message.content}",
        "n": 1,
        "response_format": "b64_json"
      }
    },
    {
      "id": "model3d",
      "op": "model_3d_generations",
      "params": {
        "model": "${inputs.model_3d}",
        "image": "${image.data.0.b64_json}",
        "response_format": "glb"
      }
    }
  ]
}
```

The image step returns `data[0].b64_json` and a top-level `size`; it does not
return a file path. The 3D step consumes those base64 image bytes and returns
base64 GLB in `context.model3d.data`, with `mime_type: "model/gltf-binary"`,
`response_format: "glb"`, and a decoded-byte `size`. Decode that `data` value
to save a `.glb` file. Both results must fit the media limits described above.
