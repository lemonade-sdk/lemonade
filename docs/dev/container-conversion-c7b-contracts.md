# C7b diagnostics and rewrite domain

C7b closes the remaining data contracts identified in the five C7 stop reports.
It adds no production engine, registration, pin, model row or execution grant.
The seven v1 definitions, both builtin policy implementations, `llamacpp@1`,
Gufo and all thirteen earlier transition manifests remain byte-identical to
`957c3fc76ec8ddf35a589121761d69681a09a572`.

## Diagnostics

`launch.diagnostics` is an ordered array of records keyed by `id`. Each record
has `id`, `phase`, `level`, `category`, `format`, and optional `when` (default
`always`). Keyed composition uses the existing add/before/after/match machinery.
The schema rejects extra properties. The compiler independently validates the
closed domains, format catalog, placeholder types, phase dependencies, unique
IDs, record count and phase order.

Phases are `loading`, `backend`, `artifacts`, `arguments`, `custom_args`,
`starting`, `success`, and `failure`. Loading follows platform admission;
backend/artifacts follow their successful preparation stages; arguments precedes
final custom-argument validation; custom_args follows that validation; starting
follows complete preparation and precedes process creation. Success follows
`start_server`, including readiness. Failure covers process creation/start and
readiness failures, after starting; preparation failures do not reach it.
For an explicit diagnostics record set, the existing `install` preparation
phase installs the container image at that point and suppresses the late second
installation. Thus image-installation failure retains only the source's earlier
loading/backend lines; a later port failure retains the GGUF line. Definitions
without an explicit record set retain their earlier installation timing. Halogen's
new fixture omits that phase and preserves C7a timing.
Records at the same phase retain their array order. The source's multiline llama
GPU diagnostic retains its embedded newline and explicit `[LlamaCpp]` prefix.

Levels are `debug`, `info`, `warning`, `error`. Categories are `RocmFpx`,
`LlamaCpp`, `Halogen`, `DS4`, and must match the definition's log category.
Conditions are `always`, `main_present`, `custom_present`, `embedding`,
`reranking`, `mmproj_cpu`. They select existing launch facts, not expressions.

The thirteen format strings are a closed schema enum and compiler catalog.
Their only placeholders are:

| Placeholder | Existing input or prepared fact |
|---|---|
| `{model:string}` | Model name |
| `{settings:options}` | Original `RecipeOptions::to_log_string()` |
| `{backend:string}` | Selected backend |
| `{main:path}` | Resolved main artifact, retained before container path rewriting |
| `{gpu:boolean}` | Selected backend's GPU decision, rendered `true`/`false` |
| `{custom:string}` | Validated selected custom-argument string |
| `{image:string}` | Resolved and validated digest-only container image |
| `{port:integer}` | Readiness command's allocated port |
| `{loaded_port:integer}` | That port after successful process start/readiness |

These are fixed typed projections of facts already used by the launch pipeline.
There is no user-specified JSON pointer, environment lookup, format language,
callback or hook identifier. The internal projections are not expression roots.
There are at most 32 records, IDs at most 64 characters, format strings at most
256 characters, each substituted fact at most 16,384 bytes, and each rendered
record at most 32,768 bytes. Oversize values fail rather than truncate. A record
is rendered completely before emitting it.

Compilation requires builtin provenance, container capability and the exact
private container execution owner. That owner seals the records, their order,
preparation order, category, load-log policy, option contracts, custom-argument
binding/defaults, ports and readiness. Existing owner fields already seal the
backend, artifacts, image policy, environment and installation. Optional
custom-argument defaults are canonicalized in the owner as in compilation.
External local definitions, external containers and ungranted builtins cannot
obtain the diagnostics execution path. Factory and runner authority checks
remain in place.

The historical `builtin_load_log: "halogen_starting_v1"` schema spelling remains
accepted for immutable C7a fixtures. Compilation lowers it to the same single
`starting` record used by the new Halogen fixture. There is no Halogen logging
branch in the runner. Selecting the old spelling together with diagnostics is
rejected, as is mixing diagnostics with `loading_and_options`.

The factory derives the compatibility callbacks from the compiled record set.
Existing v1 callbacks and the v1 load path are unchanged. New success/failure
callbacks are invoked only by the v2 load path.

## Rewrite domain

`operations[].request.rewrite_domain` has exactly two values:
`canonical_path` (the default when absent) and `chat_completions_substring`.
Selecting the field requires a granted builtin container owner. It is valid only
for the chat operation with `ds4_no_think_v1`. The substring value additionally
requires `builtin_container_legacy`, identity request mapping and no additional
request mapping or request hooks.

