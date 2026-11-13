# C8a container prerequisites

C8a adds capabilities, not engine definitions. The C8 production inventory,
seven v1 definitions, both fixed v2 builtin policies, `llamacpp@1`, grants,
models, pins and generated defaults remain byte-identical to
`316d3b96ccce498abc2a4a08ffd50446fc7203e2`.

## Pre-install private-network refusal

`launch.pre_install_refusal: "container_hf_load_v1"` is the only selection.
It reads the existing typed `ModelInfo` `hf_load` boolean and applies only to
selected container execution. It throws the source's fixed private-network
message with model and backend substitutions immediately after backend
selection/diagnostics, before the preparation loop advances to installation.
It has no expression, supplied error text, hook ID, callback or selectable
phase. An absent/false fact proceeds normally. Native execution is unaffected.

Compilation requires builtin provenance, a container-capable declaration and
an exact private execution owner. The owner seals the selection and complete
preparation order; it already seals backend and execution selection. The
compiler requires backend first, exactly once, and an explicit installation
phase exactly once. Installation respects that explicit phase even without
load diagnostics. Binding and runner construction retain their independent
execution-owner checks. External container admission remains forbidden;
external local and ungranted builtin selections cannot compile.

The exact quotations in `c8a.halobox.json` and `c8a.nathanw.json` include both
the refusal and the later `install_backend` call. Their definitions are opt-in
extensions of the unchanged C8 blocked fixtures. The failing install service
records every call before throwing `Fake install failure`. With `hf_load=true`,
both fixtures produce the complete source refusal and an empty event list.
With the fact absent or false they produce the install failure and exactly one
install event. Successful non-HF preparation installs exactly once.
`C8aRefusalBeforeAnyInstall:<engine>` catches moving the refusal to the ports
phase after installation. Separate compiled mutants remove the external gate
and owner seal; each must link, exit normally with status 1 and print its named
failing assertion. Compile failures, crashes and timeouts never count as kills.

The old three blocked fixtures and every original assertion remain unchanged.
The new consumer is opt-in, so the historical probes still describe their
original unextended definitions accurately.

## Identity disclosure

`c8a-pre-install-refusal` is a new stage after the sixteen immutable stages.
It adds exactly one schema property and advances runner/service policy
**14 -> 15**, disclosing **133** complete snapshot records. Schema bytes also
expand the three existing compact JSON values through standard JSON formatting;
those whitespace bytes are included in the disclosed schema digest. No prior
manifest is regenerated and no comparator exception is added.

`container_c8a_control.py` builds the pinned C8 tree independently.
`container_c8a_transition.py` executes its original sixteen-stage readers,
then derives each new stage from independent schema records, identity inputs
and whole-snapshot comparison with actual HTTP/CLI captures. Its census requires
all 163 old CI registrations plus the new contract registration. The new source
checker verifies every old resource, fixture and container transition reader,
including both policies and the literal splitter, byte-for-byte against C8.
Each source quotation is checked against `git show` at its pinned revision.

## Migration boundary

Halo Box and Nathanw's identified pre-install ordering gap is now expressible.
They still need complete production definitions, reviewed execution owners,
shared llama.cpp selector integration, source parity and new inventory stages.
This prerequisite does not migrate or activate them. OCI DS4's absolute-path
consumer is implemented by the second contract below.
Real image/registry, networking, accelerator, inference, cancellation, cleanup
and packaging qualification remains outside these CPU fixtures.

## Absolute-checkpoint consumer

`artifacts.<id>.checkpoint_consumer: "builtin_container_absolute_v1"` is the
only new artifact-consumer selection. It requires builtin container authority
and the exact execution-owner seal independently at compilation. It also requires
the existing sealed `ds4_rocm_v1` model contract, whose compiler restricts it to
a single ROCm container case; mixed native/container definitions cannot use it. Binding and
runner construction retain their checks. The artifact must be a required main
artifact with `strategy: "resolved_path"`, `check: "exists"` (including its
absent default) and `absolute_checkpoint_fallback: true`. It must have an
unconditional `file` mount through the existing artifact inventory. Optional
artifacts, nonempty-only checks, other roles/strategies and omitted or
conditional inventory mounts fail closed. The existing owner already seals the
complete artifact and mount records. No callback, hook ID or arbitrary path
expression is added.

