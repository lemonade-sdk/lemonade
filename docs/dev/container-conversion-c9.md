# C9 final container conversions

C9 starts at `6fb2a7531dfdebd3fe4a356f711ec4f431559227`. Each migrated leaf
stays inactive. The seven v1 resources, `llamacpp@1`, both installed v2 builtin
policies, the v1 schema, the literal splitter and all eighteen prior identity
manifests remain byte-identical. The v2 schema receives only the independently
disclosed DS4 timing property described below. Historical readers execute in the independently
built C8a control tree. New manifests disclose complete inventory snapshots;
there is no comparator exception or regeneration of historical disclosures.

## Halo Box

`backends/halobox.json` is a distinct builtin identity with public recipe
`llamacpp` and backend selector `halobox`. Its complete declaration equals
`c8a.halobox.json` field for field. It retains the shared llama artifact,
runtime-option and API hooks, explicit ROCm device-validation mapping, optional
projector/draft inventory, numbered read-only mounts, `/health`, filtered
output, ordered diagnostics and the source private-network refusal before any
installation. The shared llama advertised capability mask remains 3 (embeddings and reranking);
empty default model-capability labels are a separate field.

The fixed dormant execution owner is the literal in
`dormant_container_owners/halobox.json`. It is independent of hook grants and
is absent from the installed active policies. Its digest is
`sha256:35519afe45fc37e887a6597af3c3ae1fa653f64eb5d040593515640c5b888e1c`.
The definition has 409 authored lines. The pin is copied from
`23e953915c96d2b2a039a0b3ffdfae46f97301d5:src/cpp/resources/backend_versions.json`,
key `llamacpp.halobox`:
`rocm-10.0-strix-llama@sha256:7d3844524fda1865a2ae60cc73eb232589626ccca245cc3c7bd2afb153bd9bd6`.
The repository is `docker.io/kyuz0/amd-strix-halo-toolboxes`.
**The pin is unvalidated; no registry or image access is performed here.**

`halobox.production-parity.json` freezes the production declaration, source pin,
metadata/support/defaults, owner digest, argv, mounts, capability mask and both
inventories. The stack contains 99 shared llama model rows; the integration
registry contains 93. Neither inventory is a list of dedicated Halo Box models.
C9 retains the current shared registry and source inventory separately rather
than replacing shared host-model registrations with historical stack values.
No public recipe, duplicate model rows or active defaults/support are added.

The Halo Box stage also discloses runner/service policy **16 -> 17** for the
shared selector dispatch and its fail-closed inactive-selector behavior.

Halo Box CPU qualification: **167/167 cpp-ci**, 152 production parity checks,
12 selector checks, eleven data mutants and five compiled runtime/capability
mutants. All eighteen historical stages and the new 99-record stage replay;
complete expected/actual HTTP/CLI snapshots compare literally.

## Nathanw

`backends/nathanw.json` retains recipe `llamacpp` and selector `nathanw`.
Its 409-line declaration equals `c8a.nathanw.json`; the device-validation API
is Vulkan, its container device set is only `/dev/dri`, and its capability
mask is 3. The independent fixed owner is
`sha256:74e821a50d34a06411afbd29215da72dcba8b01ed81f070e603b3206db4c548e`.
The copied, **unvalidated** pin comes from
`d491c946fed2ab079f7387cf02a0eaabb51afce4:src/cpp/resources/backend_versions.json`,
key `llamacpp.nathanw`:
`vulkan-radv-performance@sha256:41018261e61b7e32fb38e4985ba569d0837804e727dcc62689c2fb1d73ec8f15`.
The repository is `docker.io/kyuz0/amd-strix-halo-toolboxes`.

The production parity fixture is `nathanw.production-parity.json`. As with
Halo Box, the 99 source rows and 93 current integration rows belong to the
shared llama recipe; no standalone recipe or duplicate registry inventory is
created. The Nathanw stage discloses runner/service policy **17 -> 18** for
its additional fixed selector. Halo Box's stage is retained unchanged. Nathanw passes 152 parity and twelve
selector assertions, eleven data mutants and five compiled mutants. Its
99-record stage and all prior stages replay with literal whole-snapshot comparison.
The exact CI census is 169, retaining the 165 baseline registrations.

