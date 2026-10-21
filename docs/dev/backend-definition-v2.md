# Backend definition v2 composition contract

V2 is a data contract for resolving one immutable source snapshot. It is not
currently admitted by the production catalog, CLI validator, or configured server.
V1 remains authoritative for all shipped definitions, descriptors, feature output,
and the default `/v1/backends/schema` export. Successful v2 structural checking or
resolution does not authorize hooks or establish executable support.

## Schema and declaration boundary

`schemas/backend-definition-v2.schema.json` is self-contained. Its main root
validates the composition envelope; `#/$defs/Declaration` validates the complete
materialized declaration. All references are local. Unchanged v1 payload definitions
were copied into v2; the v1 schema was neither factored nor extended. In particular,
extending a closed v1 record with `allOf` would still reject added properties.

The v2 checker implements every validation keyword used by its schema: local
`$ref`, `anyOf`, `oneOf`, `type`, `const`, `enum`, `minLength`, `maxLength`, `pattern`,
`minimum`, `maximum`, `minItems`, `maxItems`, `uniqueItems`, `properties`, `required`,
and `additionalProperties`. `$schema`, `$id`, and `$defs` provide schema metadata
and reference storage. Keyword auditing rejects unsupported validation keywords;
the checker is not a general Draft 2020-12 implementation. String length is counted
in Unicode code points; composition pointers also have a stricter UTF-8 byte bound.

`ResolvedDefinition` and `BackendDefinition` are distinct types. The former has no
executable programs, function pointers, grants, compatibility attachments, or
inserted defaults. It cannot be silently passed to `ConfiguredBackendEntry::make`.
Resolution never strips record IDs to send the result through the v1 compiler.
Compilation in a later increment must use a separate copy of the declaration.

## Envelope

The closed envelope contains required integer `schema_version: 2`, `id`, and
`recipe`, and optional `extends`, `merge`, and `edits`. Identifiers match
`^[a-z][a-z0-9._-]*$`. Both identity fields belong to the leaf and are never
inherited. `merge` defaults to `{}` and `edits` defaults to `[]`.

`extends` has exactly one of these closed forms:

```json
{"builtin":"llamacpp","contract":1,"expected_digest":"sha256:..."}
```

```json
{"external":"local-base","expected_digest":"sha256:..."}
```

`expected_digest` is optional and, when present, is `sha256:` followed by 64
lowercase hexadecimal digits. It compares the parent's materialized declaration
hash. Built-in contracts are integers from 1 through 4,294,967,295. External
references have no contract member. No filename, URL, search path, environment
substitution, or multiple-parent form is supported.

Source namespace and contract are loader-owned facts. Built-in source-table keys
require a positive contract; external keys require contract zero. Envelope `id`
must equal its source-table key. JSON cannot claim loader authority. Only v2
sources can participate in the v2 graph. The data-only resolver owns snapshots, dependency lookup, cycle checking, and
source admission.

`merge` is a partial object restricted to these payload members: `kind`,
`metadata`, `install`, `artifacts`, `launch`, `operations`, `hooks`,
`unsupported_messages`, and `rationale`. Nested fragments may be incomplete;
complete validation occurs after composition. Neither `merge` nor edits may
change `schema_version`, `id`, `recipe`, or composition-control fields.

## Merge algebra

Resolution first obtains the parent's declared payload, applies `merge`, then
executes edits in source order. It reconstructs the complete object using the
leaf's identity and version. Defaults are not part of this process.

| Input | Result |
| --- | --- |
| Omitted member | Inherit the existing value |
| Object over object | Recursively merge members |
| Array over any value | Replace the whole array, preserving supplied order |
| Scalar over any value | Replace, preserving JSON type |
| Object over a non-object | Replace with the supplied object |
| `null` | Store literal null |
| `{}` over an object | Keep inherited members |
| `[]` | Replace with the empty array |

Objects inside `literal`, option `default_value`, and metadata `config_extra`
remain data. Operator-looking values do not perform edits. Null never means delete.

## Stable record identities

| Array pointer | Identity field |
| --- | --- |
| `/metadata/options` | `id` |
| `/metadata/support` | `id` |
| `/install/cases` | `id` |
| `/launch/argv` | `id` |
| `/launch/custom_args/defaults` | `id` |
| `/operations` | `id` |
| `/launch/ports` | `name` |
| `/launch/environment/set` | `name` |

Each record structurally requires the correct field and type. Identity uniqueness
is a resolver relationship check, not `uniqueItems`: records with different data
can still have the same ID. A public option `name` is independent of its stable
`id`. Backend/custom-argument selectors and `{ "option": "device" }` expressions
refer to stable option IDs. The latter has optional `type` and `default` members.
Other arrays, including hook lists, expression operands, aliases, model modes,
and preparation order, support whole-array replacement only.

## Ordered edits and preconditions

Every edit is a closed object. `op` and a nonempty `path` are required. Pointers
use RFC 6901 escaping (`~0` and `~1`); empty object keys and numeric object keys
are legal. Numeric array traversal and `-` append syntax are forbidden. Intermediate
objects are never implicitly created. `path` starts at the payload root.