The runner first retains `ModelInfo::resolved_path("main")`. Only when that path
is empty or does not exist does it consider `checkpoint("main")`; that checkpoint
must be nonempty, absolute and existing. An existing resolved path always wins,
including when its eventual inventory validation fails. If neither supplies an
existing path, the artifact's missing-error expression reproduces the source
`ds4: no local GGUF found for model ... (checkpoint: ...)` diagnostic literally.
The selected path enters the same inventory and mount materialization as every
other container artifact. Selection does not bypass regular-file, mount,
confinement or handle-inventory checks. The old fallback flag without the new
consumer still reports `Configured v2 artifact consumer is unavailable` in v2;
v1's separate `LaunchPlanner` is untouched.

`c8a.ds4.json` extends the unchanged C8 blocked DS4 fixture, which is an
explicitly synthetic Gufo-derived leaf with the DS4 model contract. It freezes
the exact DS4 source quotation at
`8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78:src/cpp/server/backends/ds4/ds4_server.cpp:88-96`.
It proves this path-selection contract; it does not claim a production DS4
repository, argv, pin or migration. CPU files use `.fake` names and contain no
model weights. Tests cover empty/stale resolved paths, resolved precedence,
empty/relative/missing checkpoints, retained preparation order and inventory
rejection of directories and NUL paths. Compiled mutants exercise precedence,
relative-path admission, inventory bypass, the external capability gate, owner
sealing, optional main artifacts and conditional inventory mounts.

The second new stage is `c8a-absolute-checkpoint-consumer`, runner/service policy
**15 -> 16**, with one exact schema-property addition and **133** snapshot
records. The first C8a manifest remains unchanged. The final CI census is
**163 -> 165**, with every prior registration retained and both new tests in the
aggregate dependency. The prior source checker runs from its pinned C8 tree;
the C8a checker additionally verifies the current protected bytes and new
source quotations. The original transition readers and all sixteen manifests
remain unchanged and execute from the pinned C8 replay tree.

## Final migration boundary

Both documented C8 format gaps are now expressible. Halo Box and Nathanw can
proceed to production migration with their source pre-install refusal; OCI DS4
can proceed with its source absolute-checkpoint selection and the existing
inventory boundary. This is a statement about these two contracts, not a
complete source-parity qualification or a promise that a later full conversion
cannot uncover another gap. No production definition, execution grant, selector,
model, pin or activation is added here.

Halo Box and Nathanw still need complete production conversion/parity and
reviewed shared llama.cpp selector integration. DS4 still needs its complete
OCI production definition, reviewed owner/pin inventory and full parity.
All three retain the real registry/image, runtime/network, accelerator/model,
lifecycle/cancellation/cleanup and packaging qualification gates before
activation. None of those gates is satisfied by CPU fake-host tests.

## CPU qualification

The final configured/default/aggregate builds pass. `ctest -L "^cpp-ci$"`
passes **165/165**, retaining all 163 previous registrations. The pre-install
suite passes **29** assertions and the checkpoint suite **32**. All **228**
previous compiled mutants, **12** new compiled mutants, Gufo's compiled
capability mutant and the **25** production/Gufo data mutants are caught.
Every compiled mutant must link, exit normally with code 1 and print its named
failing assertion. The first three prior campaigns were repeated after the
consumer-domain restriction, against the final built source.

All sixteen original stages and the two new 133-record stages replay. Complete
expected/actual HTTP and CLI snapshots compare literally. Both policies, all
seven v1 definitions, both existing v2 leaves and all previous manifests remain
byte-identical. The literal audit verifies 68 raw literals under the 8,000-byte
budget; synthetic splitter/codegen and Linux/Windows/macOS data-macro parity
checks pass. These are Linux CPU builds, not native Windows/MSVC or macOS builds.

The frozen CLI, CLI REPL, catalog HTTP, three renderer contracts, TypeScript,
generator `--check` and ephemeral `lemond` smoke pass. The smoke checks seventeen
active recipes, all four external-container validation prefixes, current/frozen
CLI refusal, and HTTP compilation refusal for each new capability. No actual
container runtime, model, GPU/NPU, hardware queue, port-8000 lifecycle action,
push or public action is involved.

Evidence is under `.synagent/artifacts/container-v2/c8a/`; all new scratch is
under `.synagent/scratch/container-v2/c8a/`. The earlier final-validation attempt
that overlapped a rebuild is retained separately in `pre-scope-validation/` as
incomplete evidence, not a passing qualification. The stable full rerun and a
seven-suite recheck after the final helper-include cleanup both pass.
