# Converting a container backend to JSON

Gufo is the first production resource conversion. Its definition is
[`gufo.json`](../../src/cpp/resources/backends/gufo.json); it uses the existing
`ConfiguredServer` and `ContainerProcess`, with no Gufo wrapper. The leaf is
**deliberately inactive**: it has no active builtin leaf policy, executable seed,
published descriptor, CLI option or advertised support row. Its support row is
retained in the definition. The three registry entries and the image pin are
staged in the production resources; attempts to execute this recipe remain
unavailable. The dormant defaults are `{"args":""}`; activation must generate
that section and the advertised support tables with the normal generator.

| Pin provenance | Value |
|---|---|
| Source | Stack `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78`, `src/cpp/resources/backend_versions.json`, `gufo.rocm` |
| Repository | `docker.io/kyuz0/amd-strix-halo-toolboxes` |
| Copied pin | `rocm-10.0-gufo@sha256:67aa0b040993fedfd4e300242c5cb48f8add5874454a7486993668e74185e7a9` |
| Registry validation here | **Not performed. This pin was copied from the stack and has NOT been registry-validated here.** |

The existing runtime validates the digest syntax and constructs the immutable
repository/digest identity. That does not establish registry existence, image
contents or engine compatibility. Do not substitute a guessed digest.

## Repeatable procedure

1. Pin the source revision. Capture the descriptor, wrapper, process adapter,
   container manager, version file, model registry and defaults. Quote every
   production field before authoring the definition. Inventory wrapper names,
   source-text tests, opt-in variables and committed fixtures before removing any
   wrapper. No wrapper is removed by C4b: Gufo never existed on this branch.
2. Map metadata, policies, artifact inputs, launch records and protocols using
   the table below. Check expressibility first. A missing trusted capability is
   a prerequisite format change; it is not permission to omit the behavior.
3. Add an envelope with a new builtin identity to `resources/backends`. Reuse
   typed hooks only where their contract matches the original behavior. Keep
   request transformations absent when the original forwarded unchanged bodies.
   Keep advertised capability bits separate from callable compatibility APIs.
4. Copy the reviewed version-file pin and model inventory exactly. Record pin
   provenance and validation status in data documentation. Stage defaults for an
   inactive leaf; generate defaults and support tables when registering it.
5. Author an independent, fixed `BuiltinContainerExecutionOwner`. Seal the
   builtin key, recipe/backend, repository and pin source, command mode, devices,
   capabilities, artifact sources, mount layout, environment, address,
   confinement and teardown. Store its literal digest. Never derive authority
   from the definition being authorized. Hook removal must preserve this owner.
6. Freeze a parity fixture independently of the implementation. Compare actual
   launch plans against argv, environment, read-only mounts, readiness, timeout,
   output filtering and the complete buffered/streaming protocol surface. Use
   colliding basenames and every speculative mode. Execute via a fake host and
   fake client, with loopback HTTP and ephemeral ports. Never spawn a container
   runtime during CPU qualification.
7. Add a new, exact identity-transition stage. Replay all older manifests with
   their original comparators and source trees. Derive the new builtin context,
   admission, execution, catalog and ETags independently; disclose every record
   and require complete snapshot `cmp`. Preserve v1 sources and both existing
   v2 policies byte-for-byte. Do not regenerate historical fixtures or add
   comparator exceptions.
8. Configure/build, run the full cpp-ci census and all prior suites, HTTP/CLI
   and frozen CLI/REPL, renderer contracts, generator check, literal/splitter
   checks and isolated server smoke. Compile mutations of runtime/grant/pin
   controls and mutate definition data. Count only a named failing assertion,
   with normal exit 1, as a caught mutation; crashes and build failures fail the
   campaign. Retain the unmodified passing control.
9. Keep the leaf inactive until its activation gates are complete. A subsequent
   activation must add an audited fixed leaf/hook policy, registration and
   capability mask, regenerate descriptor-derived defaults/docs, and disclose
   its own identity stage. It also needs registry, actual container/network,
   GPU selection, inference, cancellation/cleanup and packaging qualification.
   These real-runtime gates are outside C4b's CPU-only assignment.

## Exact Gufo source mapping

All source locations below are at the pinned stack revision. `gufo.h` means
`src/cpp/include/lemon/backends/gufo/gufo.h`; `gufo_server.cpp` means
`src/cpp/server/backends/gufo/gufo_server.cpp`.