`match`, when supplied, is exactly `{ "id": Identifier }` or
`{ "name": NonemptyString }`. It selects exactly one record in a registered
array and must use that array's identity field. Optional `at` is a nonempty pointer
relative to that record and requires `match`. Traversing from a selected record
into an array by numeric index is still forbidden.

| Operator | Value | Required target state | Effect |
| --- | --- | --- | --- |
| `merge` | Required object | Existing object | Recursive merge |
| `replace` | Required arbitrary JSON | Existing member/record | Whole-value replacement |
| `clear` | Forbidden | Existing object/array | Empty, retaining container type |
| `remove` | Forbidden | Existing member/record | Delete |
| `add` | Required arbitrary JSON | Absent member with existing object parent, or registered array | Create member or insert complete record |

Adding a record requires a fresh identity. Array insertion appends unless `before`
or `after` selects an existing record. Anchors must match exactly once and use
the correct identity field. They are mutually exclusive and only legal on an
unmatched `add`. A matched `add` requires nonempty `at` and adds an absent member
inside that record.

A selected record's identity cannot be changed, removed, or cleared, including
through whole-record replacement. Rename records with remove/add. Whole-array
replacement is allowed and authors all supplied records anew.

Edits execute sequentially with explicit preconditions. Removing a member then
replacing it fails with `missing_target`; the error relates the removal event.
Repeated replacements succeed and the last value wins. Replacing a container then
adding a member inside it is valid. There is no blanket overlap prohibition.
Failure publishes no result and cannot mutate a cached parent or an earlier result.

## Complete declaration additions

The closed declaration requires `schema_version:2`, `id`, `recipe`, `kind:"local"`,
`metadata`, `install`, `launch`, and at least one operation. Metadata requires
`display_name`, `server_name`, and `binary`. Remote execution is not accepted.
Other payload constraints retain the v1 vocabulary with the following adaptations:

- Required stable IDs on the six arrays listed above.
- Option expressions using stable option IDs.
- Raw argv `{id,args,when?}` or flag argv
  `{id,flag,value?,when?,aliases?,reserve?}`. The forms are mutually exclusive.
  Reservation is `always`, `when_active`, or `never`; compilation later defaults
  it to `always`. Omitted conditions later default to active.
- Optional artifact `required_when` expressions.
- Custom-argument defaults require `id`, `stage` (`runtime_options` or `launch`),
  and `respect_merge_args`, with optional `when`.
- Environment preset `hook`; an OS-specific cwd map requiring Windows, Linux,
  and macOS entries; and `context_floor`, a closed map of canonical model types
  (`llm`, `embedding`, `reranking`, `transcription`, `image`, `tts`,
  `audio-generation`, `classification`, `mesh`) to nonnegative integers.
- Root hook slots `backend`, `install_context`, `runtime_options`, `environment`,
  and `system`, alongside the existing slots. Structural hook acceptance does
  not assert installed hook availability or authorization.
- Rationale targets have `subject`, optional `match`, and optional `at` requiring
  `match`. Empty `subject` selects the whole declaration. Numeric traversal into
  arrays is rejected; whole-array subjects remain legal. Record-aware targets
  preserve the intended referent across insertion or reordering.

Pure relationship validation must cover keyed identities, option-ID references,
public-name/config-key uniqueness, rationale referents, and composition phase
relationships. Installed hook lookup and executable adapter binding attach later.

## Provenance and diagnostics

The resolver API retains source locations, record-aware targets, deletion
locations, and an authorship journal. An equal-value replacement is newly authored,
even though it preserves effective bytes. This distinction is necessary for later
hook authorization. Removed and re-added records have new authorship. Provenance
must track logical records rather than numeric array positions.

Diagnostics contain `code`, symbolic `definition`, source `pointer`, `message`,
optional `effective_pointer` and `origin`, and related source locations. An invalid
child edit points inside `/edits/N/value`; inherited failures point to the parent;
removed required members point to the removal. Missing dependencies and cycles
point to `/extends`. Messages never include local labels, server paths, or raw
secret values. Diagnostics sort by symbolic definition, source pointer, and code;
retain at most 100 entries with a separate `truncated` flag. Related locations are
also charged before retention. Budget failure never returns a definition.

## Bounds

| Resource | Limit | Admission point |
| --- | --- | --- |
| One source | 1 MiB | Before copying caller bytes |
| Chain, including leaf | 8 documents | Before parent descent; cycles checked first |
| Unique closure source bytes | 8 MiB | Before retaining/parsing a new closure source |
| Source nesting | 64 | Before parser container push/DOM construction |
| Closure source nodes | 262,144 | Before node construction |
| Edits per document | 1,024 | Before edit-array retention |
| Pointer UTF-8 bytes | 4,096 | Before decoding, tokenization, or joining |
| Effective nesting | 64 | Before subtree insertion/copy |
| Effective compact bytes/nodes | 1 MiB / 65,536 | Before parent clone and each mutation |
| Cumulative payload work/visits | 16 MiB / 1,048,576 | Before each traversal/copy/serialization contribution |
| Snapshot documents/source bytes | 1,024 / 64 MiB | Before ownership/index growth |
| Retained resolved/provenance logical data | 128 MiB | Before cache, origin, journal, and dependency insertion |