## Shared llama selector integration

Builtin registration seeds and finalized registrations can carry a private
backend selector. A selector requires its fixed builtin ID, the shared recipe,
a matching fixed launch backend and exact execution authority. Duplicate
selectors, legacy selector factories and a selector without its primary recipe
registration fail finalization. Only the primary recipe is published in the
schema and descriptor registry; selectors remain separate executable identities.

Router creation and retry pass the effective backend selector into the common
factory dispatcher. Explicit container selectors choose their matching reviewed
entry. An inactive selector throws before any primary/native factory can run.
Native choices continue through the primary registration. The production seeds
contain no container selectors, so none of this stages an active leaf. The CPU
selector suites construct isolated catalogs and run the actual Router path.

## Qualification and activation

All testing uses CPU fakes, persistent project scratch, ephemeral loopback ports
and bounded commands. The fake host never executes Docker/Podman. Definition
mutations must exit normally with status 1 and their named failing assertion;
compiler/linker failures, crashes and timeouts do not count as kills. The
production parity fixtures are cross-checked against their unchanged C8a inputs.

Activation requires reviewed fixed active leaf/hook policies and exact selector
or recipe registration; regeneration of active defaults/support with the normal
generator; a separate disclosed activation stage; registry/digest and image
contents validation; and actual container networking, accelerator selection,
model inference, cancellation, teardown/cleanup and packaging qualification.
CPU fake-host parity satisfies none of those real-runtime gates.

## OCI DS4

`backends/ds4_rocm.json` is a 221-line production container definition with
recipe `ds4` and fixed backend `rocm`. The native v1 `ds4.json`, its active
registration, native `ds4.rocm = b0001` pin and current model registry remain
unchanged. The OCI pin is staged at `ds4_rocm.rocm`; the fixed container policy
selects that key explicitly. This is an internal pin namespace, not a public
recipe. Its value is copied without alteration from source `ds4.rocm` at
`8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78`:
`rocm-10.0@sha256:31b4e7e3584684768ee060e3dfeb62c1d716219da468090f3483977fdf6dfb47`.
The repository is `docker.io/kyuz0/strix-halo-ds4-toolbox`.
**This copied pin remains unvalidated.**

The fixed dormant owner digest is
`sha256:4b694ed3713311ea01e98e57177ae6574fff81e8d88e0b5624bfb5a0a463640d`.
`ds4_rocm.production-parity.json` freezes the complete definition, support,
defaults, pin provenance and separate native/current and OCI/source inventories.
`resources/container_inventory/ds4_rocm.json` retains all ten exact source rows
with `active: false`. The assignment's three-row count disagrees with both the
pinned source and the C8 stopped-inventory capture, which contain ten; there is
no identified three-row subset. No rows are silently discarded or merged into
the active native recipe.

The main artifact consumer is identical to `c8a.ds4.json`. The other C8a
fields were explicitly synthetic Gufo data. Production adds the DS4 metadata,
repository/pin/capability (`SYS_PTRACE`), exact custom-argument reservations,
ordered diagnostics and full buffered/streaming protocols. It retains file
mounts, conditional resident draft loading, streaming caches and environment,
`/v1/models`, info/debug-only inherited output with filtering, the prelaunch
context budget and the chat-completions substring rewrite domain.

### Memory-query timing

The pinned wrapper installs the image and chooses a port before querying its
memory plan (`ds4_server.cpp:104-126`). The old synthetic contract queries memory
in the artifacts phase. A failing install combined with insufficient free
memory distinguishes those paths: source returns `Fake install failure` before
any memory query, whereas early memory evaluation returns the memory refusal.

