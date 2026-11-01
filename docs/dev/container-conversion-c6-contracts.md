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

Halogen remains blocked: `exclusive_host` is still absent from both the schema
and Router scheduling, and HGN bundle selection, native-context resolution
before Router memory admission, model extras, SVM and CWSR host checks have not
been implemented. There is no accepted-but-unenforced exclusivity policy.

DS4 remains blocked on integrated-GPU pool, weight-byte and free-memory inputs,
its derived residency/cache plan and the explicit user-message `/no_think\n`
rewrite for buffered and streaming requests. No content rewrite or corresponding
mutation qualification is claimed. The earlier native-versus-later-OCI source
provenance distinction in C5 still applies.