Use `increment > limit - used` checked arithmetic. Bounded source preflight must
precede DOM construction and reject duplicate keys. SAX callbacks alone do not
bound the nlohmann lexer string scratch allocated before callbacks; source admission
and preflight must bound scratch too. Cache subtree nodes/depth/exact compact bytes,
including escaped keys and delimiters. Memoized parents still incur each child's
logical output and work charges. Removal does not refund cumulative work. The
128 MiB limit concerns logical data, not process RSS. These new bounds do not alter
production v1 admission. The bounds tests instrument DOM, clone, origin, and pointer admission seams.

## Identity serialization

Canonical means the tested nlohmann JSON compact serialization policy: sorted
object keys, preserved array order, explicit field presence, and its numeric
serialization. This is not a claim of JCS compliance. Reject nonfinite numbers and
integer tokens that overflow into floating representation. Test `1` versus `1.0`,
signed/unsigned boundaries, exponent spellings, Unicode, and negative zero.

```text
source_digest = SHA256("lemonade.backend-source.v2\n" + canonical(envelope))
definition_hash = SHA256("lemonade.backend-definition.v2\n" + canonical(declaration))
dependency_digest = SHA256("lemonade.backend-dependencies.v2\n" + canonical({
  "resolver_revision": 1,
  "leaf_source_digest": source_digest,
  "parents": [root-to-immediate-parent identities]
}))
```

Parent identities contain loader-owned document keys, source digests, and
materialized definition hashes. Formatting changes preserve all canonical hashes.
Overridden parent changes may preserve the effective hash while changing dependency
identity. Different edit histories with equal output retain equal definition hashes
and distinct source identities. V1's serialization and hashes remain unchanged.

## Worked artifacts and later attachment

`test/fixtures/backend-v2/llamacpp.example.json` and `hrx.example.json` reproduce the
plan's C2 and corrected C3. The llama.cpp sketch contains eleven protocol records
but only one sample support/release case. Its support matrix, release cases,
runtime defaults, exact policy errors, and new typed hook implementations remain
port work. HRX removes the context floor and vanilla-only hook references; its
replace-environment/add-variable sequence is intentionally valid ordered editing.
Neither file is a production definition or an authorized external hook consumer.

`algebra.json` retains both positive worked cases and the numeric-index,
replace-after-remove, and missing-anchor rejections with exact diagnostics.
`contract.json` contains independent structural acceptance/rejection cases.

Phase 3 must attach the loader-owned export registry, hook grants and sealed-slot
authorization, catalog graph admission, and representation negotiation. Phase 4
must attach executable compilation, option binding/defaults on a separate copy,
typed hooks, descriptor initializers independent of registration/factories, and the
llama.cpp port. No source or provenance object can serialize authority. CLI
materialization, production definition conversion, and real backend/platform
qualification remain later increments.

## Data resolver implementation

`make_v2_source_snapshot` preflights admitted byte views with SAX, rejects decoded
key duplicates, and validates the envelope grammar using control-only scratch
records. It owns source bytes and loader keys, without retaining local labels.
The lexer scratch and duplicate-key sets are bounded by raw source admission;
no unbounded token can precede that check. Dependency closure limits are checked
before constructing payload DOMs. A resolver with lower limits rechecks source
admission before using its snapshot.

`DefinitionResolver::resolve` uses a single-parent dependency walk and a private
transaction cache. Its immutable nodes cache exact compact bytes, nodes, and depth.
Object merge and edits project the complete new root before constructing changed
nodes. Unaffected nodes and their compact origin chains remain shared. Published
results contain an independent JSON declaration and immutable provenance; failed
children cannot modify parents. Retention accounting conservatively charges
admitted scratch, copies, indexes, dependency data, journal records, and cached
results as logical data. It does not estimate allocator overhead or promise an RSS
ceiling. Cache hits still charge the transaction's logical output budget.

`origin_at` accepts effective JSON pointers, including numeric positions for
inspection. Editing and rationale resolution forbid numeric array traversal.
Origins follow nodes across array insertion/removal, while tombstones cover
explicit removals, clear/replace operations, and container replacement during
merge. Re-authored data takes precedence over an older removal when reporting a
missing descendant. The authorship journal includes equal-value replacements.
Diagnostics and related locations share the diagnostic record budget.

The structural checker uses linear implementations of the four shipped string
patterns; the schema audit rejects unknown patterns as well as unsupported
keywords. Relationship checking follows schema expression locations and leaves
arbitrary JSON values opaque. It checks stable/public/config-key identities,
option references, rationale targets, preparation dependencies, environment-hook
presence, release-case presence, and declared readiness/watchdog ports. Hook lookup,
authorization, expression compilation, and executable adapter checks attach later.

