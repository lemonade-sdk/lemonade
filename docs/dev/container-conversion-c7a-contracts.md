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

## Prelaunch DS4 automatic-context budget

The new closed `model_contract: "ds4_rocm_v1"` requires a builtin execution owner,
a single fixed ROCm container case with `memory_plan: "ds4_rocm_v1"`, and the
bound main GGUF artifact reader. The existing owner seals model-contract
selection and the launch memory plan. External containers fail authorization;
local definitions cannot satisfy the compiler's builtin/container grant check.
No production DS4 definition or grant is changed.

`BackendOps::auto_context_kv_budget_gib` defaults to nullopt. V2 supplies it only
for this model contract. The runner collects the bounded typed integrated-pool
and main/draft byte facts before launch, without probing free memory again,
installing an image, allocating a port or starting a process. It uses the same
resident/streaming/reserve computation as launch, returning nullopt for an
unknown working set and retaining a negative source budget when capacity is
insufficient. Missing facts and nonfinite/out-of-range facts fail closed;
observed stat failure is explicit zero. The automatic-context estimator uses a
supplied KV budget directly, bounds it by available memory, and retains draft
and MTP KV accounting. The generic path remains unchanged on nullopt.

For the C7 counterexample, pool 62 GiB, weights 64 GiB, streaming working set
8 GiB and available 50 GiB produce reserve 37 GiB and KV budget **5 GiB**. The
fixture's 128 KiB/token metadata produces **40960 tokens**, where the generic
path still produces **0**. These are CPU arithmetic fixtures, not measurements.
The model contract also preserves registered capability labels while applying
typed GGUF metadata, reproducing the source metadata override. Its internal
metadata-consumer test uses typed synthetic results and creates no GGUF file.

`test/fixtures/backend-v2/c7a.budget.json` retains the exact OCI DS4
`8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78` planning quotations and adds the
metadata/budget overrides at lines 244-262 of
`src/cpp/server/backends/ds4/ds4_server.cpp`. It covers resident/streaming/draft,
nullopt, negative capacity, stat failure, input bounds and no prelaunch effects.
The new `c7a-prelaunch-ds4-budget` stage discloses runner/service policy
**12 -> 13**, one model-contract enum extension and **133** snapshot records.
The prior log stage now reads its pinned schema; its manifest is unchanged.

Named compiled mutants remove the ops seam, ignore it in the real estimator,
drop the reserve/nullopt/profile/grant/owner, overwrite registered labels, or
ignore draft weights. The estimator mutant must fail
`C7aEstimatorUsesEarlierBudgetInsteadOfZero`; a rejected build is not a kill.

## Migration readiness

**All five are not yet qualified for complete literal source parity.** C7a
implements the three requested contracts, and migrates none of the engines.
Halogen's named C7 blocker is now expressible; its production definition,
reviewed owner, pin inventory and full engine parity still need a migration unit.

ROCm FPX, Halo Box and Nathanw now have the requested inherited direct-stream
surface. Their source-specific load diagnostics, including `Model loaded on
port ...` after successful start, are not supplied by the new Halogen-only log
binding. The captured ROCm FPX source emits that success line at
`e3d734e056d1caada9efc4a2c8b218c23e0016b2:src/cpp/server/backends/rocmfpx/rocmfpx_server.cpp:143`;
Halo Box emits it at
`23e953915c96d2b2a039a0b3ffdfae46f97301d5:src/cpp/server/backends/llamacpp/llamacpp_server.cpp:464`,
and Nathanw at the corresponding pinned source line 462. Those require their
own closed log bindings and success-phase handling before a full-parity claim.

OCI DS4's budget and metadata-label requirements are now expressible. Its
image/path-bearing startup and optional custom-argument diagnostics still need
a closed log binding. Its captured direct-stream method also tests
`endpoint.find("chat/completions")`, so query-bearing chat endpoints receive
the no-think rewrite. The new llama-derived stream profile intentionally retains
llama's exact canonical-path transform semantics; C6b DS4's bound operation
fixture proves only canonical-path rewriting. Full DS4 migration must additionally
express and qualify that query-path transform domain. This source quotation is
retained in `c7a.budget.json`; no existing bounded fixture is widened or relabeled.

Real OCI, device, model, registry-digest and packaging qualification remains
outside these CPU fixtures. Gufo remains the only inactive container engine
JSON in production resources.

The full prior execution mutation replay additionally required isolating test
phases from unexpected exceptions in `test_container_execution.cpp`. C6's earlier
device fixture can encounter a managed/external program mutant before the
original `ContainerDefinitionActuallyExecutes` check. Each phase now records
that exception as a failure and continues, allowing the original assertion to
run as well. Every prior assertion and expected value is retained; the trusted
suite still executes 199 checks. The mutation runner's named assertions are
unchanged. Reused mutation objects are checked against the final production
source text and relinked against the final test objects; missing objects are
compiled before replay. Compile/link failures and timeouts never count as kills.