The new closed selection is
`launch.execution.cases[].memory_phase: "after_ports"`. It requires the
existing `ds4_rocm_v1` model and memory contracts, an exact container execution
owner and the exact preparation sequence
`backend, artifacts, install, program, ports`. It adds no callback or arbitrary
phase expression. The existing owner already seals the entire execution case.
The compiler lowers this selection to a distinct internal memory policy; the
old artifact-phase selection and all C8a fixtures remain unchanged. The explicit
installation phase works independently of diagnostics.

The DS4 stage discloses this one schema-property addition, runner/service
policy **18 -> 19**, the inactive definition, separate OCI pin and staged
inventory. The source checker derives the schema bytes independently from the
C8a schema plus that single closed property. Earlier C9 manifests, all eighteen
pre-C9 manifests and their original comparators are retained without changes.

## Final CPU validation

Evidence is retained under `.synagent/artifacts/container-v2/c9/`, with scratch
builds under `.synagent/scratch/container-v2/c9/`. No model, accelerator or real
container runtime is used, and the existing port-8000 server is left alone.

| Gate | Result |
| --- | --- |
| CMake configuration, default build and `cpp-ci-tests` aggregate build | Pass |
| `ctest -L "^cpp-ci$"` | 170/170; the original 165 registrations remain |
| Final production parity | Halo Box 156; Nathanw 156; OCI DS4 82; all three earlier engine suites pass |
| Shared llama Router selector suites | 12 assertions per selector |
| C8a refusal/checkpoint, C7a, manager, definition, execution, prerequisite, runner and runtime suites | Pass |
| Identity replay and catalog HTTP | All 21 stages; literal complete snapshots, no residual diffs |
| Frozen CLI, CLI REPL and renderer contracts | Pass; renderer typecheck also passes |
| Backend boilerplate generator `--check` | Pass |
| Full-field descriptor dumps | Native, Windows and macOS preprocessor views identical to frozen mapper |
| Synthetic codegen | All three views; floats, null, booleans, nested JSON, escapes and absent optionals |
| Literal splitter audit | 90 raw literals, each at most 8,000 content bytes |
| Protected-source audit | 132 old inputs byte-identical; exact single-property v2 schema derivation |
| Ephemeral `lemond` smoke | Health, 17 public recipes, all four catalog/validation prefixes; inactive external container execution refused |

The first final CI pass was 169/170: the unchanged runtime-boundary suite failed
`RouterAcquiresV2InferenceBeforeForwarding`. Its isolated recheck passed all 43
assertions, and the repeated full pass was 170/170 without source or assertion
changes. The failed pass is retained in `first-final-attempt/`; the passing
aggregate is in `cpp-ci-final.log`. This is a timing-sensitive observation, not
a claimed source fix.

## Every disclosed stage

All eighteen pre-C9 manifests are byte-identical. The Halo Box and Nathanw
manifests also remain byte-identical to their individual C9 commits. Historical
readers run against the independent C8a control; every new stage uses the same
literal whole-snapshot comparison after applying its exact disclosure.

| Stage | Disclosed records |
| --- | ---: |
| `production-activation` | 171 |
| `hrx-builtin-child` | 141 |
| `production-external-export-activation` | 151 |
| `container-data-only` | 24 |
| `container-format-gaps` | 2 |
| `gufo-inactive-inventory` | 99 |
| `c6-raw-label-facts` | 97 |
| `c6-container-device-validation` | 133 |
| `c6-container-halogen-contract` | 133 |
| `c6-container-ds4-memory-content` | 133 |
| `c7a-builtin-container-stream` | 133 |
| `c7a-builtin-load-log` | 133 |
| `c7a-prelaunch-ds4-budget` | 133 |
| `c7b-diagnostics-rewrite-domain` | 133 |
| `rocmfpx-inactive-inventory` | 99 |
| `halogen-inactive-inventory` | 99 |
| `c8a-pre-install-refusal` | 133 |
| `c8a-absolute-checkpoint-consumer` | 133 |
| `halobox-inactive-inventory` | 99 |
| `nathanw-inactive-inventory` | 99 |
| `ds4_rocm-inactive-inventory` | 136 |

