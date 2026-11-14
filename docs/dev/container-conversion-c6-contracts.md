# C6 container prerequisites

C6 adds prerequisite contracts without migrating or activating an engine.
The C5 source audit remains the provenance inventory. Registry, real container,
model and accelerator qualification remain outside these CPU fixtures.

## Raw speculative labels

At stack `e3d734e056d1caada9efc4a2c8b218c23e0016b2`,
`src/cpp/server/backends/rocmfpx/rocmfpx_server.cpp:126-128` passes
`has_label(model_info.labels, "mtp")` to its argv builder. At lines 86-89 it
adds `defaults.push_back({"--spec-type draft-mtp", "--spec-type"});`.
That test does not suppress MTP when DFlash is selected.

The two input-phase references `/model/labels/mtp` and `/model/labels/dflash`
are exact, case-sensitive membership booleans. They are available to runtime
argument defaults and launch expressions. They do not expose the label array or
an arbitrary membership operator. Unknown descendants, mismatched types and
non-boolean defaults reject during compilation. Neither the typed llama hooks
nor their policies, grants, source definitions or effective speculation behavior
change. In particular, DFlash continues suppressing `/runtime/mtp`.

`test/fixtures/backend-v2/c6.raw-labels.json` freezes the source quotations,
ordered argv records, custom-argument defaults and independent expected argv.
`raw_label_contract()` in the existing runner suite executes these records with
fake artifact paths and a fake launch service. It covers the distinguishing
`[dflash]` and `[dflash,mtp]` inputs, duplicate labels, case sensitivity, empty
labels and user overrides including aliases. No container runtime is invoked.
The compiled mutations `raw-label-suppressed`, `raw-label-conflated` and
`raw-dflash-suppressed` must exit 1 at named parity assertions; build failures,
crashes and timeouts do not count.

The JSON schema bytes are unchanged by this contract. Runner service policy
6 to 7 is disclosed separately in `c6.raw-labels.catalog-transition.json`:
97 complete snapshot changes. Admission, execution and schema identities stay
fixed; catalog IDs and their propagated CLI responses and full/filtered ETags
change. Historical stages 171/141/151, C2, C4a and C4b replay first with their
original manifests and exact comparators. No historical disclosure is regenerated.

## Device-validation API per container case

At Halo Box stack `23e953915c96d2b2a039a0b3ffdfae46f97301d5`,
`src/cpp/server/backends/llamacpp/llamacpp_server.cpp:105-110` contains
`if (backend == "nathanw") return "vulkan";` and
`if (backend == "halobox") return "rocm";`. Line 368 passes that result to
`BackendUtils::validate_device_backend_match`. At Nathanw stack
`d491c946fed2ab079f7387cf02a0eaabb51afce4`, the same file at lines 105-108
returns `backend == "nathanw" ? "vulkan" : backend`, used at line 366.

A container execution case can now contain
`"device_validation": {"backend": "rocm"}` or the corresponding `"vulkan"`
record. Its closed object requires exactly `backend`; the value is a literal
from that two-member enum. It accepts no expressions, arbitrary backend names,
extra fields or skip-validation switch. Native cases cannot attach it. The
existing maximum of 32 execution cases bounds the records. Omission selects
`DeviceValidationBackend::Selected` internally and preserves today's behavior.

The compiler retains the record as a typed enum and requires a backend
validation-hook consumer. After selecting the unique execution case, the runner
passes the API name to both the typed validation hook and its scoped
`SelectionServices`. The original selector remains in the invocation frame,
container policy, image pin lookup and program selection. The existing device
matcher still rejects explicit devices from the wrong API. The full execution
record is part of the sealed container owner, so changing this mapping requires
a separately reviewed owner. No production owner or hook grant changed.

`test/fixtures/backend-v2/c6.device-validation.json` freezes both source
quotations, a synthetic two-case definition and its owner digest. The existing
container-execution suite accepts `halobox` with `ROCm0` and `nathanw` with
`Vulkan0`, rejects each opposite device, preserves empty-device behavior and
checks the selected program, selector and distinct image pin. Removing the
records reproduces the original explicit-device rejection. Other assertions
cover the required consumer and the sealed mapping. Structural negatives reject
unknown APIs, expressions, unknown fields, an absent backend and boolean records.
The `device-validation-api-ignored` compiled mutation must fail both named
`MappedDeviceAccepted` assertions; ignoring validation rejection must fail both
`MappedWrongDeviceRejected` assertions.

