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