The seven v1 definitions, `llamacpp@1`, both fixed builtin policy files, the
active native DS4 pin, live defaults and live model registry have zero-byte
diffs. Expected/actual schema, legacy HTTP, digest and ETag residual diff files
are empty after the disclosed transitions.

## Mutation evidence

Each mutation must compile and link, exit normally with status 1, and fail its
named assertion. A crash, compiler error or timeout is not counted. The final
C9 data campaign contains 36 mutants (11 Halo Box, 11 Nathanw, 14 OCI DS4), and
the compiled campaign contains 19 (5, 5 and 9 respectively).

The following data mutations run independently for all three engines. The
assertion suffix is the engine's exact ID.

| Rule and breakage | Catching assertion |
| --- | --- |
| Change an argv flag | `C8Frozenargv` |
| Change a mount destination | `C8Frozenmounts` |
| Change devices, repository or pin reference | `C8FrozenContainerPolicy` |
| Change readiness path | `C8Frozenreadiness` |
| Change a diagnostic line or its order | `C8Frozendiagnostics` |
| Change declared capability mask | `C8FrozenCapabilityAndMetadata` |
| Change generated capability mask in compiled code | `C8GeneratedCapabilityMask` |

Halo Box and Nathanw also mutate the refusal selection and order
(`C9FrozenPreInstallContract`), move refusal after installation
(`C9RefusalBeforeAnyInstall`), misroute the selector (`C9ExactSelectorIdentity`),
permit inactive fallback (`C9InactiveSelectorCannotFallBack`) and drop Router
selector propagation (`C9RouterDispatchesSharedSelector`). Each engine has its
own failing run for every rule.

OCI DS4 additionally mutates the rewrite domain (`C8FrozenRewriteAndProtocols`),
memory phase (`C9FrozenExecutionContract`) and checkpoint consumer, fallback and
existence selection (`C9FrozenArtifacts`). Compiled mutants separately break
the following rules; each assertion has suffix `:ds4_rocm`.

| Compiled breakage | Catching assertion |
| --- | --- |
| Change source checkpoint error | `C9CheckpointStillFailsWithoutExistingAbsolute` |
| Allow checkpoint fallback over an available resolved path | `C9AvailableResolvedAlwaysWins` |
| Accept a relative checkpoint | `C9CheckpointStillFailsWithoutExistingAbsolute` |
| Bypass file inventory through fallback | `C9FallbackCannotBypassArtifactInventory` |
| Query memory before installing | `C9Ds4InstallBeforeMemory` |
| Remove preparation-order validation | `C9MemoryPhaseFixedOrder` |
| Remove model-contract validation | `C9MemoryPhaseNeedsDs4Contract` |
| Require diagnostics to perform installation | `C9Ds4InstallIndependentOfDiagnostics` |

The prior compiled campaigns retain all 228 checks. The legacy
`container-reference` fault originally replaced every reference with `rocm`,
corrupting valid Halo Box/Nathanw definitions at static initialization. That
crash is not counted. The C9 wrapper narrows only that fault to an absent
reference; it reaches the original `MissingContainerReferenceRejected`
assertion with exit 1. The original campaign and all its assertion names are
unchanged. C8a's twelve compiled mutations, Gufo's compiled capability mutation
and the three earlier engines' 25 data mutations are also rerun. Identity
disclosures have 23 additional mutation checks.

## Six-engine inventory

| Engine | Production JSON | Authored lines | State |
| --- | --- | ---: | --- |
| Gufo | `backends/gufo.json` | 97 | Inactive |
| ROCm FPX | `backends/rocmfpx.json` | 291 | Inactive |
| Halogen | `backends/halogen.json` | 146 | Inactive |
| Halo Box | `backends/halobox.json` | 409 | Inactive |
| Nathanw | `backends/nathanw.json` | 409 | Inactive |
| OCI DS4 (`ds4:rocm`) | `backends/ds4_rocm.json` | 221 | Inactive |

All six container definitions are migrated to production JSON. Activation is
still separate work: it must satisfy the qualification and exact-policy gates
above, including reconciliation of the complete dormant OCI DS4 inventory with
the native recipe. No container recipe is activated by C9.
