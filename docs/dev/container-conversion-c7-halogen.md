# C7 Halogen conversion stop

C7 inspected stack `cbbeeb5270b19e06cfd9c150233655776392bd38` against integration base
`4f9bb9e3e768953ddc967c59a06710259a5eed60`. This engine was **not migrated**:
the remaining behavior below cannot be preserved by the current v2 contracts.
The assignment requires stopping an engine rather than dropping behavior.

Production definition lines added: **0**. No execution grant or owner digest,
active leaf, registration, model row, version pin, generated default/support
section, parity fixture or identity transition was added for this engine.
The source captures and CPU probes are preflight evidence, not migration parity.

## Pinned inventory

| Field | Stack source inventory, not production additions |
|---|---|
| Recipe / backend | `halogen` / `rocm` |
| Version key | `halogen.rocm` |
| Repository | `ghcr.io/peonist-ai/halogen-flash-server` |
| Pin | `latest@sha256:f4ee69cafd334f5fed1173e998550dea00505e0df2bb3cb90b60d7a7aa1332f4` |
| Registry rows with this recipe | 1 |
| Defaults | `{}` |
| Pin validation | **Unvalidated. No registry access was performed.** |

Reproduce each quotation with `git show cbbeeb5270b19e06cfd9c150233655776392bd38:<path>`.
Exact captures, including version/default files and all recipe model rows, are
in `.synagent/artifacts/container-v2/c7/source/halogen/`.

## Missing v2 builtin load-log binding

At the pinned source, `src/cpp/server/backends/halogen/halogen_server.cpp:193`
logs exactly:

```cpp
LOG(INFO, "Halogen") << "Starting " << model_name << " on port " << port_ << std::endl;
```

The assignment's wrapper-conversion guideline explicitly requires preserving
verbatim source log text through `BuiltinLoadLog`, including preparation timing.
The existing `halogen_hgn_v1` contract faithfully supplies exclusivity, context,
HGN bundle/extras, host checks and launch facts, but supplies no log binding.

`ConfiguredBackendEntry::make(shared_ptr<const ExecutableDefinition>)` in
`src/cpp/server/backends/configured/configured_v2_backend_ops.cpp` accepts no
`BuiltinLaunchCompatibility`. Its v2 constructor leaves `launch_compatibility`
default-initialized. `ConfiguredServer::load()` at
`src/cpp/server/backends/configured/configured_server.cpp:96-115` takes that v2
path and returns before the v1 `BuiltinLoadLog` handling at lines 116-125.
It supplies empty preparation callbacks to `DefinitionRunner::prepare_load`.
Registering an old-style hook in `installed_backend_hooks()` and
`installed_hook_manifest()` would not make this v2 branch call it.

The closed schema permits only `none` or `loading_and_options` for
`launch.load_log_policy`. The runner's latter policy logs `Loading model: ` and
`Per-model settings: `; neither is the source's port-bearing message.
The compiled C7 probe binds the actual C6b Halogen definition and verifies
`!entry->launch_compatibility.load_log.has_value()` with
`C7V2FactoryHasNoBuiltinLoadLog`. A proposed `halogen_starting` selection rejects
at `/merge/launch/load_log_policy`, checked by `C7NoHalogenStartingLogPolicy`.
These observations do not invalidate the existing C6b model/host contract.

A prerequisite must provide a narrowly authorized builtin logging path for v2
and preserve preparation timing, with a separately disclosed runner stage.
Adding an engine wrapper or changing the frozen policies is outside this unit.
C7 therefore stops Halogen rather than silently dropping its required log.
Logs are retained in `.synagent/artifacts/container-v2/c7/probe-run.log`.

The Gufo precedent also lacks a v2 builtin load-log binding. Copying that
omission would not satisfy this assignment's explicit verbatim-log guideline.
No Gufo source, existing policy or prior qualification fixture was changed.