`descriptor_from_declaration(declaration, origin, platform)` is a pure data mapper.
It rejects unknown enums and OS names. Production callers and generated v1 objects
remain on their original paths. The separate CMake v2 initializer emitter is used
only by an embedded synthetic fixture. Its private resolution cache depends on
embedded source/schema data and the resolver, with no registry/factory recursion.

## Compiled export registry

The first phase-3 unit adds the data-only `BuiltinExportRegistry` and
`HookAbiDescriptor` tables. Production exports are empty. In particular,
`llamacpp@1` is not installed as an export. The example definitions remain
composition examples and cannot supply authority. Resolution, including a
caller-supplied `DefinitionNamespace::Builtin`, is still a data operation.

Export keys consist of a builtin ID and a positive contract major. Lookup is
exact; there is no version fallback. Registry construction is private to the
loader. The synthetic registry and trusted snapshot constructors are defined only in
the export and authorization test executables, and is absent from the production library. External JSON cannot
provide or replace a registry entry.

Each contract pins the canonical source, declaration and dependency digests, a
positive policy revision, and the complete policy digest. Each exported slot
identifies a root slot, stable operation ID or artifact key, its stage and hook
ID. Its policy binds the installed ABI revision, parameter digest, ordered hook
sequence, semantic owner digest, removal rule, slot and phase dependencies,
sealed inputs, typed mutable inputs, and allowed effects. Every hook selection
in the base must have exactly one policy. Duplicate hook IDs in one owner/stage,
uncovered selections, cyclic dependencies and contradictory data policies fail
the audit.

Every export also pins a complete operation-owner graph, including hook-free
operations. Each record contains the stable ID, operation kind, delivery, model
modes, transport, formats and format metadata, explicit request/response/handler
chains (including empty chains), removal permission, owner dependencies, sealed
owning fields and collision relationships. Audit checks this graph against the
pinned base independently of hook-slot coverage. Missing owners, changed chains,
incomplete collision relationships and dependency cycles reject.

Mutable scalar inputs declare a type, optional range, string byte bound, enum
and permitted absence. Structured input rules use the audited `Expression`
schema fragment and explicit reference roots; arbitrary policy expressions are
not supported. Literal expression payloads and reference defaults remain data.
Hook selections and operation owner semantics cannot be declared mutable.
These records are inputs to the authorizer; the registry audit alone
does not authorize an external declaration.

Identity domains are distinct:

| Identity | Domain prefix, followed by canonical JSON |
|---|---|
| Parameters | `lemonade.backend-hook-parameters.v1\n` |
| Owner semantics | `lemonade.backend-hook-owner.v1\n` |
| Operation-owner graph | `lemonade.backend-operation-owners.v1\n` |
| Mutation policy | `lemonade.backend-hook-mutation-policy.v1\n` |
| Export policy | `lemonade.backend-export-policy.v1\n` |
| Registry | `lemonade.backend-export-registry.v1\n` |
| Installed ABI metadata | `lemonade.backend-hook-abi.v1\n` |

The registry identity binds sorted export identities and the installed ABI
metadata, including implementation revisions and operation restrictions. The
production aggregate digest is a committed literal in
`backend_definition_exports.cpp`; normal builds do not regenerate it. Test
source, definition, dependency, policy and aggregate pins are committed in
`test/fixtures/backend-v2/export.synthetic.pins.json`. The original resolver
domains and fixtures are unchanged. Filesystem labels, function pointers and
execution defaults do not enter these identities.

The server compares every legacy executable hook registration with the exact
twelve-record legacy ABI projection before returning the installed hook table.
The complete data-only inventory contains nineteen records; the seven additional
records describe the v2 backend, install-context, runtime-options, launch/prepare,
environment, system and narrow artifact slots. They do not install callable
targets. `hook-abi.inventory.json` pins both inventories, the ABI digest and the
production export-registry aggregate. These audits call no hook targets. An ABI revision change requires a new
contract major for every dependent export. Incompatible slot meaning, removal
obligations, parameter shape, input surface or effects also require a new major.
A compatible implementation correction changes the implementation identity;
a compatible data update changes the base identities. Old majors can coexist
only as separately pinned and audited entries. Removing a major provides no
fallback for its children.

The legacy installed feature manifest is unchanged. Exporting an existing hook
adds authorization permission, rather than installing a new executable
primitive. A later, separate admission manifest must advertise that permission.
The authorization unit described below consumes these records. Mixed-version
catalog admission, candidate graph validation and representation negotiation
remain pending; production admission and executable validation are still
v1-only. No v2 registration or factory is constructed by this unit.


## Loader authority and sealed slots

`load_v2_authority_snapshot(external_sources, limits)` owns a frozen source index
and a copy of the installed compiled registry. Its intake rejects builtin keys
and nonzero external contract numbers. Only `BuiltinCatalogLoader` constructs
`FrozenDefinitionSnapshot`; it audits every compiled export against the owned
sources. Production has no bundled v2 exports, including `llamacpp@1`.
Synthetic trusted intake is compiled only into the tests.