The substring value implements the pinned DS4 predicate literally:
`endpoint.find("chat/completions") != std::string::npos`. It therefore includes
query-bearing paths, alternate prefixes/suffixes and a match inside a query.
It provides no configurable matcher, regular expression or arbitrary path rule.
Unrelated paths retain their original bytes. The existing safe-relative-endpoint
confinement still rejects authority URLs, double-slash prefixes, CR/LF and NUL.
Malformed JSON passes through; endpoint, SSE and timeout controls are retained.
The owner seals the selected domain. Definitions without the field retain their
previous canonical-path behavior, including `llamacpp@1`.

## Source fixtures

Each `test/fixtures/backend-v2/c7b.<engine>.json` contains an independently
reviewed synthetic definition, literal expected lines, its rewrite domain, and
exact load/stream quotations from its pinned source. `container_c7b_sources.py`
checks the quotations using `git show`, never by checking out a source branch.
The C7b extension to `ContainerC7aTest` executes the real factory/load path with
fake launch services, a fake container host/client and ephemeral loopback
readiness. It checks full ordered diagnostics on success and process failure,
conditional embedding/reranking lines, empty custom arguments, byte bounds,
closed fields, keyed composition, owner seals and HTTP rewrite bytes. Halogen's
new and C7a spellings are additionally compared byte-for-byte with identical
ordered preparation/process events. All original C7a assertions remain.

| Engine | Pin | Diagnostic text, in source order |
|---|---|---|
| ROCm FPX | `e3d734e056d1caada9efc4a2c8b218c23e0016b2` | `Loading model: <model>`; `Model loaded on port <port>` |
| Halo Box | `23e953915c96d2b2a039a0b3ffdfae46f97301d5` | `Loading model: <model>`; `Per-model settings: <options>`; `Using LlamaCpp Backend: <backend>`; `Using GGUF: <path>`; `Using backend: <backend>` plus `[LlamaCpp] Use GPU: true`; conditional mmproj/embedding/reranking lines; `Adding custom arguments: <args>` when nonempty; `Starting llama-server...`; `Model loaded on port <port>` |
| Nathanw | `d491c946fed2ab079f7387cf02a0eaabb51afce4` | Same llama diagnostics, with the `nathanw` selector retained |
| Halogen | `cbbeeb5270b19e06cfd9c150233655776392bd38` | `Starting <model> on port <port>` |
| OCI DS4 | `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78` | `Adding custom arguments: <args>` when nonempty; `Starting ds4-server (<image>) for <path> on port <port>` |

The conditional llama text is exactly `Skipping mmproj argument since GPU mode
is not enabled`, `Model supports embeddings, adding --embeddings flag`, and
`Model supports reranking, adding --reranking flag`. The GPU-only container
fixtures omit the CPU mmproj line. Both model-mode branches are exercised.

ROCm FPX, Halo Box, Nathanw and Halogen retain the canonical rewrite domain.
DS4 exercises its substring domain and a separate absent-field control proving
that the default remains canonical. Wire fixtures include valid/malformed JSON,
query-bearing chat, substring-bearing alternate paths, unrelated paths and
endpoint-confinement failures.

## Identity disclosure and validation

One new stage, `c7b-diagnostics-rewrite-domain`, advances runner/service policy
**13 -> 14**. Its exact schema changes are three additions: `$defs/Diagnostic`,
`$defs/Launch/properties/diagnostics`, and
`$defs/Request/properties/rewrite_domain`. It discloses 133 complete snapshot
records. `container_c7b_transition.py` pins the previous schema to C7a's final
commit and derives the new stage independently. The old C7a budget reader now
reads that historical schema explicitly; no prior manifest was regenerated.
There is no comparator exception or normalization of a prior snapshot.

The CI suite count stays at 158; C7b extends the existing aggregate dependency.
`test/container_c7b_mutations.py` requires a successful compile/link, normal
exit 1 and the named failing assertion for each mutant. Header-validator mutants
use an isolated static-inline copy so the linker cannot select an unchanged
weak implementation. Previous mutation suites and all fourteen transition
stages are replayed during final qualification. Evidence and scratch belong in
`.synagent/artifacts/container-v2/c7b/` and
`.synagent/scratch/container-v2/c7b/` respectively.

All five engines' identified C7/C7a data-contract blockers are now expressible.
Their production migrations, reviewed execution owners/pins, full engine parity
and real container/model/platform qualification remain work for the next unit.
CPU fixtures do not qualify OCI registries, devices, model performance or actual
Windows/macOS builds. No production engine is migrated by C7b.