| Stack field and quoted value | JSON record or trusted owner |
|---|---|
| `gufo.h:33-37`: `"gufo"`, `"Gufo"`, `"gufo"`, `DEVICE_GPU` | Envelope `id`, `recipe`; `metadata.display_name`, `binary`, `default_device:"gpu"` |
| `gufo.h:38-41`: `SlotPolicy::Standard`, `false`, `true`, `false` | `metadata.slot_policy:"standard"`, `selectable_backend`, `uses_ctx_size`, `dynamic_models` |
| `gufo.h:43-44`: `"gufo_args"`, `"--gufo-args"`, `""`, `"ARGS"`, `"Custom arguments to pass to gufo serve llm"`, `"Gufo Options"` | `metadata.options[id=args]` fields `name`, `cli_flag`, `default_value`, `type_name`, `help`, `group`; string contract |
| `gufo.h:47`: `"rocm"`, `BackendTier::Guest`, `BackendFormat::Container`, `{"linux"}`, `{{"amd_gpu", {"gfx1151"}}}`, `"AMD Strix Halo"` | `metadata.support[id=rocm]`: backend, guest tier, container format, Linux support, architecture constraints, device summary |
| `gufo.h:49-53`: `{"chat"}`, `{"main"}`, `{}`, `"Gufo"`, `{}` | `supported_modes`, `required_checkpoints`, `default_capabilities`, `web_display_name`, `rocm_channels` |
| `gufo.h:54-62`: `true`, `true`, `VersionPolicy::Exact`, `false`, `true`, `{}`, `{}`, `json::object()`, `false` | Metrics, CWSR-fix requirement, exact version policy, self-managed downloads false, takes-args true, empty arg/bin variants and config-extra, streaming-from-storage false |
| `gufo.h:65-67`: `"docker.io/kyuz0/amd-strix-halo-toolboxes"`, `{"/dev/dri", "/dev/kfd"}`, `{}` | `containers.rocm.repository`, `devices`, empty `cap_add` |
| `gufo_server.cpp:24,121,153-154`: `kBackend = "rocm"`, `install_backend`, `get_backend_image` | `launch.backend.fixed`, `install.strategy:"oci"`, pin source `backend_versions` / recipe `gufo` / backend `rocm` |
| `gufo_server.cpp:39-57`: `llamacpp::ops()->populate_metadata`, `resolve_checkpoint_path`, `find_imported_checkpoint`, `validate_registration_checkpoint`, `validate_checkpoint_file` | Existing `llamacpp.artifacts.v1` typed artifact hook on main, mmproj and draft; GGUF operations are inherited from that shared implementation |
| `gufo_server.cpp:123-128`: `resolved_path()`, `resolved_path("mmproj")`, `resolved_path("draft")`, `"GGUF file not found for checkpoint: "` | `artifacts.main`, optional `mmproj` and `draft`; exact main-path error expression |
| `gufo_server.cpp:130,132`: `get_option("ctx_size")`, `DEVICE_GPU` | Common `/options/ctx_size` fact; descriptor GPU default and fixed ROCm selection; no invented per-engine context default |
| `gufo_server.cpp:74-80,136`: `"serve", "llm"`, `"--model"`, `"--served-model-name"`, `"--context"`, `"--host", "0.0.0.0"`, `"--port"` | `launch.execution.cases[id=rocm-container].program` image command `gufo`; ordered `argv` records `serve`, `model`, `name`, `context`, `host`, `port` |
| `gufo_server.cpp:82-84`: `"--mmproj", mmproj_path` | Conditional projector argv and mount, enabled only for nonempty artifact |
| `gufo_server.cpp:28-34,85-91`: `dflash2/--dflash-model`, `mtp/--mtp-model`, `dspark/--dspark-model` | `/model/speculative_mode`, `argv` mode/flag records and keyed `launch.checks[id=draft-mode]`; only validate when draft exists |
| `gufo_server.cpp:88-89`: `"gufo: a draft model needs gufo_speculative set to dflash2, mtp or dspark, not '" + speculative + "'"` | Exact `launch.checks[id=draft-mode].error` concatenation |
| `gufo.h:15-26`, `gufo_server.cpp:94-101`: reserved flags `-m --model -c --context --mmproj --served-model-name --speculative --dflash-model --dspark-model --mtp-model -i --host -p --port --api-key`; `"Invalid custom gufo arguments:\n"` | `launch.custom_args.reserved`, `error_prefix`, option `args`; parse and append validated user arguments |
| `gufo_server.cpp:140-144`: `gguf_files(path)` in main/projector/draft order; `server_process.cpp:133-155`: `kModelsDir + "/" + file_name`, numbered collision directories starting at `2` | Inventory mounts with `expand:"gguf_shards"`, root `/mnt/models`, `layout:"basename_numbered"`; whole-value argv/env substitutions |
| `server_process.cpp:158-162`: `hip_visible_device(recipe_, backend_)`, `"HIP_VISIBLE_DEVICES"`; `container_manager.cpp:421-422`: `"HOME=/tmp"` | Platform-owned compatible HIP selection; runtime-owned HOME. Neither is an authored environment exception. Host artifacts stay on persistent disk |
| `server_process.cpp:177`: `spawn(..., "", {})` | Inherited environment and working directory; no native executable staging |
| `gufo_server.cpp:148-155`: `"/ready"`, `ProcessOutput{inherit_output, true}`, `get_default_timeout()` | Global readiness timeout, `/ready` path, output inheritance at info/debug/trace and health-log filtering; standard watchdog on `/ready` |
| `gufo_server.cpp:163-172`: `forward_request("/v1/chat/completions", request)`, `"/v1/completions"`, `"/v1/responses"` | Six ordinary buffered/streaming operation records, identity requests, existing JSON/SSE codecs; no llama request/response rewrites |
| `container_manager.cpp:383-386,538,576-585`: `--cap-drop=all`, `--security-opt=no-new-privileges`, `--pull=never`, internal network, stop/remove/network-remove | Fixed isolation policy, private internal networking, loopback publishing, stop/remove/reap and ten-second grace |
| `server_models.json`: three `gufo` rows and `gufo_speculative`; `defaults.json`: `"gufo":{"args":""}` | Exact production registry entries; frozen dormant defaults in parity fixture until registration is activated |

