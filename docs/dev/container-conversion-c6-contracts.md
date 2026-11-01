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
