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
