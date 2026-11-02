# C9 final container conversions

C9 starts at `6fb2a7531dfdebd3fe4a356f711ec4f431559227`. Each migrated leaf
stays inactive. The seven v1 resources, `llamacpp@1`, both installed v2 builtin
policies, both schemas, the literal splitter and all eighteen prior identity
manifests remain byte-identical. Historical readers execute in the independently
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
