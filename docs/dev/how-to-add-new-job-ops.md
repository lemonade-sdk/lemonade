# How to add a new op to the jobs endpoint

## Summary

To add an operation, define a handler, connect it to server/backend code through
a provider if needed, and register it. See the [operations table](job-system.md#operations)
for the registered names, outputs, and exclusive flags.

## Files involved

| File | Role |
|------|------|
| `src/cpp/include/lemon/jobs/job_ops.h` | Declares `OpProviders` (provider contract) and `OpRegistry` |
| `src/cpp/server/jobs/job_ops.cpp` | Implements `build_op_registry()` — the place where ops are **registered** |
| `src/cpp/server/services/job_ops.cpp` | `JobOps::build()` constructs `OpProviders` with lambdas that call into Router/backends |
| `src/cpp/include/lemon/jobs/job_graph.h` | Validates op names at job-creation time against the registry's known set |
| `src/cpp/include/lemon/jobs/job_types.h` | Defines `StepRecord`, `Job`, `JobError`, status enums |
| `test/server_jobs.py` | Integration tests for the jobs HTTP API |

## Step-by-step

### 1. Decide if the op needs a provider

If the op only uses data already available in the job `context` or is a pure
computation (like the built-in `sleep`), you can implement it inline inside
`build_op_registry()` without adding anything to `OpProviders`.

If the op needs to call into the server's Router, backends, model manager, etc.,
add a callable to `OpProviders` in `job_ops.h`:

```cpp
// job_ops.h
struct OpProviders {
    // ... existing providers ...
    std::function<json(const json& params, CancelFlag& cancel)> my_new_op;
};
```

### 2. Implement the provider lambda in `services/job_ops.cpp`

Inside `JobOps::build(ServerContext& ctx)`, add a lambda that captures the
`ServerContext` members it needs (`router`, `model_manager`, ...) and forwards to
the relevant Router/backend path:

```cpp
providers.my_new_op = [router](const lemon::jobs::json& params,
                               lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
    // 1. Validate params, throw JobError(status, message) on bad input
    // 2. Call router / backend / model_manager etc.
    // 3. Return arbitrary JSON
};
```

**Rules:**

- Throw an exception with a useful message for invalid parameters or backend
  failures. `JobError(int status, std::string message)` is available, but step
  execution catches `std::exception` and records only its message in the step's
  `error` field. Only the job-create path interprets `JobError::status`, choosing
  between HTTP 400 for an invalid graph and HTTP 429 for a full job store.
- Respect the `cancel` flag. Check `cancel.load()` at sensible cancellation points
  and throw an exception with the message `"interrupted"` if you detect it.
  ACE-Step checks the request token during polling and between its `/lm` and
  `/synth` stages.
- The return value is arbitrary JSON; it will be stored under
  `context[<step id>]` and is available to later steps via `${step_id.field}`.

### 3. Register the op in `build_op_registry()`

In `src/cpp/server/jobs/job_ops.cpp`, add a `reg.register_op(...)` call inside
`build_op_registry()`. Include `<stdexcept>` for the exception in this example:

```cpp
reg.register_op("my_new_op", {[providers](const json& params, const json& context, CancelFlag& cancel) -> json {
    if (!providers.my_new_op) throw std::runtime_error("my_new_op not available");
    return providers.my_new_op(params, cancel);
}, /* exclusive = */ false});
```

**Exclusive flag:** Set to `true` only if the op touches the model slot (loads,
unloads, or runs inference on a loaded model). Exclusive ops queue behind the
Router's exclusive gate; non-exclusive ops run concurrently with normal traffic.

### 4. (Optional) Add tests

In `test/server_jobs.py`, add a test method to `JobEngineTests` that posts a job
using the new op and asserts on the resulting `context` and step status.

Example pattern:

```python
def test_my_new_op(self):
    steps = [
        {"id": "a", "op": "my_new_op", "params": {"...": "..."}},
    ]
    job = self.create_job("my-new-op", steps)
    done = self.poll_status(job["id"], "completed")
    self.assertEqual(self.step_by_id(done, "a")["status"], "completed")
    self.assertIn("expected_key", done["context"]["a"])
```

## Op handler signature

```cpp
std::function<json(const json& params, const json& context, CancelFlag& cancel)> run;
```

- `params` — the step's `params` object with `${refs}` already resolved against
  the job context.
- `context` — a snapshot of the full job context *before* this step ran (so the
  op can read earlier step outputs without consuming them).
- `cancel` — atomic bool; set to `true` on interrupt/delete. The op should poll
  it and throw an exception with the message `"interrupted"` when detected.

## Data flow between steps

1. Step `a` runs and returns `{"tps": 42.0, "model": "x"}`.
2. The engine stores it at `context["a"]`.
3. If `extract: {"tps": "tps"}` is set, it also copies
   `output.tps` to `context["tps"]`.
4. Step `b` can reference `${a.tps}`, `${a.model}`, or `${tps}` in its params.

## Returning binary/media results

Ops that produce media (audio, GLB) buffer the backend bytes, base64-encode them,
and return them inline in the op output. Because the whole job context is
persisted to `jobs.json`, that output is capped by `kMaxJobMediaOutputBytes` in
`lemon/jobs/job_media_limits.h`; a result above the cap fails the step with an
error message. For audio ops, reuse `negotiate_speech_format` or
`negotiate_audio_generation_format` from `lemon/server/audio_formats.h` and write
the selected format back into the request. Supported-format lists and metadata
depend on model residency; see [media inputs and outputs](job-system.md#media-inputs-and-outputs)
for defaults and result fields. The sink must stop accepting data once `cancel`
is set or the result exceeds the cap. `media_op_result` in
`services/job_ops.cpp` handles this: its sink refuses writes once cancelled or
past the cap, and it binds `cancel` as the request cancel token so a backend's
blocking HTTP call to its subprocess (e.g. Trellis' multipart `/generate`) is
aborted too.
