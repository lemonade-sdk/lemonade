# Adapter plane

The adapter plane is one in-tree backend, `adapter`, that starts an out-of-tree engine from a JSON spec. Any engine that serves an OpenAI-compatible HTTP API on a port can be described this way without a new C++ backend. This page documents what the plane does today and which parts of it would need a change in `lemond`.

It was built against Lemonade 2026.45 and 2026.46 and rebased onto `main`. It is offered as a blueprint for the community-backend design you are working on, to take in whole, in part, or only as a list of things we ran into. Reference specs for several engines are in [lemonade-engine-adapters](https://github.com/festeraeb/lemonade-engine-adapters).

- [Spec example](#spec-example)
- [Placeholders](#placeholders)
- [Environment allowlist](#environment-allowlist)
- [Device selection](#device-selection)
- [Readiness](#readiness)
- [Conformance levels](#conformance-levels)
- [Running the checks](#running-the-checks)
- [What the plane needs from lemond](#what-the-plane-needs-from-lemond)

## Spec example

A spec is `<name>.json` in `<config dir>/adapters/` (or `$LEMONADE_ADAPTERS_DIR`). A model selects it through its checkpoint, `"<name>::<model path>"`. The spec is read at load time, so changing an engine does not change Lemonade.

```json
{
  "spec_version": 1,
  "display_name": "llama.cpp server",
  "binary": "/opt/engines/llama.cpp/bin/llama-server",
  "args": [
    "-m", "{model_path}",
    "--host", "127.0.0.1",
    "--port", "{port}",
    "-ngl", "99",
    "-c", "4096"
  ],
  "env": {
    "CUDA_DEVICE_ORDER": "PCI_BUS_ID",
    "CUDA_VISIBLE_DEVICES": "{gpu_uuids}",
    "LD_LIBRARY_PATH": "/opt/engines/llama.cpp/lib"
  },
  "default_device_index": "0",
  "ready": {
    "path": "/health",
    "timeout_s": 600,
    "json_field": "status",
    "ready_values": ["ok"],
    "error_values": ["error"]
  },
  "identity": { "owned_by": "llamacpp" },
  "completion": true
}
```

| Field | Meaning |
|---|---|
| `spec_version` | Integer, currently 1. A higher version is refused (level cap 0). |
| `binary` | Absolute path, path relative to the spec folder, or a name on `PATH`. Required. |
| `binary_sha256` | Optional. When present, the file must match before launch. |
| `args` | Array of strings; each element stays one argv entry after filling. Required unless `args_template` is given. |
| `args_template` | One string split on whitespace. Accepted only at `spec_version` 1, kept for older specs. |
| `env` | Extra environment, subject to the [allowlist](#environment-allowlist). |
| `default_device_index` | Card used when the model sets no `adapter_device`. |
| `ready` | Readiness probe, see [Readiness](#readiness). |
| `identity.owned_by` | When set, `/v1/models` must report this owner or the engine is stopped. |
| `telemetry_path`, `memory_path`, `budget_path` | Engine routes for statistics, the memory report and the budget request. |
| `telemetry` | `{enabled, env, what}`: extra environment applied only when `enabled` is true or the user sets `adapter_telemetry=on`. Off by default. |

Unknown fields are ignored. A missing or wrong-typed required field makes `check_spec` cap the adapter at level 0 and `load` refuses with a message that names the problem.

There is no shell: the argv array is passed to the process directly.

## Placeholders

| Placeholder | Value |
|---|---|
| `{port}` | Port chosen by lemond. |
| `{model}`, `{model_path}` | The path after `::` in the checkpoint. |
| `{device}` | Index of the selected card, as decimal text. |
| `{gpu_uuids}` | UUID of the selected card; the index text when the card has none. |
| `{spec_dir}` | Folder holding the spec. |

A `{name}` token not in this set is an error that names it. `{ctx}` is accepted by the argv builder but `load()` does not supply a value for it yet, so a spec that uses it is refused today; write the context size into `args`. Braces that are not a `{identifier}` token, such as `"{}"`, stay as text.

## Environment allowlist

A spec starts a process as the lemond user, so `env` is limited to names starting with `CUDA_`, `HIP_`, `ROCR_`, `HSA_`, `GGML_`, `LLAMA_`, `OMP_`, `MKL_`, `VK_`, `NAUTIBURN_`, `STRATA_` or `LEMONADE_ENGINE_`, plus `LD_LIBRARY_PATH` and `TZ`. Anything else, including `LD_PRELOAD` and `PATH`, is refused with the variable named.

`LD_LIBRARY_PATH` entries must be absolute, contain no `..`, and lie inside the spec folder, the binary's folder, or, when the binary is in a `bin/` folder, that engine bundle's root. The refused entry is named.

The prefix list is ours and is easy to change; it lives in `adapter_spec.h`.

## Device selection

`adapter_device` (a model option) wins, then the spec's `default_device_index`, then `0`. The value is a decimal index or a GPU UUID, matched against the NVIDIA and AMD discrete GPU lists lemond already builds. An index is matched against the driver's index, not a list position. Both `{device}` and `{gpu_uuids}` are filled, so a spec can pass either form. An unknown index or UUID is an error that names it. When lemond finds no GPU list, the value is used as given.

## Readiness

After the process starts, lemond's existing `ready.path` check runs. If `ready.json_field` is set, the plane then polls it every 2 seconds until its value is in `ready_values`, stopping with an error on a value in `error_values` or after `timeout_s` (default 600).

`ready_state()` in `adapter_spec.cpp` classifies an engine's status answer into five states, using the optional lists in `ready`:

| State | When |
|---|---|
| Starting | No `json_field`-matching value yet, or a non-object answer. |
| Loading | Value in `loading_values`; `progress_field` is read as 0..1, or 1..100 as a percentage. |
| Ready | Value in `ready_values`, or any object answer when no `json_field` is set. |
| Warm | Value in `warm_values`. |
| Error | Value in `error_values`. |

`load()` does not call `ready_state()` yet; it uses the `ready_values` / `error_values` loop above. The classifier is tested and ready to be wired in once there is somewhere to show Loading and progress.

## Conformance levels

`tools/adapter_conformance.py` grades one adapter against a running lemond and reports the highest level it earns. Failing a level is a downgrade, not a removal: level 0 needs no test because the engine can always be run without control, for example as a provider.

| Level | Proves |
|---|---|
| L0 | Nothing tested. Engine runs without lemond controlling its process. |
| L1 lifecycle | Load starts the engine, it answers a chat request, and unload frees its GPU memory, checked on the device (`all-smi`, falling back to `nvidia-smi`), not from the engine's own report. |
| L2 eviction | With `max_models` for llm set to 1, loading a second model evicts this one and frees its memory. Needs `--other`. |
| L3 options | The per-model `adapter_device` option is honoured: memory appears only on the requested card. Needs `--device`. |
| L4 telemetry | `/api/v1/stats` carries the engine's statistics (`adapter_stats`). |

`reference_check()` in the same file compares the SHA-256 of a greedy answer (prompt, token count, expected digest) to detect a wrong build or quantisation. It has unit tests and is not part of the L1 to L4 run.

L4 cannot pass on a stock `lemond` today, for the reason in the next section.

## Running the checks

All commands run from the repository root. `BUILD_DIR` names a configured build; the unit tests take the `nlohmann/json` header from its `_deps` folder (or set `JSON_INCLUDE`).

```bash
# Unit tests for the pure spec module, one job at a time (ap2 ap3 ap4 ap5 ap6 ap7 ap8 ap10 ap11)
BUILD_DIR=build bash test/cpp/adapter_spec/run.sh ap2

# Conformance tool unit tests
python3 -m unittest test/test_adapter_conformance_gpu.py test/test_adapter_conformance_reference.py

# Live test: builds a throwaway tree, starts the given lemond on ADAPTER_LIVE_PORT (default 13807)
# and loads a recording fake engine
ADAPTER_LIVE_PORT=14300 python3 test/cpp/adapter_spec/live/spec_wiring_live.py build/lemond

# Do the Lemonade declarations the backend depends on still match?
python3 tools/adapter_hook_map.py --check
python3 tools/adapter_hook_check.py

# Grade a real adapter against a running lemond
python3 tools/adapter_conformance.py --lemond http://127.0.0.1:13305 --model <model> --other <model2> --device 0
```

The live test currently reports 12 of 13 passing. The failing case, "stats carry the engine's memory report", is the missing router hook described below.

`src/cpp/server/backends/adapter/hook_map.json` lists the Lemonade declarations the backend uses (descriptor, `ServerCommand`, `WrappedServer` methods and so on) with a fingerprint of each. `adapter_hook_map.py --check` names any that changed; `--update` rewrites the fingerprints after the backend has been adapted. `test/cpp/adapter_spec/hook_map.json` is a second record of the same dependencies as file, symbol and signature; `adapter_hook_check.py` re-greps it.

## What the plane needs from lemond

R1 to R17 are the requests we drew up while building the plane. The plane works on stock Lemonade without any of them; the right-hand column says what it does today. Items marked "in the plane" are already implemented there and need nothing from lemond. Items marked "lemond" need a change on your side, and some are policy that is yours to set.

| # | Request | Why | Today without it | Status |
|---|---|---|---|---|
| R1 | Spec command as an argv array with typed placeholders, no shell | Removes shell injection from community specs | Only `args` arrays are accepted; unknown placeholders refused before launch | In the plane |
| R2 | Optional `binary_sha256` checked before launch | A catalogue can pin what runs | Checked in `load()` when present | In the plane |
| R3 | Spec sources limited to signed catalogues or a local folder; env by allowlist | A spec starts a process | Local folder only; allowlist enforced | Allowlist in the plane; signed catalogues are yours to decide |
| R4 | Port allocated by lemond | Avoids collisions | `{port}` comes from lemond's `choose_port()` | In the plane |
| R5 | Probe states starting, loading (progress), ready, warm; timeout per spec | Large models load for minutes; honest UI | Classifier written and tested; `load()` uses ready/error values and the spec timeout | Classifier in the plane; showing the states needs lemond |
| R6 | Health as a probe with a latency budget | A hung engine should leave rotation | lemond's existing health flag | Not built |
| R7 | Per-level conformance with downgrade | Keep the control the adapter earned | `adapter_conformance.py` reports the level; lemond does not act on it at runtime | Tool in the plane; runtime use needs lemond |
| R8 | Unload verified against the device | One wrong adapter must not break eviction | `unload()` checks `nvidia-smi` / `rocm-smi` for the engine's process tree and kills a leftover holder; the tool checks the device too | In the plane (Linux) |
| R9 | Memory report per device and tier | MoE engines span VRAM, host RAM, remote memory | Parser and `adapter_memory` stats object written and tested; reported through `get_additional_telemetry()`, which nothing in lemond calls | **Needs lemond**: a router hook that calls `WrappedServer::get_additional_telemetry()` |
| R10 | Budget verb: shrink to N MiB on device X | Serve slower instead of evicting | Request builder and reply parser tested; no caller, no endpoint | **Needs lemond**: same hook, plus a verb in the eviction path |
| R11 | Device by UUID | Index order changes | Index or UUID accepted; both placeholders filled | In the plane |
| R12 | Metrics passthrough under the engine id | Operators see engine metrics | `telemetry_path` is read; not proxied as a route | Not built |
| R13 | Engine may own helper processes, including remote | Some engines span hosts | Engine is one process to lemond | Not built; invisible to lemond |
| R14 | Opt-in telemetry declared in the spec | Consent declared, not hidden | `telemetry` object and `adapter_telemetry` option; off by default | In the plane; showing it in the UI is yours |
| R15 | Greedy reference check in conformance | Detect broken build or quantisation | `reference_check()` with tests; not in the L1 to L4 run | In the plane |
| R16 | Versioned spec, unknown fields ignored, missing required fields give L0 | Specs outlive daemon versions | `check_spec()` | In the plane |
| R17 | Hook map with fingerprints and a CI check | Tells the author what moved | `hook_map.json`, `adapter_hook_map.py`, `adapter_hook_check.py` | In the plane |

The one change that unlocks the most is the router hook. At present `get_additional_telemetry()` is declared on `WrappedServer` and implemented by the adapter, but the router only uses the URL and parser variants for per-request spans, and `/api/v1/stats` returns lemond's own aggregates. A hook that lets the router call it, pinning a loaded engine for the duration of the short call, would deliver R9 (memory in stats) and give R10 a place to land, and it would let the conformance L4 check pass. That is the reason the live test case above fails.

If a different shape suits Lemonade better, for example a manifest-declared metrics route instead of a method call, the plane's parsers and tests carry over unchanged.
