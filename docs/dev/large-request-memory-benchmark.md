# Large-request memory benchmark

This manual Linux benchmark reproduces `lemond` RSS retention after repeated
multi-megabyte HTTP requests and separates the two changes in the associated
fix. It is intentionally not a normal CI gate: process RSS varies by libc,
kernel, host load, and allocator history.

## What the allocator setting changes

Unless an operator override is present, `lemond` attempts to fix glibc's
`M_MMAP_THRESHOLD` at 1 MiB during startup. This is a process-wide allocator
policy, not an HTTP-only setting. Eligible allocations throughout `lemond` may
become mmap-backed, including large registry, cache, telemetry, and JSON
allocations. The threshold does not guarantee that every allocation above
1 MiB uses mmap; glibc can reuse suitable free blocks and is also subject to its
mmap limits.

The call happens before telemetry initialization and `Server` construction,
both of which start worker threads, because glibc documents `mallopt()` as
MT-Unsafe. The setting is a tuning, not a correctness requirement: if
`mallopt()` reports failure, `lemond` logs a warning and continues. Operators
can replace the built-in 1 MiB threshold without rebuilding by setting either
`MALLOC_MMAP_THRESHOLD_` or `glibc.malloc.mmap_threshold` in `GLIBC_TUNABLES`;
`lemond` does not overwrite either value. Non-glibc builds do not compile or
call this code.

This targets glibc's dynamically increasing mmap threshold directly.
`MALLOC_ARENA_MAX` limits arena proliferation but does not prevent that
threshold from increasing, while `malloc_trim(0)` only releases releasable
arena pages and would add process-wide work after selected requests. A static
threshold gives deterministic release behaviour for the affected allocations,
while the environment overrides retain an operator-controlled escape hatch.

The second change avoids creating a serialized copy of parsed JSON in
non-streaming chat, completions, and responses handlers. Streaming handlers
still create the string immediately before installing their SSE provider,
which retains it by value.

## Four attribution variants

Build all variants from the same baseline commit and with the same Release
toolchain:

1. `baseline`: neither change;
2. `serialization-only`: lazy non-streaming serialization only;
3. `allocator-only`: startup allocator configuration only;
4. `combined`: both changes, matching the proposed patch.

The checked-in runner records every binary path, SHA-256 digest and reported
version so the result can be tied back to exact artifacts. Its default
backend-free route is `/api/v1/test`. Because that route deliberately ignores
the JSON body, it isolates allocator behaviour: baseline and
serialization-only should match, as should allocator-only and combined. Use a
mock or real inference route to measure serialization and end-to-end effects.

Both modes authenticate requests with `LEMONADE_ADMIN_API_KEY` when it is set,
falling back to `LEMONADE_API_KEY`. Stress-mode subprocesses inherit the same
environment, and the selected credential is also used for their internal
shutdown request. Credential values are never written to the result JSON.

## Backend-free RSS run

Run three fresh-process repetitions per variant:

```bash
python3 tools/benchmark_large_request_memory.py stress \
  --variant baseline=/path/to/baseline/lemond \
  --variant serialization-only=/path/to/serialization-only/lemond \
  --variant allocator-only=/path/to/allocator-only/lemond \
  --variant combined=/path/to/combined/lemond \
  --source-commit baseline=<full-baseline-sha> \
  --source-commit serialization-only=<full-serialization-only-sha> \
  --source-commit allocator-only=<full-allocator-only-sha> \
  --source-commit combined=<full-combined-sha> \
  --requests 170 \
  --body-mib 3 \
  --concurrency 1 \
  --repetitions 3 \
  --settle-seconds 1 \
  --output large-request-memory-sequential.json
```

There is no large-request warm-up by default. Each repetition starts a new
`lemond` with an empty cache, waits for `/live`, records `VmRSS` and `VmData`
from `/proc/PID/status`, sends the measured batch with one fresh TCP connection
per request, waits one second, and samples memory again. The JSON contains all
raw repetitions plus the median and range for memory, elapsed time, and request
rate.

For a deliberately allocator-heavy concurrent throughput boundary, increase
the batch and concurrency while keeping fresh processes and repeated runs:

```bash
python3 tools/benchmark_large_request_memory.py stress \
  --variant baseline=/path/to/baseline/lemond \
  --variant serialization-only=/path/to/serialization-only/lemond \
  --variant allocator-only=/path/to/allocator-only/lemond \
  --variant combined=/path/to/combined/lemond \
  --source-commit baseline=<full-baseline-sha> \
  --source-commit serialization-only=<full-serialization-only-sha> \
  --source-commit allocator-only=<full-allocator-only-sha> \
  --source-commit combined=<full-combined-sha> \
  --requests 8192 \
  --body-mib 3 \
  --concurrency 32 \
  --repetitions 3 \
  --output large-request-memory-concurrent.json
```

This no-op route is a worst-case allocator benchmark, not inference
throughput. Report its cost rather than extrapolating it directly to model
latency.

## Real-backend validation

`real-backend` mode measures already-running Lemonade targets. Each target must
have the named model loaded with the same context, backend, model artifact, and
runtime options. The PID is the corresponding `lemond`, not its backend child.

```bash
python3 tools/benchmark_large_request_memory.py real-backend \
  --target baseline=http://127.0.0.1:19001,12345 \
  --target combined=http://127.0.0.1:19002,12346 \
  --source-commit baseline=<full-baseline-sha> \
  --source-commit combined=<full-combined-sha> \
  --model Qwen3.5-122B-A10B-GGUF \
  --prompt-file /path/to/fixed-long-context-prompt.txt \
  --max-tokens 128 \
  --repetitions 3 \
  --output large-request-memory-real-backend.json
```

The runner records request-body bytes, wall time, prompt and generation token
rates reported by the backend, and `lemond` RSS/VmData change. Leave
`--cache-prompt` disabled for independent full-prefill repetitions; enable it
only when the test is explicitly intended to measure maximum-depth cached
generation. By default, one unmeasured one-token request warms each target
before the repeated long-context samples; set `--warmup-requests 0` to include
cold-start effects instead. With multiple targets, odd repetitions use the
listed order and even repetitions reverse it to reduce fixed-order bias.
For release validation, include a longer generation and a non-ROCm backend when
the target hardware supports one.

For every published result, preserve the JSON and report:

- baseline and patched source commits;
- OS, kernel, glibc, compiler and hardware;
- endpoint and exact payload construction;
- request count, concurrency, warm-up, settle delay and repetition count;
- median and full range rather than excessive single-run precision;
- model artifact, context, backend/runtime and cache policy for real inference.
