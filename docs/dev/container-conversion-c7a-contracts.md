# C7a contracts

These are prerequisites, not engine migrations. The seven v1 definitions, both
v2 builtin policies, `llamacpp@1`, Gufo, registry, defaults and backend pins stay
byte-identical to `2985766b9368fe3dca5173d51536edfb3db3ed9b`.

## Builtin container direct streaming

C7's three llama-derived sources call the inherited HTTP forwarder for a
query-bearing path such as `/v1/chat/completions?probe=1`. The existing
`llamacpp_legacy` compiler gate remains unchanged. The new explicit launch policy
`builtin_container_legacy` grants that endpoint surface to a privately reviewed
builtin container execution owner. It uses the existing canonical operation
bindings for chat, completions and responses, including request transforms,
malformed-body passthrough and caller SSE/timeout controls. Other safe relative
paths retain their original body and endpoint. Transport endpoint confinement
still rejects authority URLs, double-slash paths, CR/LF and NUL.

The private execution owner seals the new policy and the complete operation map,
with canonical optional-field defaults. Compilation requires builtin provenance,
a container declaration and the matching sealed owner. Binding and runner
construction independently recheck execution authority. A local builtin, an
ungranted container identity, and an external local definition cannot select it;
external container definitions already fail at authorization. No production
identity receives a new grant in this unit. A definition that selects `strict`
or `bound_passthrough` still cannot reach an arbitrary endpoint.

`test/fixtures/backend-v2/c7a.stream.json` contains exact method quotations from
ROCm FPX `e3d734e056d1caada9efc4a2c8b218c23e0016b2`, Halo Box
`23e953915c96d2b2a039a0b3ffdfae46f97301d5`, and Nathanw
`d491c946fed2ab079f7387cf02a0eaabb51afce4`, all in
`src/cpp/server/backends/llamacpp/llamacpp_server.cpp`.
`ContainerC7aTest` exercises all three synthetic builtin identities over ephemeral
loopback HTTP, without creating an engine definition or invoking a container.

This is a new disclosed stage: `c7a-builtin-container-stream`, runner/service
policy **10 -> 11**, one exact schema enum extension and **133** complete snapshot
records. `container_c7a_stream_transition.py` derives it independently after every
prior immutable stage. The C6b DS4 schema reader now pins its historical commit,
as earlier stages already do; its manifest and assertions are unchanged. The
census adds `ContainerC7aTest`, preserving all 157 existing tests and requiring
its aggregate build dependency. No comparator exception is introduced.

The compiled mutants in `test/container_c7a_mutations.py` must exit normally with
status 1 and the named assertion: external stream escape, missing execution
grant, unrequested forwarding, missing policy seal, and lost legacy forwarding.
Compilation failures, hangs and crashes do not count as kills. Evidence lives in
`.synagent/artifacts/container-v2/c7a/`, scratch in
`.synagent/scratch/container-v2/c7a/`.

## V2 builtin load-log binding

`launch.builtin_load_log: "halogen_starting_v1"` is a closed binding to
`BuiltinLoadLog::starting`. It requires a builtin container execution owner and
`model_contract: "halogen_hgn_v1"`; its selection, model contract and log category
are sealed. External local definitions and ungranted builtin containers cannot
compile it. It accepts no arbitrary text, phase, expression, callback or hook ID.
The v2 factory now derives its optional compatibility binding from the sealed
runner; callers cannot attach a callback to that factory.

`ConfiguredServer::load` invokes the bound starting callback after successful
preparation and before constructing/starting the process. Thus an artifact,
port or image-installation failure emits no starting line, while a later process
creation/start failure retains it. The exact source line is `Starting <model>
on port <port>` at INFO in category Halogen, without a generic loading/options
line. Existing v1 logging and v2 `loading_and_options` remain unchanged.

`test/fixtures/backend-v2/c7a.log.json` retains the C6b HGN fixture and the exact
`cbbeeb5270b19e06cfd9c150233655776392bd38` Halogen command/log/start source
quotation. `c7a_load_log.h` runs the actual v2 factory and `ConfiguredServer`
path with fake launch services and an intentionally failing fake process
factory, comparing the entire ordered event sequence. No runtime is invoked.

The new `c7a-builtin-load-log` stage discloses runner/service policy **11 -> 12**,
one schema binding addition and **133** snapshot records. The prior streaming
stage now reads its pinned committed schema; its manifest is unchanged. Named
compiled mutations cover missing binding, early line, line lost on failure,
missing owner seal and external binding access.
