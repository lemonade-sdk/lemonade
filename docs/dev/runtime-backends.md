# Runtime local backends

`lemond` reads external local backend definitions from immediate, regular
`<config_dir>/backends.d/*.json` files during startup. Filenames use case-sensitive
`.json` selection and UTF-8 lexicographic ordering. Other files and subdirectories
are ignored. Selected symlinks, nonregular files, unreadable files, invalid
objects, and definitions larger than 1 MiB fail startup. Each file holds one
schema-v1 object. A missing directory is allowed.

The catalog freezes immediately before configuration loading in both standalone
and embedded Windows startup. Editing definitions or sending SIGHUP has no effect
until restart. An invalid catalog never publishes a subset or starts serving.
Compiled backends retain their order; external entries append in filename order.
External definitions cannot replace a compiled recipe, including an opt-out
configured built-in. Definition IDs, external recipes, configuration sections,
and external option ownership must be unique. Built-in/common options, request
control fields, and global configuration keys are reserved. External definitions
receive no compiled compatibility attachment or named CLI flags.

Use `schemas/backend-definition-v1.schema.json` as the structural contract. Only
installed primitives are accepted; definitions do not install hooks or code.
External scalar options retain their JSON types, enum/range constraints and
`unset_values` (default `[null]`). Values such as `false`, `0`, `""`, and `-1` are
preserved unless declared unset. Existing REST null clearing semantics apply.
Ordinary options use their exact names in the backend's config section; backend,
args and device roles retain the existing nested naming rules. Existing per-model
options endpoints expose and save these options without loading a model.

Managed definitions can supply `install.default_versions` when shipped pins do
not cover their pin identity. Shipped pins and existing user version/path choices
retain precedence. `external_only` requires a configured executable and never
falls through to downloading one. Definitions and model registrations are local
administrator inputs; schema discovery does not install or replace them.

## Metadata and validation API

All endpoints below exist under `/api/v0/`, `/api/v1/`, `/v0/` and `/v1/` and use
normal API-key authentication.

`GET backends/schema` returns schema/control/definition versions, SHA-256 catalog
and feature-manifest IDs, and ordered public recipe metadata. `?recipe=ID` returns
one exact recipe in the same envelope. Empty/repeated filters return 400; unknown
recipes return 404 with code `unknown_recipe`. Responses use a private, no-cache
ETag, checked after authentication.

The export includes option contracts, static support metadata, and known
operation descriptions. `operations_complete:false` means the retained compiled
backend's operation projection is incomplete. It does not mean unsupported.
Executable paths, argv, environment, install configuration, expressions, raw
definitions, current settings and credentials are excluded.

`POST backends/validate` accepts original definition bytes. It checks the server's
installed feature set, binding preparation and catalog collisions without writing,
installing, launching or publishing anything. A matching external ID **and**
recipe can be substituted in a private candidate; built-ins cannot. Success is
200 with `valid:true`; invalid input is 400 with ordered diagnostics; input over
1 MiB is 413. Validation does not promise executable/model availability.

Definition hashes cover canonical source JSON before validator defaults. Object
key order and whitespace do not affect them; array order and source values do.
Catalog hashes also cover ordered identities, origins, selected built-in arms and
the installed feature manifest, and exclude ports, paths and credentials.

The docs generator uses separate temporary cache/config directories and
`--builtin-backends-only`, which skips external directory discovery entirely.
Generated committed artifacts remain built-in-only.

## CLI

`lemonade backends schema [RECIPE]` prints the connected server's complete JSON
metadata response. `lemonade backends validate FILE` submits the original bounded
file bytes to that server. It does not first reject definitions using the client's
older embedded schema. Add `--offline` for provisional embedded-schema validation
without discovery or network access; the warning explains that connected-server
catalog collisions and executable bindings have not been validated.

Existing named flags remain supported. `load`, `run`, and `launch` also accept
repeatable `--option KEY=JSON_VALUE`, for example:

```sh
lemonade load MyModel --option 'example_batch=4' --option 'example_label="a=b"'
lemonade run MyModel --option 'example_enabled=false' --chat-cli
lemonade launch claude --model MyModel --option 'example_batch=4'
```

