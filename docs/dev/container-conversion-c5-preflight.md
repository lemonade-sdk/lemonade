# C5 container conversion preflight

C5 stopped each of its five engine conversions at a concrete expressibility or
source-provenance blocker. No C5 production definition, execution grant, pin,
registry entry, default, registration or identity transition was added. Gufo
remains the sole staged production container definition and remains inactive.
The existing native DS4 definition and active llama.cpp identity are unchanged.

This audit applies to integration base
`47506840bc6e1f2dd7eae92e3e9a8c9d36c97f30`. The source references below are Git
objects, not files checked out from the stack. Reproduce a quotation with
`git show <revision>:<path>`. Paths ending in `*_server.cpp` are under
`src/cpp/server/backends/<family>/`; descriptor and server headers are under
`src/cpp/include/lemon/backends/<family>/`.

| Engine, in migration order | Pinned stack source | Blocking behavior |
|---|---|---|
| rocmfpx | `e3d734e056d1caada9efc4a2c8b218c23e0016b2` (`origin/jfowers/cb-5-rocmfpx`) | Raw `mtp` label is unavailable to launch expressions; the existing reusable runtime hook loses it in a supported input combination |
| halogen | `cbbeeb5270b19e06cfd9c150233655776392bd38` (`origin/jfowers/cb-6-halogen`) | Host-exclusive scheduling is absent from the schema and runtime; bundle and host-check contracts are also missing |
| halobox | `23e953915c96d2b2a039a0b3ffdfae46f97301d5` (`origin/jfowers/cb-9-halobox`) | Selected container backend and device-validation API need distinct identities |
| llamacpp:nathanw | `d491c946fed2ab079f7387cf02a0eaabb51afce4` (`origin/jfowers/cb-4-llamacpp-nathanw`) | Same device-validation API gap, with Vulkan rather than ROCm |
| ds4:rocm | `5df9cbe2de1c77eb7e5b28be2bf6d1d527f77272` (`origin/jfowers/cb-3-container-ds4`) | The named source still describes native DS4; the later OCI implementation also needs unavailable memory-planning and request-transform contracts |

## ROCm FPX: the reusable MTP fact is lossy

At its pinned revision, `rocmfpx_server.cpp:126-128` calls:

```cpp
command.args = rocmfpx::build_server_argv(gguf_path, mmproj_path, draft_path,
                                          has_label(model_info.labels, "mtp"), ctx_size, port_,
                                          rocmfpx_args);
```

The argv builder adds `{"--spec-type draft-mtp", "--spec-type"}` whenever that
boolean is true (`rocmfpx_server.cpp:86-89`). It then supplies `--parallel 1`
with alias `-np`, `-b 2048` with alias `--batch-size`, and `-ub 2048` with alias
`--ubatch-size` (`:90-96`). User flags override those defaults. No DFlash
condition guards the MTP default.

The installed `llamacpp.runtime_options.v1` hook has different semantics:

```cpp
const bool use_dflash = !input.draft_checkpoint.empty() && dflash(input.draft_checkpoint) && labeled(input.labels, "dflash");
return {use_dflash, !use_dflash && labeled(input.labels, "mtp")};
```

See `src/cpp/server/backends/configured/typed/llamacpp_hooks.cpp:78-81`.
The two inputs below have the same checkpoint, paths, options and model name:

| Labels | Draft checkpoint | Existing runtime facts | Required ROCm FPX MTP default |
|---|---|---|---|
| `dflash` | `org/repo:dflash.gguf` | `dflash=true, mtp=false` | Absent |
| `dflash, mtp` | `org/repo:dflash.gguf` | `dflash=true, mtp=false` | `--spec-type draft-mtp` |