The reviewed owner is `reviewed_gufo_execution_owner()` in
`backend_builtin_authorization.cpp`. Its fixed semantics and digest are independent
of both the authored source and hook grants. It is intentionally absent from
`embedded_v2_policies().execution_owners`. The unchanged active policies authorize
only llama.cpp and HRX. The parity suite installs this exact dormant owner into
an isolated test catalog, so it tests the production data without publishing it.

The common parity format is
[`gufo.production-parity.json`](../../test/fixtures/backend-v2/gufo.production-parity.json).
It pins source provenance, pin-validation status, activation status, owner digest,
argv, speculative mappings, colliding mounts, device set, readiness, six protocol
records, capability mask and complete model inventory. The C4b disclosure is
[`gufo.catalog-transition.json`](../../test/fixtures/backend-v2/gufo.catalog-transition.json).
Adding an inactive embedded identity and a version-file key still changes the
builtin context and propagated admission/execution/catalog identities and ETags.
The live snapshot records the dormant source explicitly in `inactive_definitions`;
its exact union with advertised definitions must equal the complete embedded
inventory. No source is omitted from the comparison. The eight-definition v1
replay uses its original explicit resource list and unchanged C++ comparator;
the separate resource/snapshot gates cover all ten current sources. Raw C4a/C4b
legacy snapshots are not equal.

## Remaining engines: planning estimates

The [C5 source preflight](container-conversion-c5-preflight.md) found concrete
prerequisites for all five remaining engines. No C5 leaf was staged. The
[C6 prerequisite contracts](container-conversion-c6-contracts.md) subsequently
close all four prerequisite contracts, including Halogen and DS4. Production
migrations and their independent parity campaigns remain the next unit. The estimates
below assume those prerequisites are resolved; they are not a finding that the
current format can express every engine.

These estimates cover source derivation, definition/grant/inventory, CPU parity,
mutations and identity disclosure, assuming the current format is sufficient.
They exclude registry access, real inference/hardware and packaging validation.
They are estimates, not measured implementation times. Inspect the named stack
sources again before starting; any missing contract becomes a prerequisite unit.

| Engine | Source surface to inspect at the pinned stack | Estimated effort | Main cost/risk |
|---|---|---|---|
| rocmfpx | `rocmfpx/rocmfpx.h`, `rocmfpx_server.cpp` | 1-2 engineering days | Custom argv/environment, GGUF artifacts, speculative options, exact devices and draft support; reuse Gufo's launch fixture shape |
| halogen | `halogen/halogen.h`, `halogen_server.cpp` | 1-2 days | Entrypoint, readiness, model/draft preparation and protocol parity; distinguish fixed flags from custom arguments |
| halobox | `llamacpp/llamacpp.h:49,71`, `llamacpp_server.cpp:108,153` | 2-3 days | This is the llama.cpp ROCm container variant. Audit alias selection, pin source, HIP environment and support topology; share the mixed llama conversion with nathanw |
| llamacpp:nathanw | `llamacpp/llamacpp.h`, `llamacpp_server.cpp`, existing v2 llama definition | 3-5 days | Mixed native/container selection and hooks, existing llama artifact/request semantics, selector-specific policy and a new identity; preserve `llamacpp@1` and introduce a separate reviewed stage |
| ds4:rocm | `ds4/ds4.h`, `ds4_server.cpp`, existing v1 `ds4.json` | 2-4 days | Mixed external/native/container ownership, directory artifacts, mode/protocol compatibility and native fallback; retain v1 bytes and stage a new v2 identity |

Allow another 1-3 days per engine if source preflight finds a missing typed fact,
mount strategy, hook effect or protocol. Real-runtime qualification requires its
own schedule and accelerator queue authorization.