`DefinitionAuthorizer(snapshot).authorize(leaf)` accepts an external key, never a
caller-built `ResolvedDefinition` or pure `DefinitionSourceSnapshot`. It first
runs the unchanged pure resolver, including declaration hashing, and then
checks every external layer. Success creates an immutable `AuthorizedDefinition`
containing the exact resolved object and an opaque `ResolvedAuthority`. Neither
authority nor authorized results have public constructors or JSON serializers.
Copying declaration data or selecting the builtin namespace cannot produce a
token. Each token retains its frozen snapshot, registry identity, exact resolved
object and authorization revision.

A private grant stores the builtin ID and contract major, source, declaration
and dependency digests, logical owner, stage, hook ID, ABI revision, parameter
digest and mutation policy digest. The original root and policy remain attached
through external descendants. The next layer receives only its immediate
parent's surviving grants. Removed mandatory-owner obligations remain in force,
so protected behavior cannot be recreated under a new or re-authored owner,
whether or not the hook is retained.

The private authored-value observer is part of the existing edit engine. After
pure resolution, authorization parses each frozen external envelope again and
replays its merge and ordered edits through that engine. Parsing, replay,
inspection, projections and retained grants share the resolver's remaining work
and retention budgets. The observer examines original values at schema hook
positions and direct hook-field targets, including selections overwritten by
later edits. Protected operation and artifact owner fields also reject authored
writes. Request literals remain ordinary data. Source values and local labels
never enter the authorship journal, authorization diagnostics or catalog rows.
The value-free journal alone cannot distinguish a hidden hook selection from
an ordinary overwritten value.

Final verification requires an exact inherited slot and installed ABI, sealed
parameters, owner semantics, ordered hook sequence and recursively preserved
hook provenance. Replacing an identical hook or an enclosing subtree does not
preserve provenance. Each surviving grant checks explicit sealed inputs, typed
mutable values and the remaining declaration data. Existing data is sealed by
default; the identity fields and rationale are excluded, and operations are
compared by stable ID. Inserting an unrelated operation preserves existing
slots. Mutable inputs must satisfy every retained policy that consumes them.

Removing a mandatory hook while its owner remains rejects. Removing an operation
owner requires explicit owner-removal permission and compatible slot policies.
A colliding inherited sibling is admissible only through its own audited owner
record, sealed-field provenance from the immediate parent, exact semantics and
ordered chains. Each retained hook must also retain its own exact slot grant.
A hook-free sibling needs its owner record and an audited empty chain. Sharing
a hook name never establishes owner identity.

New owners with the same public operation kind or backend endpoint reject,
including after the original owner and every hook grant have disappeared.
Endpoint comparison ignores delivery and HTTP envelope (JSON, SSE, bytes or
multipart). It establishes disjointness only for fixed, unambiguous HTTP
method/path/port triples; dynamic paths, noncanonical spellings and other
uncertain relationships get no permissive exemption. An independently audited
inherited endpoint may be dynamic. Artifact aliases with the same role continue
to reject. Owner obligations persist through all external descendants, and an
owner absent from an immediate parent cannot return.

| Action | Decision |
|---|---|
| Retain a same-kind or endpoint-sharing sibling | Allow only its separately audited surviving owner identity and exact retained grants. |
| Retain a hook-free sibling | Allow only its audited owner identity, sealed provenance and exact empty chains. |
| Remove an optional owner and retain its independent sibling | Allow when removal permission and dependencies hold. |
| Remove a protected owner and recreate its behavior under a new or re-authored owner, whether or not the hook is retained | Reject; owner removal cannot authorize a replacement. |

`Independent` slots may disappear only when all retained slot and phase
dependencies remain satisfied. Hook lists can be cleared when every removed
slot permits it; replacing a list with retained hooks re-authors those hooks.
Removing an independently removable launch hook may also remove its associated
`hook` preparation phase, as required by the pure relationship validator.

The final verifier enumerates all surviving selections after all edits and
checks the root obligations even for grants removed by an earlier external
parent. It never rebuilds a child's grants from the root export. Compatibility
attachments and serialized origin or authority controls are forbidden.
Authorization does not compile expressions, bind functions, apply executable
defaults, construct entries, validate executable support or publish a catalog.
Future executable consumers must accept the authorized token. The v1 store,
legacy metadata projections, schema export and hash domains remain unchanged.

The first 4c increment implements the owner graph and authorization revision 2.
The single-operation phase-3 fixture and all its negative assertions remain
unchanged. A separate synthetic sibling fixture audits the eleven frozen
llama.cpp cells against all four oracle views; streaming completion has empty
request and response chains.

The next 4c increment adds loader-owned builtin leaf authorization and the
nineteen-record ABI metadata inventory. Its production export-registry pin is
`sha256:6bac770f2a8d740927eb20a69a430d9430a0eabea744efedb74486afe941ead6`.
ABI metadata, including implementation revisions, contributes to this aggregate
even when the export set is empty. The twelve legacy records, source/declaration/
dependency hashes and v1 projection remain unchanged. Synthetic aggregate pins
change to include the expanded inventory; builds compare literal expectations.