The existing prepare hook also exposes the same facts for these two inputs.
The model expression frame contains only `name`, `checkpoint` and
`speculative_mode`; raw labels are not available
(`backend_definition_runner.cpp:447-450`). The installed typed reference table
provides only the lossy `/runtime/mtp`, not `/model/mtp` or `/model/labels`
(`backend_definition_policy.cpp:411-420`). The expression language has no
arbitrary label-membership operation. Replacing the raw label test with
`/runtime/mtp` therefore changes behavior. Testing only the three current model
rows would miss this valid model-input counterexample.

The CPU probe compiled the stack's exact argv-builder and reserved-flag function
bodies. Both helper headers were verified byte-identical to that stack before
using them. It confirms different stack argv for the two inputs above and
identical installed runtime-hook facts. This is a semantic counterexample,
not a real model or container run.

The launch's other source values are `llama-server`, `-m`, `--ctx-size`,
`--port`, `--host 0.0.0.0`, `--jinja`, `--metrics`, optional `--mmproj` and
`--model-draft` (`rocmfpx_server.cpp:57-75`). Model files are expanded in
main/projector/draft order (`:129-134`). These are expressible by the existing
records, but they are insufficient without a faithful MTP input.

Protocol parity also needs care: `RocmFpxServer` inherits `LlamaCppServer`
(`rocmfpx_server.h:15`), including its request/response behavior, slots and
tokenizer interfaces. Its capability function removes only `CAP_EMBEDDINGS`
and `CAP_RERANKING` (`:39-41`), yielding advertised mask zero. Slots and
tokenizer are not counted in that deployment-mode mask. Copying Gufo's six
identity-forwarding records would lose this inherited surface despite the
matching zero mask.

A prerequisite must expose the raw MTP-label fact through an audited typed
contract while preserving the existing llama hook and its identity.

## Halogen: exclusive host ownership and HGN bundles

The pinned descriptor requires `SlotPolicy::ExclusiveHost`
(`halogen.h:19-22`). Current `SlotPolicy` contains only `Standard`,
`ExclusiveNpu`, `CoexistByType` and `Unmetered`
(`src/cpp/include/lemon/backends/backend_descriptor.h:27-32`), and the v2 schema
permits only their corresponding names
(`schemas/backend-definition-v2.schema.json:527-535`). The CPU resolver probe
with `slot_policy: "exclusive_host"` rejects with:

```text
invalid_value /merge/metadata/slot_policy
```

Using `standard` would discard the stack's host-wide exclusion guarantee. The
prerequisite includes router scheduling semantics as well as schema and
descriptor representation.

Additional missing contracts, independently visible in the same stack:

| Source quotation | Required behavior | Current boundary |
|---|---|---|
| `info.max_context_window = kNativeContext` and `options.set_option("ctx_size", kNativeContext)` (`halogen_server.cpp:47-61`), with `kNativeContext = 262144` (`:30`) | Set native context metadata and resolve automatic/noninteger/nonpositive context before router sizing | Existing runtime hook only returns llama speculative facts; there is no Halogen context contract |
| `file == main_variant`, suffixes `-ngram.hgn`, `-vision.hgn`, and prefix `tokenizer/` (`:67-83`) | Download exactly the selected HGN bundle and reject a missing main variant | `ConfiguredV2BackendOps` has no `select_checkpoint_files` override; installed artifact hooks are llama GGUF operations |
| `kfd_gpu_properties("gfx1151")` and `halogen::check_kfd_node(properties)` (`:86-92`) | Check the selected KFD node's SVM and CWSR prerequisites | No installed container-host-check hook/effect or configured ops method |
| `(values["capability"] & 0x08000000ULL) == 0` (`:110-115`) and `ctl_stack_size + (simd_count / 2) * 479232ULL` (`:117-123`) | Preserve both exact setup failures and repair instructions | Topology/architecture selection alone does not provide these property values |
| `extra<std::string>("halogen_lookup_table", "")`, `extra<std::string>("halogen_vision_tower", "")` (`:152,159`) | Resolve and validate same-snapshot table/tokenizer/optional vision paths | Those model extras are absent from the typed expression frame |