This contract adds exactly one schema record at
`/$defs/ExecutionCase/oneOf/1/properties/device_validation`. Schema identity
`sha256:3c2c9298b2b7dfc160e5405e49a688cb67f4514702109c5891d3373b3de1d79c`
becomes
`sha256:92459ed5bcc276f775bf92fcf7aadcff902e5e78f5b99c357fa571c86d23e799`.
Runner service policy 7 to 8 is disclosed in the same contract stage,
`c6.device-validation.catalog-transition.json`, after the immutable raw-label
stage. Its 133 snapshot records cover schema, admission, execution, catalog,
CLI and ETag propagation. The comparator independently permits that single
schema addition and requires complete snapshot equality.

## Remaining prerequisites

The C5 expressibility blockers are closed for ROCm FPX, Halo Box and Nathanw.
Their production migrations, grants, pins and parity qualification remain the
next unit's work. No engine was migrated by these contracts.

C6b closes Halogen's host-exclusivity, bundle, context and host-fact
prerequisites as described below. Its engine migration remains a separate unit.

C6b also closes DS4's memory-fact, residency/cache and content-rewrite
prerequisites as described below. The earlier native-versus-later-OCI source
provenance distinction in C5 still applies.

## C6b: Halogen model and host contract

`SlotPolicy::ExclusiveHost` is represented by `exclusive_host` in v2 metadata.
`Router::load_model()` calls `enforce_host_exclusivity_locked()` while holding
its load mutex and before memory admission or constructing a backend. Local
admission evicts resident host-exclusive models; host-exclusive admission
evicts every other local model. Unmetered cloud models remain. A busy resident
or a cross-residency displacement refuses admission. Eviction is bounded and
admission fails if the displaced server remains. `is_loading_` continues
serializing startup even while `load_model()` releases its mutex for launch.

The closed declaration value `model_contract: "halogen_hgn_v1"` compiles to
`ModelContract::HalogenHgnV1`. It requires a container definition, an exclusive
host slot and the four explicit roles `main`, `halogen_lookup_table`,
`halogen_tokenizer`, and `halogen_vision_tower`. The execution owner seals both
the contract and its slot policy. Existing owners have exactly their previous
semantics when the new property is absent. This adds no callback, arbitrary
extra lookup, arbitrary host read, shell command, or production grant.

Model management retains the base validation/import defaults and normal active-
snapshot path resolution without requiring a GGUF artifact hook.

The contract implements the stack's case-sensitive `.hgn` selector: preserve
repository order and include the selected main variant, every `-ngram.hgn` and
`-vision.hgn` sidecar, and every `tokenizer/` descendant. A missing main variant
throws; other formats return the default-selector sentinel. Metadata reports
262144 context tokens. `ConfiguredV2BackendOps::resolve_runtime_options()`
normalizes noninteger or nonpositive context to 262144. The existing
`Router::resolve_effective_options()` calls that operation before
`resolve_auto_ctx_size()` and before any backend launch. The fixture passes a
model far larger than host memory through this Router path and proves that
its native context bypasses automatic memory sizing; moving normalization to
launch fails `HalogenContextBeforeRouterMemoryAdmission`.

Bundle roles resolve from the main checkpoint's snapshot and exactly two named
extras: `halogen_lookup_table` (required), `halogen_vision_tower` (optional).
`tokenizer` is fixed. Missing files preserve the stack's errors. Absolute or
escaping extra paths reject before mounting; existing confined mount validation
still applies. The fixture compares complete resulting entrypoint, argv,
environment, mounts, `/health`, and the 1800-second startup floor for vision and
text-only bundles. `EnvironmentSet.when` is an optional boolean expression;
it expresses the absent vision variable without supplying an empty value.

The selected supported `gfx1151` KFD node supplies at most 65536 bytes of
properties through a narrow node-ID service. The SVM bit is `0x08000000`.
CWSR requires all three properties and exactly
`ctl_stack_size + (simd_count / 2) * 479232`, using integer division. Failure
IDs, messages and repair steps match stack `cbbeeb5270b19e06cfd9c150233655776392bd38`,
`halogen_server.cpp:110-125`. Empty properties retain the source's no-extra-check
behavior; unavailable services and oversized reads throw. GPU topology and
container confinement still apply. CPU fixtures provide every host fact and
never launch a container runtime.

`test/fixtures/backend-v2/c6b.halogen.json` retains exact stack quotations and
independent expected selections and failures. `ContainerPrerequisiteTest`
executes the contract and full Router busy/concurrent admission refusals.
Named compiled mutations cover unenforced exclusivity, one-way exclusivity,
context resolved only at launch, ignored bundle selection, ignored extras,
ignored SVM/CWSR checks, missing metadata and management defaults, and ignored
conditional environment assignment.