Builtin v2 bytes and leaf execution policies have separate loader-owned embedded
tables in `backend_builtin_authorization.cpp`. Both production tables are empty.
The loader audits each leaf's source, resolved declaration, dependency closure,
owner graph and slot policy pins before freezing the context. Builtin closure
contains only embedded builtin sources. Internal sources can be composed without
exporting them; only leaves with an audited execution policy can receive a token.
The shared policy record shape does not grant external export permission.

Catalog admission mints a fresh builtin token for each admitted transaction.
`builtin_v2_definition(key)` retrieves that retained token with exact namespace
and contract-major matching. The public authorizer and source intake remain
external-only. A caller-created `ResolvedDefinition` cannot mint authority.
Builtin authorization identity revision 3 additionally binds source, builtin
policy and builtin policy-table identities; external revision 2 is unchanged.
Test-only constructors are compiled into test executables and are absent from
the production archive. Loader objects have no target, factory, service or IO
dependencies.

The mutable contracts, exact admission membership, private typed targets and
sealed compiler/binder described below complete 4c. Production exports and both
builtin v2 tables remain empty. Startup still rejects v2 declarations, and
candidates remain non-executable. Publication and runtime execution belong to 4d.

### Catalog admission and private candidates

`freeze_builtin_catalog()` captures the installed reservations, defaults, pins,
export registry and builtin authority context. `make_catalog_source_snapshot()`
indexes mixed v1/v2 external input with forced external origin. `admit_catalog()`
returns an immutable `AdmittedCatalog` only after every v2 source has resolved and
passed authorization and all ownership checks have passed. It creates no server
registrations. Startup rejects v2 execution with `unsupported_execution_version`.

External IDs cannot claim builtin definition, backend, section or installation
identities, including compiled-only backends. Recipes, configuration sections and
public option names have one owner. Persisted option keys use the runtime suffix
mapping (`_backend`, `_args`, `_device`). V2 `install_recipe` and `pin_recipe` are
absent or equal the leaf recipe. An inherited explicit alias must be removed or
replaced by the child. Existing valid v1 aliases remain supported and reserve
those identities against v2 claims, independent of source order.

All sources are indexed before resolution, so forward references work. An
external v1 parent produces `unsupported_parent_version`; an absent external
parent produces `missing_parent` at `/extends/external`. A builtin without an
installed export produces `unexported_builtin`; a different available contract
produces `contract_mismatch`. Validation never searches disk or retrieves a
missing dependency.

The catalog limits include builtin sources: 1,024 definitions, 64 MiB source
bytes and 128 MiB retained logical data. Discovery bounds the selected path list
and checks file allowances before reading. Intake checks the complete source
allowance before copying. Its ledger charges owned source copies, headers and
indices, v1 compiler allowance, dependency edges, v2 source snapshots, resolver
DOMs and provenance, dependencies, authorization replay and grants, ownership
sets and identity data. The v1 compiler allowance is conservative, so logical
admission can reject before a process reaches 128 MiB RSS; this is not an RSS
promise. Per-source, chain, work and diagnostic limits still apply. Public recipe
rows also have an independent limit of 1,024.

`validate_candidate_definition()` replaces only a matching external ID and recipe
in the captured source set and uses the same admission API as startup. Every
proposed graph is rebuilt and every descendant is reauthorized. No authority is
reused across snapshots. A failed parent pin, removed dependency or changed
policy rejects the entire proposal; the active object remains immutable. An
immediate-parent declaration pin does not pin transitive dependency or policy
identities.

The existing `POST backends/validate` route uses this private mechanism. A v2
candidate that passes admission still returns HTTP 400 and `valid:false`, with
`resolved`, `authorized` and `catalog_admitted` true and `executable` false. Its
only remaining diagnostic is `unsupported_execution_version`. A candidate is
never published. Responses allow logical identities, hashes and bounded safe
diagnostic pointers; they exclude source and resolved declarations, local labels,
server paths, command/environment values, exception text, grants and policies.

### Representation negotiation

`GET backends/schema` keeps its exact legacy body and ETag when `representation`
is omitted or is `1`. `representation=2` requests the admission view. Empty,
unknown and repeated values, including repeated identical values, return
`invalid_representation`. All four API prefixes and authentication apply.

The v2 view retains the executable `catalog_id` and adds a separate
`catalog_v2_id`, `accepted_definition_versions:[1,2]`,
`acceptance_stage:"declaration_authorization"`,
`executable_definition_versions:[1]`, resolver/authorizer/grant revisions and an
independently versioned `admission_manifest`. Its identity includes the v2 schema,
export-registry and hook-ABI identities and admission bounds. Export permissions
are represented by their registry identity, never by policy bodies or grant
records. The legacy feature manifest and its schema version are unchanged.

V2 recipe rows identify the actual input version: `1` for existing definitions
and `null` for compiled-only recipes. Candidates do not become recipe rows. The
ETag remains the digest of the exact serialized response, including its selected
representation and recipe filter. A legacy ETag cannot validate a v2 body.

## Immutable execution compilation (phase 4a)

`compile_execution_definition(shared_ptr<const AuthorizedDefinition>, limits)`
produces a non-runnable `CompiledDefinition`. Raw JSON, resolver results and v1
`BackendDefinition` objects cannot enter this API. The result retains the exact
authorization token and exposes const views of a separate execution copy. It has
no backend factory, hook target, executable-valid marker or conversion to v1.