Each assignment splits at its first `=` and parses the entire remainder as one
finite, non-overflowing JSON value. Keys are exact, with no trimming. Duplicate
generic keys or collisions with explicitly supplied named flags (including
negated aliases and `--pinned`) are errors; unprovided defaults do not collide.
Generic values merge into raw outgoing JSON after named processing, without a
client-local recipe allowlist.

Generic options resolve the model's recipe and its option contract from the
selected server before any pull, load, browser opening, background load or agent
launch. Generic launch selection without an explicit model lists registered
models, avoiding a recipe-import side effect before preflight. Named-only flows
keep their existing selection and requests, with no mandatory schema request.
There is no persistent schema cache or dependency on client-local definitions.

New local option/definition validation errors exit 2. Successful schema/validation
commands exit 0; transport, authentication, missing schema support, unknown recipe
and incompatible protocol responses exit 1. Existing CLI11 and named-command
error behavior is retained. An old or unreachable server never becomes an offline
success; offline validation must be requested explicitly.

## Shared renderer configuration

The desktop and web renderer obtain external recipes from the selected server's
`backends/schema` response. Schema, control and definition versions must all be
1. Unknown controls, incompatible control/type pairs, unsafe numeric values and
malformed contracts disable external submission with an explanation. A compatible
recipe with zero execution options has an explicit empty-options message.
Authentication failure, an unreachable server, a missing schema endpoint and an
unknown recipe have distinct messages. Legacy forms remain available on servers
without the schema endpoint, with a compatibility notice.

External recipe rows also export `backend_option`: the exact option-driven launch
selector name, or null for a fixed backend. This additive v1 field exposes no
executable, arguments, environment or installation records. Older compatible
responses may identify a selector with one unambiguous `supported_backends` UI
hint. Without either identity, the client leaves backend selection to the server;
it does not guess from an option suffix. Runtime choices and setup messages come
from that server's `/system-info`, including when the client is another OS.

The retained eleven-recipe presentation adapter preserves built-in labels, order,
backend choices, context slider, resets, pin and save controls. Its context
fallback remains 4096. Missing UI metadata does not disable those fields. Other
built-in recipes retain their existing presentation. External identities and
option names are open strings with own-property-safe lookups and no legacy
camelCase/API alias conversion.

External forms support text, textarea, number, checkbox and typed select controls.
Missing optional UI hints derive controls from the declared type or enum. The
form shows the schema default, server default excluding saved overrides, effective
value and saved override separately. Numeric draft text is validated as a whole;
partial parses, nonfinite values, unsafe integers, fractional integers and range
violations cannot submit. Enum indices preserve JSON types. False, zero, empty
strings and minus one remain values unless that exact scalar is an unset sentinel.
Sentinels resolve before ordinary type/range/enum validation.

**Save** posts only edited API option keys to `/models/{id}/options`. Untouched
fields are omitted. **Use inherited value** previews the server default and sends
null when explicitly submitted, clearing a saved override on Save. **Load** first
dry-runs those keys, then uses fresh effective values for installation preflight
and `/load` with `save_options:false`. Load does not persist edits. Pin is a
separate load-only control, excluded from Save and dry-run bodies.

Catalog state stays in memory and is keyed by effective API URL, opaque
authentication generation and catalog ID. Raw API keys are absent from those
identities and error messages. Requests capture a connection generation and
cannot retry a mutation against another server. Connection changes clear cached
schemas, ETags, names and availability; obsolete responses are discarded. The
browser bridge keeps settings and credentials client-local and emits local and
cross-tab notifications while using the current origin as its server.

Opening/reopening a form, connection/backend updates, focus while open and each
Save/Load revalidate the catalog. Changed catalogs refresh untouched values and
revalidate retained dirty edits by exact key. Removed edits stay visible until
discarded. The submission discovering a changed catalog stops for review before
mutation. Invalid retained edits must be corrected before another submission.

Frontend checks use Node/CJS with the existing TypeScript transpiler and
`react-dom/server`; no browser testing package or runtime dependency is required.
Run `npm run typecheck` separately from the web bundle: its loader uses
`transpileOnly`. `test/catalog_ui_exercise.py --output DIRECTORY` uses an installed
Chrome with GPU disabled, the real served bridge, isolated server data and the
existing deterministic CPU engine. It records actual browser stages or an
explicit unverified result when no browser is available. Native Tauri behavior
and distribution packaging require their own environment-specific qualification.