This is a new identity stage, `c6b.halogen.catalog-transition.json`, with runner
service policy 8 to 9. Its four exact schema records add the closed declaration
property, its merge-envelope position, conditional environment assignment,
and the `exclusive_host` metadata enum member. The stage contains 133 complete
snapshot records. The schema identity is
`sha256:df03a45692d8c53dbc44a0891ffbb25fff249a83c6ed4aa01c9f15545882d251`.
All earlier stages retain their original disclosures and exact comparators;
the C6 device stage now reads its schema bytes from its immutable commit.
Halogen's prerequisite is closed by these contracts. Its production definition,
reviewed dormant owner/grants/pin and migration parity campaign remain separate.

## C6b: DS4 memory and content contracts

The earlier `cb-3-container-ds4` source is still native. These prerequisites
implement the later OCI source at `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78`;
they do not relabel the earlier pin or migrate either implementation.
`test/fixtures/backend-v2/c6b.ds4.json` retains both revision identities, exact
quotations and independent expected memory plans and request bodies.

A container execution case can select exactly `memory_plan: "ds4_rocm_v1"`.
It requires the ROCm selector, a single backend phase before its single
artifact phase, and explicit main/draft artifact roles. Native cases cannot attach it. The compiler retains a typed
`MemoryPlanPolicy`; the runner reads facts after artifact validation, before
mounting or emitting argv/environment. The execution owner seals the record.
No definition can supply a host path or executable for collecting these facts.

The narrow services supply integrated-GPU availability, VRAM and virtual-window
GiB, main/draft byte-size facts, and known/unknown free GPU memory. Missing
service facts reject; memory references cannot specify defaults. An absent
draft has an explicit zero-byte size. An observed stat failure is represented
by a present `ArtifactByteFact{false, 0}`, retaining the source's zero-byte
result; an unavailable fact is `nullopt` and rejects. The session exposes this
status, raw pool/byte facts, free-memory facts when queried, and the derived
plan through a closed `/memory/` reference table. Invalid numeric facts reject.

The pool is zero without an available integrated GPU; otherwise it is the
larger of VRAM and the virtual window. Weights are main bytes / 2^30 plus draft
bytes / 2^30. Positive weights are resident when the positive pool is at least
weights + 16 GiB. Residency leaves a 16 GiB reserve and disables the streaming
caches. Otherwise a positive pool yields integer-truncated expert cache 40%,
model cache 20%, and free reserve 60%. A nonpositive pool leaves all three zero.
The working set is weights when resident, or `min_resident_gb` when streaming.
Only a positive working set and reserve query free GPU memory. Known free
memory below their sum preserves the source's one-decimal refusal message;
explicit unknown memory preserves the source's no-refusal behavior. The
fixture checks the equality threshold, draft bytes changing residency, both
pool inputs, streaming, no pool, zero weights, unknown memory, and exact
low-memory refusal. It compares complete resulting argv, environment and
conditional draft mounts, including DSpark and all three cache settings.

`request.content_transform: "ds4_no_think_v1"` compiles to a typed content
transform on buffered or streaming chat-completion operations. It has no
parameters and accepts no arbitrary prefix, role or message-selection code.
The source's exact selection rule is **all** object messages whose `role` is
exactly `user` and whose `content` is a string. Remove exactly one leading
`/no_think\n`; preserve later copies, leading whitespace, other roles,
non-string content and multipart content. This is not limited to the last
user message. A non-string role retains the source's conversion exception.
The source streaming wrapper selects endpoints containing `chat/completions`;
the fixture binds the canonical chat operation and preserves completion and
responses bodies. Existing bounded operation admission remains in force.

With `bound_passthrough`, malformed streaming JSON preserves the original bytes.
Valid streaming chat JSON is rewritten and reserialized even when no content
changes, matching the source. Buffered and streaming bodies traverse the
actual transport and an ephemeral loopback HTTP echo, so both wire bodies are
observable independently. Tests cover multiple historical user messages,
repeated/bare prefixes, an empty result, role case, multipart content, valid
primitive/non-message JSON, malformed JSON and the role-conversion exception.
The sealed container owner additionally records selected content transforms;
old owners have exactly their prior semantics when no transform is selected.

This is its own stage, `c6b.ds4.catalog-transition.json`: runner policy 9 to 10,
two exact schema additions (`ExecutionCase` container `memory_plan` and
`Request.content_transform`), 133 full snapshot records and schema identity
`sha256:59c258591be9f262c9bf771e2ef7419883f06b0bc3f2399d22e76426789fd439`.
The Halogen stage now reads its schema from its immutable local commit, keeping
its original comparator and disclosure exact. No earlier disclosure changes.
Both remaining C5 prerequisites are closed. Halogen and DS4 still require their
production definitions/owners/pins and separate migration parity campaigns;
real container, registry, device, model, cancellation and packaging qualification
remain outside these CPU prerequisites.