The compiler preflights the authored declaration and the additions required for
normalization before copying the declaration. Hard limits cap normalized bytes
and nodes, global expression nodes and depth, cumulative work and retained
logical storage. Caller limits can only reduce those ceilings. Accounting is
conservative and monotonic, including temporary copies and retained source
storage; it describes logical allowances rather than allocator RSS. A future
catalog transaction must share a ledger across admission and compilation rather
than granting every entry a fresh catalog allowance.

Only the execution copy receives metadata logging defaults, absent option and
install-case arrays, optional collections, active conditions, flag reservations
(`always`) and JSON/SSE codec defaults. Stable option IDs map to declaration-order
slots containing the public name, persisted config key, type and full contract.
Authored names, absence, provenance and source/definition/dependency hashes stay
unchanged. Private staging data moves into an actually const implementation;
no mutable JSON or instruction aliases survive publication.

Expressions compile only at schema-designated positions. `literal` and lookup
`default` payloads remain opaque. Explicit types do not coerce values. A missing
option uses its declared default; present empty strings, false, zero and null
stay present. Reference defaults apply only to absent lookups. `present` tests
existence and `coalesce` selects the first present value, including null. Launch
records preserve raw arguments, flag aliases and reservations, custom-default
stages and merge policies, optional artifacts, canonical model context floors
and all platform cwd choices. Phase checks reject facts used before their
producer. The first adapter set supports hook-free buffered JSON and OpenAI SSE;
other adapter combinations fail explicitly.

The private invocation-frame seam contains option values and runtime facts. A
frame is supplied to expression evaluation and is never retained in a compiled
declaration. No model, environment, port or effective context is resolved by
compilation. The compiler has no filesystem, process, hook, hardware or network
effects; lexical path expressions only manipulate path strings.

The domain-separated compilation identity includes authored definition and
dependency identities, normalized semantics and compiler/default/binding/adapter
revisions. It is not an execution identity or proof of executable validity.
The standalone compiler library is not linked into production binaries. Startup
continues rejecting v2 sources, candidates remain `executable:false`, production
exports remain empty and the v1 path is unchanged. The initial 4a compiler
rejected every hook; 4c now accepts only hooks backed by surviving sealed grants.
The frozen 4b llama.cpp oracle remains the reference for the later production
port. Catalog publication and runtime execution attach in 4d and 4e.

## Typed mutable contracts and admission membership (phase 4c increment)

The mutable surface has a separate closed `MutableFragmentKind` vocabulary.
An installed policy chooses an exact stable target, the permitted fields,
record and string limits, numeric bounds, finite value domains, required
records, collection permissions and typed references. JSON cannot select a
validator or grant an arbitrary subtree capability.

| Contract | Mutable data | Sealed data |
|---|---|---|
| `OptionPresentationV1` | Name, CLI flag, help and group of an existing option ID | ID, value type, binding role, UI and references |
| `OptionDefaultV1` | Typed scalar default and permitted enum, range and scalar unset constraints | Value type, fields outside the policy and outer bounds |
| `ArgvSpellingsV1` | Flag, aliases and an explicitly designated literal string value | Stable record ID, condition, reservation policy and reference source |
| `ReservedFlagsV1` | Bounded unique flag spellings | Required managed flags and aliases cannot disappear |
| `CustomDefaultsV1` | Typed default records, arguments, flags, aliases, Boolean conditions, permitted stage and merge policy | Hook grants, managed reservations and additional hidden flags |
| `DeviceSelectionV1` | Fixed backend, default device and channel data within finite domains | Selection strategy, option binding and effect capabilities |
| `EnvironmentEntriesV1` | Name-keyed child assignments and typed string values | Parent environment reads, staging destinations and other capabilities |
| `SupportRowsV1` | Closed support rows with finite backend, OS, tier, format, device, architecture and channel data | Executable operations, installer strategy and hooks |
| `InstallCasesV1` | Closed release cases, permitted repository and selectors, typed filename/version expressions, backend-keyed default versions | Strategy, hook, recipe ownership and download capabilities |
| `ContextFloorsV1` | Positive bounded floors under canonical model-type keys | Original caller options and fact-provider authority |

Each contract establishes the shared outer envelope. Every surviving slot also
has its own surface; those restrictions intersect. A slot cannot widen the
shared limits, fields, domains, references or collection permissions. The shared
sealed projection still applies when the final hook-bearing owner is removed.
Older scalar and `Expression` rules keep their existing representation and pins.
Policies using the new surface include it in their canonical policy identity;
the synthetic authority fixture pins policy revision 4 explicitly.

Closed record validation runs before permitted data is masked from sealed
comparisons. The observer also checks authored writes, so replacing an enclosing
launch/metadata/install subtree or an existing option/argv record cannot become
a permitted edit by preserving some final sealed values. Collection `add`,
`remove` and `clear` are separate permissions, checked against the authored
operation as well as final record membership. Clearing and re-adding identical
bytes does not bypass a denied `clear`. Required records cannot disappear.
Complete operation-owner semantics and empty hook chains stay independently
sealed; the new fragment targets cannot name an operation owner.

