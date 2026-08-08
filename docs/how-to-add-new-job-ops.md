# How to add a new op to the jobs endpoint

## Summary

The jobs engine uses a **pluggable op registry**. Adding a new op is additive: you
declare a handler function, wire it to existing server/backend code via a provider
lambda, and register it. No changes to the job graph, expression language, HTTP
routes, or persistence layer are needed.

## Files involved

| File | Role |
|------|------|
| `src/cpp/include/lemon/jobs/job_ops.h` | Declares `OpProviders` (function-pointer contract) and `OpRegistry` |
| `src/cpp/server/jobs/job_ops.cpp` | Implements `build_op_registry()` — the place where ops are **registered** |
| `src/cpp/server/server.cpp` | Constructs `OpProviders` and passes lambdas that call into Router/backends |
| `src/cpp/include/lemon/jobs/job_graph.h` | Validates op names at job-creation time against the registry's known set |
| `src/cpp/include/lemon/jobs/job_types.h` | Defines `StepRecord`, `Job`, `JobError`, status enums |
| `test/server_jobs.py` | Integration tests for the jobs HTTP API |

## Step-by-step

### 1. Decide if the op needs a provider

If the op only uses data already available in the job `context` or is a pure
computation (like the built-in `sleep`), you can implement it inline inside
`build_op_registry()` without adding anything to `OpProviders`.

If the op needs to call into the server's Router, backends, model manager, etc.,
add a function pointer to `OpProviders` in `job_ops.h`:

```cpp
// job_ops.h
struct OpProviders {
    // ... existing providers ...
    std::function<json(const json& params, CancelFlag& cancel)> my_new_op;
};
```

### 2. Implement the provider lambda in `server.cpp`

Inside `Server::Server()`, locate the `lemon::jobs::OpProviders providers;` block
(~line 413). Add a lambda that captures `this` (and any other state you need) and
forwards to the relevant Router/backend path:

```cpp
providers.my_new_op = [this](const lemon::jobs::json& params,
                             lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
    // 1. Validate params, throw JobError(status, message) on bad input
    // 2. Call router_ / backend / model_manager_ etc.
    // 3. Return arbitrary JSON
};
```

**Rules:**
- Throw `lemon::jobs::JobError(int status, std::string message)` for client-facing
  failures (e.g. `400` for bad params, `404` for unknown model, `424` for backend
  failure, `499` for interrupt).
- Respect the `cancel` flag. Check `cancel.load()` at sensible cancellation points
  and throw `JobError(499, "interrupted")` if you detect it.
- The return value is arbitrary JSON; it will be stored under
  `context[<step id>]` and is available to later steps via `${step_id.field}`.

### 3. Register the op in `build_op_registry()`

In `src/cpp/server/jobs/job_ops.cpp`, add a `reg.register_op(...)` call inside
`build_op_registry()`:

```cpp
reg.register_op("my_new_op", {[providers](const json& params, const json& context, CancelFlag& cancel) -> json {
    if (!providers.my_new_op) throw JobError(501, "my_new_op not available");
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
  it and throw `JobError(499, "interrupted")` when detected.

## Data flow between steps

1. Step `a` runs and returns `{"tps": 42.0, "model": "x"}`.
2. The engine stores it at `context["a"]`.
3. If `extract: {"tps": "timings.predicted_per_second"}` is set, it also copies
   `output.timings.predicted_second` to `context["tps"]`.
4. Step `b` can reference `${a.tps}`, `${a.model}`, or `${tps}` in its params.

## Existing ops for reference

| Op | Exclusive | Provider location | Notes |
|----|-----------|-------------------|-------|
| `system_info` | no | inline in `job_ops.cpp` | Calls `SystemInfoCache` |
| `system_stats` | no | inline in `job_ops.cpp` | Calls CPU/GPU/VRAM metrics |
| `models` | no | inline in `job_ops.cpp` | Lists models or returns one by `params.id` |
| `sleep` | no | inline in `job_ops.cpp` | Cancellable `params.ms` sleep |
| `load` | yes | lambda in `server.cpp` | Calls `router_->load_model(...)` |
| `unload` | yes | lambda in `server.cpp` | Calls `router_->unload_model(...)` |
| `chat` | yes | lambda in `server.cpp` | Calls `router_->chat_completion(...)` |

## Constraints from project notes

- **Purely additive**: do not remove or change the signature of existing ops.
- **Static linkage**: new server helpers must use static linkage to avoid symbol
  conflicts.
- **Omni collections**: if your op touches model loading/unloading, ensure omni
  collections are skipped so the orchestrator retains its existing logic.