The image entrypoint, `HALOGEN_CHECKPOINT`, `HALOGEN_NGRAM_TABLE`,
`HALOGEN_TOKENIZER`, optional `HALOGEN_VISION_TOWER`, `HALOGEN_API_PORT`,
`HALOGEN_CTX`, `/health`, and global timeout floor 1800 are established source
requirements (`halogen_server.cpp:176-200`). Entrypoint execution, environment
records and the timeout floor are already expressible; they are not blockers.
The inherited host environment and trusted HIP selection still apply.

## Halo Box and Nathanw: backend identity differs from device API

Halo Box is a backend selector under recipe `llamacpp`, not a standalone
recipe. The stack maps its selector to ROCm for device validation:

```cpp
if (backend == "nathanw") return "vulkan";
if (backend == "halobox") return "rocm";
```

See Halo Box `llamacpp_server.cpp:105-110`. The result is passed to
`BackendUtils::validate_device_backend_match(device_api(llamacpp_backend),
llamacpp_device)` at `:368`. Nathanw's own pinned stack maps
`backend == "nathanw" ? "vulkan" : backend` at `llamacpp_server.cpp:105-108`
and uses it at `:366`.

Current `DefinitionRunner` passes the selected backend directly to the installed
backend hook (`backend_definition_runner.cpp:584-594`). Its device-validation
hook forwards that input unchanged (`typed/llamacpp_hooks.cpp:43-45`), and
`SelectionServices` passes `input.backend` unchanged to the matcher
(`backend_runtime_services.cpp:335-343`). The selection must remain `halobox`
or `nathanw` for execution-case selection and the pin; replacing it globally
with `rocm` or `vulkan` changes which backend is launched.

The CPU probe calls the actual installed validation hook and actual matcher:

| Selector and explicit device | Current unmapped result | Stack mapped result |
|---|---|---|
| `halobox`, `ROCm0` | Rejects: `contradicts backend choice 'halobox'` | Accepts under `rocm` |
| `nathanw`, `Vulkan0` | Rejects: `contradicts backend choice 'nathanw'` | Accepts under `vulkan` |

There is no JSON record for the device-validation API. Omitting the backend
hook would also omit validation, including wrong-device rejection. A new,
reviewed mapping contract is required, without changing `llamacpp@1`.

Both stacks require Linux Guest/container/gfx1151 support and the same toolbox
repository. Halo Box grants `/dev/dri` and `/dev/kfd`; Nathanw grants only
`/dev/dri`; both have empty capability additions. Their launches retain the
llama request/protocol surface, explicit-device handling, optional
projector/draft paths, container host `0.0.0.0`, and the rejection of `hf_load`
on the private container network. The source model inventory belongs to recipe
`llamacpp` (99 rows at each pinned source), shared with native variants; it is
not 99 dedicated models per container. No duplicate standalone recipe or model
inventory was invented.

## DS4: source discrepancy and later OCI behavior

The specifically named `cb-3-container-ds4` ref resolves to
`5df9cbe2de1c77eb7e5b28be2bf6d1d527f77272`. At that revision:

- `ds4.h:54` says `BackendFormat::Native`.
- `backend_versions.json` has `ds4.rocm = "b0001"`, not an OCI digest.
- `ds4_server.cpp:90` obtains `get_backend_binary_path` and `:152` starts
  `NativeProcess` with the bundled loader path.
- There is one DS4 model row.

The later named Gufo stack, `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78`, does
contain OCI DS4. It was inspected separately rather than silently treating it
as the earlier ref. It has ten DS4 model rows and these further requirements:

| Source quotation | Missing contract |
|---|---|
| `return (std::max)(igpu.vram_gb, igpu.virtual_gb)` (`ds4_server.cpp:29-37`) and `file_gib(main) + file_gib(draft)` (`:66-67`) | Typed integrated-GPU pool and artifact-byte-size inputs |
| `memory_plan(igpu_pool_gb(), weights_gib(model_info))` (`:126`) | Resident/streaming choice and derived cache/reserve sizes from `ds4.h:39-61` |
| `get_available_memory_gb(...)`, `min_resident_gb`, and exact `ds4 needs ... GiB ...` failure (`:140-150`) | Prelaunch free-memory service and model working-set fact; this cannot be replaced with a constant |
| `DS4_ROCM_STREAM_MODEL_CACHE_GB`, `DS4_ROCM_STREAM_FREE_RESERVE_GB` (`:180-185`) | Environment values dependent on that memory plan |
| `without_no_think_prefix(request)` (`:199-200`), and the buffered/streaming prefix handling at `:51-61,203-214` | A request hook that strips `/no_think\n` from user string messages only, including the streaming malformed-JSON passthrough rules |

Current `LaunchServices`, the installed hook contracts and typed facts provide
none of those memory inputs. Existing expression operators do not implement
message-array iteration and prefix stripping. Available llama request hooks
do not perform this DS4 transformation. Reusing the native v1 definition or
always selecting streaming would drop required behavior from the later OCI
source. No v1 definition was edited and no DS4 v2 leaf was staged.

## Pin inventory and activation boundary

These values are source inventory only. **None has been registry-validated in
C5, and none was added to the integration branch's version file.**

| Source and version key | Repository | Stack pin |
|---|---|---|
| ROCm FPX `rocmfpx.rocm` | `docker.io/kyuz0/amd-strix-halo-toolboxes` | `rocm-10.0-rocmfpx@sha256:4df326e907a469f4acc6091fa7fb68fb275c2d5a7136bacd8e47e7339a5c49f7` |
| Halogen `halogen.rocm` | `ghcr.io/peonist-ai/halogen-flash-server` | `latest@sha256:f4ee69cafd334f5fed1173e998550dea00505e0df2bb3cb90b60d7a7aa1332f4` |
| Halo Box `llamacpp.halobox` | `docker.io/kyuz0/amd-strix-halo-toolboxes` | `rocm-10.0-strix-llama@sha256:7d3844524fda1865a2ae60cc73eb232589626ccca245cc3c7bd2afb153bd9bd6` |
| Nathanw `llamacpp.nathanw` | `docker.io/kyuz0/amd-strix-halo-toolboxes` | `vulkan-radv-performance@sha256:41018261e61b7e32fb38e4985ba569d0837804e727dcc62689c2fb1d73ec8f15` |
| Earlier DS4 `ds4.rocm` | Native `lemonade-sdk/ds4-rocm` release | `b0001` (not an OCI pin) |
| Later Gufo-stack DS4 `ds4.rocm` | `docker.io/kyuz0/strix-halo-ds4-toolbox` | `rocm-10.0@sha256:31b4e7e3584684768ee060e3dfeb62c1d716219da468090f3483977fdf6dfb47` |

No engine has C5 authored definition lines, a new owner digest, a production
parity fixture, or implemented C5 parity mutation rules. Those items must wait
for the prerequisite contracts. Prior Gufo and runtime mutation campaigns do
not qualify any of these five engines.

After the prerequisites, each engine still needs the full procedure in
[container-conversion.md](container-conversion.md): exact production data and
fixed dormant grant, inventory and parity, six or more named failing mutations,
its own identity disclosure and complete CPU qualification. Activation remains
a separate step requiring reviewed active policy/registration, generated
defaults/support, registry validation and real container/network/device/model,
cancellation, cleanup and packaging qualification. No active container leaf
was introduced by this audit.

Pinned captures, inventory, probe source/build commands and output, preservation
proofs and fresh validation results are retained under
`.synagent/artifacts/container-v2/c5/` and
`.synagent/scratch/container-v2/c5/`. The eleven CPU probe assertions establish
blockers, not conversion parity or hardware qualification.