Typed references name a specific producer field, scalar result type and
availability phase. Platform, install, declared-artifact, allocated-port and
launch references are enumerated rather than admitted by root prefix. Option
references use stable IDs. Prepare/runtime facts require their corresponding
typed provider symbol; removing the producer invalidates the reference.
Repository-phase expressions cannot consume asset-phase version facts, and
install expressions cannot consume launch facts. Compiler revision 2 checks
named platform/install/artifact/launch fields and types in addition to the
existing phase checks. Compiler revision 3 also records exact producer and field
indices for the closed `ModelFacts` fields under `/launch/hook/`, checks scalar
types and rejects references before their producer phase. Arbitrary hook facts
remain unsupported. Compilation never obtains a fact value.

Admission now gives each transaction a private `CatalogAdmissionTicket` and
retains it in its admitted authorization wrappers. Its private membership check
requires both ticket-object equality and the exact token-object address in that
catalog's retained maps. Identical deterministic admission digests do not grant
cross-transaction authority. A wrapper carrying the same ticket but absent from
the maps also rejects. Standalone authorization has no ticket. The ticket is
neither serialized nor used as a deterministic digest. The binder checks
the token retained by its exact compiled input through this boundary.

## Sealed symbolic compilation and immutable binding (phase 4c)

The compiler reads surviving grants through the server-private
`AuthorityInspection` seam. Each symbolic slot retains its grant ordinal, closed
contract enum, owner kind and ID, stage, order, parameter/owner/policy identities,
ABI and implementation revisions, effects, typed input/fact indices and
required slot dependencies. Declaration traversal checks correspondence against
those grants. It cannot create authority. Owner correspondence uses the retained
authored declaration because execution defaults change normalized owner fields.
The compiler has no access to the callable inventory.

The separate binder has one public entry point:

```cpp
ExecutionBindResult bind_execution_definition(
    std::shared_ptr<const AdmittedCatalog> admitted,
    std::shared_ptr<const CompiledDefinition> compiled,
    const ExecutionBindLimits& limits = {});
```

`ExecutableDefinition` is final, has private construction and exposes only const
compiled, identity and metric views. Binding retains both exact input shared
pointers in an actually const implementation. It first charges retained context
and membership-scan work, then applies `contains_token`. Only after membership
passes does it inspect symbolic slots and the installed callable inventory.
Identical admission digests, matching-ticket clones, standalone authorization
and listed tokens with detached tickets cannot substitute for exact membership.

The ten installed targets are independently audited against their closed C++
contracts. Binding selects by the sealed slot's contract enum and checks exact
slot, ABI, implementation, effect and dependency correspondence. It never calls
the legacy `target(id, stage)` lookup. The result retains typed targets privately;
there is no public invocation or bound-target accessor. Semantic identities and
revisions determine the execution digest; addresses and admission tickets do
not enter it.

Binding defaults cap slots at 4,096, retained logical bytes at 128 MiB, work
visits at 1,048,576 and diagnostics at 100. Caller limits can only reduce these
ceilings. Checked arithmetic precharges slot storage, retained catalog/compiled
context, membership scans, dependency and expression traversal, and identity
construction. Every failure returns a null definition, including zero diagnostic
capacity. Accounting is conservative logical storage/work, not measured RSS.

The three CI suites are `BackendDefinitionBindingTest`,
`BackendDefinitionBindingBoundsTest` and `BackendDefinitionBindingBoundaryTest`.
They check all ten target kinds, independently authored eleven operation chains,
transaction and slot attacks, accounting order, immutable retention and API
exclusions. Dependency inspection covers production compiler/binder/registry
objects, with installed target-address references permitted only in the registry.
The same compiler, binder and adapter sources are built separately with test-only
entry counters and fail-fast effect seams. Hook-bearing success and failure paths
must satisfy `HookBearingCompileAndBindInvokeNothing`; isolated target-call
mutants must fail it. Production archives contain no test injection API.

The HRX-shaped positive fixture binds an artifact slot with no runtime-options,
prepare, environment or system slots or dependent facts. After proving zero calls
during compilation and binding, it invokes all five real bound `ArtifactOps`
members with confined fake reads. Metadata and labels, checkpoint validation,
GGUF validation, quantized path resolution and imported-file selection must work.
Real recipe options, shared defaults, context and environment stay unchanged,
with zero runtime/system entry or service sentinels. This establishes useful
restricted delegation; rejecting a legacy provider symbol alone cannot prove it.

4c does not publish executable catalogs or activate a production v2 backend.
4d must attach the private runner, runtime consumers, bounded capability services
and atomic publication of a completely compiled/bound catalog. 4e must attach
the production llama.cpp definition and descriptor/factory switch, qualify it
against the frozen oracle and resolve bounded system-probe behavior explicitly.
Production HRX inheritance, external exports and hardware qualification remain
later work. The aggregate export pin and all v1 snapshots remain unchanged.
