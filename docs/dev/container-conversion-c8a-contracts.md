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
consumer remains the second C8a contract until implemented separately.
Real image/registry, networking, accelerator, inference, cancellation, cleanup
and packaging qualification remains outside these CPU fixtures.
