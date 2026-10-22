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
External definitions cannot replace a compiled recipe, including a built-in
that runs from an embedded definition. Definition IDs, external recipes, configuration sections,
and external option ownership must be unique. Built-in/common options, request
control fields, and global configuration keys are reserved. External definitions
receive no compiled compatibility attachment, compiled hook or named CLI flag.

Use `schemas/backend-definition-v1.schema.json` as the structural contract. Only
installed primitives are accepted; definitions do not install hooks or code. Only
built-in definitions can select an installed compiled hook (see
[Installed hooks](#installed-hooks)); an external definition expresses its
behavior through data primitives alone.
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

## Built-in configured backends

ThinkSound (`thinksound`), DS4 (`ds4`), RyzenAI (`ryzenai-llm`), HRX
(`llamacpp-hrx`), Kokoro (`kokoro`), Moonshine (`moonshine`), ONNX Runtime
(`onnxruntime`) and TRELLIS.2 (`trellis`) have no engine-specific server class.
They run only through `ConfiguredServer`, from the definitions embedded at build
time from `src/cpp/resources/backends/<stem>.json`.
There is no environment switch and no fallback implementation; the former
`LEMONADE_THINKSOUND_CONFIGURED`, `LEMONADE_DS4_CONFIGURED` and
`LEMONADE_RYZENAI_CONFIGURED` variables are ignored. Each definition is the single
source of truth for its launch, install, artifact and operation behavior, and its
`rationale` records explain the non-obvious choices (DS4 SSD streaming,
`/v1/models` readiness, reserved arguments and Linux-only install case, the
ThinkSound companion files, RyzenAI sentinel order, the HRX host requirement and
`--parallel 1` default, the Kokoro index file and streaming formats, the
Moonshine port triple and architecture fallback, the ONNX Runtime
servable-directory rules and startup diagnostics, the TRELLIS custom-argument
order and mesh upload).
An embedded definition that fails validation stops registration at startup.

CMake embeds every `src/cpp/resources/backends/<stem>.json` its
`file(GLOB CONFIGURE_DEPENDS)` finds as `lemon::backends::k_<stem>_definition` in
the generated `backend_definition_resources.h`, after `k_backend_definition_schema`
from `schemas/backend-definition-v1.schema.json`;
`cmake/BackendDefinitionResources.cmake` writes the constants in lexicographic file
order. Adding or removing a definition re-runs the configure step on the next build,
and so does editing a definition or the schema. Each constant is a raw string
literal delimited by `R"definition(` and `)definition"`, so the configure step
fails, naming the file and byte offset, when a definition or the schema contains
`)definition"` (any JSON string ending in `)definition`), instead of emitting a
literal that ends early. It also fails when `<stem>` does not match
`[a-z][a-z0-9_]*`, because the stem names the constant. A file whose stem has no
`LEMON_BACKENDS` entry is still embedded, but nothing registers it.

MSVC rejects a string literal longer than 16,380 bytes before adjacent literals
are concatenated (error C2026; MSVC 14.51 lifts the limit), and the schema alone
is over 33,000 bytes. `cmake/CxxStringLiterals.cmake` therefore writes each
constant as adjacent raw string literals of at most 8,000 bytes, half the limit,
which the compiler concatenates back into the file's bytes. It splits after the
last line break that fits in the budget. A line longer than the budget is split
inside the line, never inside a UTF-8 sequence, because the generated header must
stay valid UTF-8 for MSVC, and never between a carriage return and its line feed,
because compilers fold CRLF inside a raw string to LF but treat a lone CR
differently. The configure step fails, naming the file, the constant and the byte
offset, when such a line is not valid UTF-8, and, naming the length, when any
emitted literal would exceed the budget. `BackendDefinitionResourcesTest` compares
every embedded constant with its source file byte for byte and checks that every
generated header is valid UTF-8 with no string literal over the budget.

A new built-in configured engine needs:

- its definition, `src/cpp/resources/backends/<stem>.json`;
- its `"<recipe>|<stem>"` entry in `LEMON_BACKENDS` in `CMakeLists.txt`, the
  explicit registration that compiles its bridge, generates its descriptor and
  adds it to the registries;
- its registration bridge, `src/cpp/server/backends/<stem>/<stem>_server.cpp`,
  which validates `k_<stem>_definition`, and
  `src/cpp/include/lemon/backends/<stem>/<stem>_server.h`, which declares
  `configured_entry()`, `create()`, `spec()`, `ops()` and the `capabilities()`
  literal;
- its version pins in `src/cpp/resources/backend_versions.json` when it downloads
  a binary, and its models in `src/cpp/resources/server_models.json`.

It needs no other CMake or template edit: its embedded definition and its
descriptor follow from the file and the `LEMON_BACKENDS` entry. An external
definition in `backends.d` needs no rebuild at all.

`src/cpp/server/backends/<stem>/<stem>_server.cpp` is only the registration
bridge. It validates the embedded definition once, builds the
`ConfiguredBackendEntry` and exposes it through `configured_entry()`, `create()`,
`spec()` and `ops()`. The bridge also attaches the compiled compatibility that
external definitions can never request:

| Recipe | Attachment | Effect |
| --- | --- | --- |
| `thinksound` | `TrustedCompatibility::LegacyNonChatSse` | Keeps the legacy-forwarding streaming bindings for chat completion, completion and responses next to buffered audio generation. |
| `ds4` | `BuiltinLaunchCompatibility{DEVICE_GPU, true, load_log}` | Sets the GPU device once the backend is selected, before installation, and prepends the bundled executable directory to `LD_LIBRARY_PATH` on every host. Its load log drops the generic loading line, logs custom arguments at DEBUG and logs the executable and model path in the INFO startup line. |
| `ryzenai-llm` | `BuiltinLaunchCompatibility{{}, false, load_log}` | Its load log keeps the loading line, the resolved executable, the model path and the quoted command at DEBUG, with no INFO lines. |
| `llamacpp-hrx` | `BuiltinLaunchCompatibility{{}, false, load_log}` | Its load log keeps the INFO loading line, adds the DEBUG per-model settings line and has no startup line. |
| `kokoro` | `TrustedCompatibility::LegacyNonChatSse` and `BuiltinLaunchCompatibility{{}, false, load_log}` | Keeps the legacy-forwarding chat streams next to speech. Its load log keeps the loading line and logs the port once it is allocated and the POSIX loader path once the environment is resolved, at INFO. |
| `moonshine` | `TrustedCompatibility::LegacyNonChatSse` and `BuiltinLaunchCompatibility{DEVICE_CPU, false, load_log}` | Keeps the legacy-forwarding chat streams, sets the CPU device before installation, and logs the per-model settings, model, executable and both ports at INFO and the custom arguments at DEBUG. |
| `onnxruntime` | `TrustedCompatibility::LegacyNonChatSse` and `BuiltinLaunchCompatibility{DEVICE_CPU, false, load_log}` | Keeps the legacy-forwarding chat streams, sets the CPU device before installation, and logs the per-model settings, the servable directory and the executable at INFO. |
| `trellis` | `TrustedCompatibility::LegacyNonChatSse` and `BuiltinLaunchCompatibility{{}, false, load_log}` | Keeps the legacy-forwarding chat streams; its load log adds the DEBUG custom-argument line before the INFO startup line. |

A `BuiltinLoadLog` replaces `ConfiguredServer`'s generic INFO `Loading model:` and
`Starting <program> on port <port>` lines with the engine's own messages; its
`prepared` hook runs after each `prepare_order` phase, so diagnostics emitted
before a failing phase still appear. The subprocess log level itself comes from
the definition's `log_level_policy` (`debug_flag` for RyzenAI, `passthrough`
otherwise), not from the attachment.

Each definition's `metadata` block is also the single source of the engine's
`BackendDescriptor`: the recipe, option, CLI flag and support data linked into both
`lemonade` and `lemond`. At configure time, for every `LEMON_BACKENDS` stem that
has a `src/cpp/resources/backends/<stem>.json`,
`cmake/BackendDescriptorFromDefinition.cmake` writes
`lemon::backends::<stem>::descriptor` into the generated
`backend_descriptors_generated.h` with CMake's own `string(JSON)`, so these engines
have no `<stem>.h`. Descriptor headers under `include/lemon/backends/<stem>/` exist
only for the engines without a definition. The generator emits every descriptor
field, including each support row's OS set, devices, summary and architecture
gates and each option's JSON default; `metadata.config_extra` and option defaults
keep their JSON values, emitted as `nlohmann::json::parse()` of raw string
literals split the same way as the embedded definitions. Any other string literal
it would emit over the 8,000-byte budget, such as an option's help text, fails
the configure step. It maps `default_device`, `slot_policy`, `version_policy`,
`tier` and `format` by explicit tables and fails the configure step, naming the
file and JSON pointer, on an unknown value, an unknown key, a wrong type or a
missing required field, instead of defaulting it. `server_name`, `log_category`
and `log_level_policy` are server-only and never reach the descriptor. A per-OS
`binary` object becomes `#ifdef _WIN32` / `__APPLE__` / Linux branches, matching
`native_platform()`. The objects are `inline const`, so the descriptor registry,
the factory registry and the capability registry share one object per engine and
the pointers callers cache stay valid for the process. Each engine's oracle test
compares every field of the descriptor `ConfiguredBackendEntry` builds from the
same JSON at runtime with the generated one, `descriptor_for("<recipe>")`, through
the shared `descriptor()` helper in `test/cpp/configured_fixture.h`, so the two
mappings cannot drift. `capabilities()` in `<stem>_server.h`
is a literal (`CAP_AUDIO_GENERATION` for ThinkSound, `CAP_TTS` for Kokoro,
`CAP_TRANSCRIPTION | CAP_STREAMING_TRANSCRIPTION` for Moonshine, `CAP_CLASSIFICATION`
for ONNX Runtime, `CAP_MODEL_3D` for TRELLIS, `0` for DS4, RyzenAI and HRX)
because the header-only mode contract test cannot read a definition, and every
engine shares the single `ConfiguredServer` type, which inherits every capability
interface a definition can bind (embeddings, reranking, classification,
transcription, streaming transcription, speech, audio generation, 3D generation,
slots and tokenize) whether or not its definition binds it, so a mask derived
from the type would claim all of them for every engine. Dispatch reads only the
operation bindings. `ConfiguredRegistrationTest`
fails if a literal disagrees with the definition's implemented operation bindings
inside the recipe's declared deployment modes. HRX binds llama-server's
embeddings, reranking, slots and tokenize endpoints for every model mode, as its
former llama.cpp subclass did, but declares only `chat`, so its export stays `0`.

The CPU oracle tests (`test_thinksound_oracle`, `test_ds4_oracle`,
`test_ryzenai_oracle`, `test_hrx_oracle`, `test_kokoro_oracle`, `test_moonshine_oracle`,
`test_onnxruntime_oracle`, `test_trellis_oracle`,
`test_configured_execution`,
`test_configured_llm_execution`) drive the registered entries through fake
effects, fake processes and loopback HTTP fixtures, against hand-authored
expectations for argv, environment order, readiness, error envelopes,
operation bindings and install assets.

## Differences from the retired wrapper classes

The migrated engines keep their routed HTTP behavior. What differs is log
ordering, the state a server object is left in after a failed load, and direct
C++ calls that the HTTP handlers never make. `Router` constructs a new server for
every load attempt, including its retry, and keeps it only when `load()`
succeeds, so a server object that failed to load is never reachable through
HTTP.

- **Moonshine streaming address.** `ConfiguredServer` replaces its port mapping
  only once preparation succeeds, and `get_streaming_address()` reads that
  mapping. A fresh object whose preparation fails, for example on custom
  arguments rejected after the three ports were probed, reports an empty
  address. A readiness failure keeps the newly mapped `tcp://` address while the
  HTTP port resets to 0, as the wrapper's did. A failed preparation on an object
  that loaded before, including a failed port search, keeps the previous address.
  The wrapper instead set both ports to 0 before searching and published the
  probed ports before it validated custom arguments.
- **Moonshine and ONNX Runtime HTTP port.** A `probe` port becomes the server's
  HTTP port only when `start_server()` runs. A failure after probing leaves
  `get_backend_port()` at its prior value, 0 on a fresh object, where the
  wrappers had already assigned the probed port. A `standard` port still comes
  from the server's port chooser, which publishes it at allocation.
- **TRELLIS custom-argument log line.** The load log's `starting` hook writes the
  DEBUG `Adding custom arguments:` line after preparation has resolved the
  accelerator environment; the wrapper wrote it before. The CUDA environment's
  INFO lines (`Respecting existing CUDA_VISIBLE_DEVICES=`, `Restricting
  CUDA_VISIBLE_DEVICES to`, `Setting __NV_PRIME_RENDER_OFFLOAD=1`) now precede it,
  and an exception thrown while the environment is resolved suppresses it, where
  the wrapper had already written it. Only log output changes.
- **HRX streaming forwarding.** A direct `forward_streaming_request()` call binds
  only `/v1/chat/completions`, `/v1/completions` and `/v1/responses`; any other
  endpoint throws `std::invalid_argument` (`Streaming operation adapter is not
  bound`), where the inherited llama.cpp override forwarded it unchanged. The
  router streams only those three routes, which behave as before.
- **HRX installation.** An unsupported install backend raises the same message
  as a `std::runtime_error` from the definition's `install.errors.backend`, where
  the wrapper threw `std::invalid_argument`. `/install` handles both alike, so its
  responses are unchanged.
- **Option conversion order.** Preparation converts the custom-argument option
  and then `ctx_size` before its first phase. HRX therefore converts both before
  installation and model resolution, and reports a malformed `hrx_args` before a
  malformed `ctx_size`; the wrapper converted them after installation and model
  resolution, `ctx_size` first. TRELLIS converts `trellis_args` before it checks
  the model path. Each conversion error keeps its text; only when it surfaces
  changes.
- **Kokoro malformed speech request.** A direct `audio_speech()` call with a
  JSON scalar instead of an object throws `json::out_of_range` (404, from the
  request mapper's JSON-pointer assignment), where the wrapper's key assignment
  threw `json::type_error` (305). The speech handler requires `model` and
  `input` fields before it calls the backend, so such a body never reaches it.

The oracle tests pin the Moonshine and ONNX Runtime failure states and the
TRELLIS accelerator log order with CPU fakes. The TRELLIS test runs the
production CUDA environment step with `CUDA_VISIBLE_DEVICES` already set, so no
GPU is queried; see [platform qualification](platform-qualification.md) for where
these tests have run.

## Installed hooks

A hook is compiled behavior that a built-in definition selects by ID. The
validator, which the CLI also links, accepts only the IDs and stages listed in
`installed_hook_manifest()`; `installed_backend_hooks()` binds the same IDs to
typed functions in `lemond`, and `BackendDefinitionTest` fails if the two lists
differ. Every installed hook takes empty `params`. The feature manifest lists
them, so installing a hook changes the catalog's feature-manifest ID.

Hooks reach engine-specific compiled behavior (file reads, directory scans,
multipart and TCP clients, a rerun of the backend executable), so only a
definition embedded at build time can select one. `BackendDefinitionStore::validate`
rejects every hook selection in an external definition before it looks up the
ID: `hooks.launch`, `hooks.start_failure`, an artifact `hook`, an operation
`handler`, and each `request.hooks` and `response.hooks` entry. The diagnostic
has code `reserved_hook`, message `Compiled hooks are reserved for built-in
definitions`, and the selection's `/id` pointer, whether or not the ID is
installed. An install hook and the other `hooks` stages have no implementation
and fail with `unknown_hook` for every origin. Startup discovery in `backends.d`,
`POST backends/validate` and `lemonade backends validate --offline` share this
validator. The feature manifest records the rule as `"hook_origins":
["builtin"]`.

| ID | Stage | Selected by | Effect |
| --- | --- | --- | --- |
| `llamacpp.gguf` | `artifacts` | an artifact with `"strategy": "hook"` (role `main` only) | Delegates metadata probing, checkpoint resolution, import lookup and checkpoint validation to llama.cpp's GGUF rules. |
| `llamacpp.max_tokens_alias` | `request` | `operations[].request.hooks` | Copies `max_completion_tokens` to an absent `max_tokens`. |
| `llamacpp.tool_schema_limits` | `request` | `operations[].request.hooks` | Removes tool-schema string and array bounds that llama.cpp's grammar cannot repeat. |
| `llamacpp.client_model` | `response` | `operations[].response.hooks` (JSON transports only) | Replaces an echoed `model` with the request's model, or the loaded model name, so responses do not leak the local GGUF path (and user name) llama-server was launched with. |
| `kokoro.voice_index` | `launch` | `hooks.launch` with a `hook` phase in `prepare_order` | Logs and reads the resolved `index.json` and exposes it as `/launch/hook`; argv joins its `model` and `voices` entries with `path_join`. |
| `moonshine.model_arch` | `launch` | `hooks.launch` | Returns the registry's `moonshine_arch` extra, else 2, 4 or 5 from the checkpoint name (`tiny`, `small`, otherwise). |
| `moonshine.transcription` | `operation` (`transcription`) | `operations[].handler` | Posts the audio as multipart to `/inference` on the backend port and maps its errors to the transcription error envelope. |
| `onnxruntime.model_directory` | `artifacts` | an artifact with `"strategy": "hook"` | Resolves the single complete model directory (`model.onnx`, `tokenizer.json`, `config.json`) inside the active snapshot, or inside a whole import; an ambiguous import logs a warning and resolves to nothing. |
| `onnxruntime.servable_directory` | `launch` | `hooks.launch` | Rejects a missing or ambiguous layout under the resolved path at load and returns the servable directory. |
| `onnxruntime.classify` | `operation` (`classification`) | `operations[].handler` | Sends `text` (or `input`) and an optional `top_k` to `/classify` with a 120-second timeout; a request without either string gets a 400 envelope. |
| `onnxruntime.startup_error` | `start_failure` | `hooks.start_failure` | Reruns the refused executable for at most 20 seconds and appends up to 400 bytes of its output, or its exit code and installed files, to the start error. |
| `trellis.mesh` | `sink_operation` (`model_3d_generation`) | `operations[].handler` | Decodes a base64 or data-URL image, rejects formats other than PNG, JPEG, BMP and GIF, posts it as multipart to `/generate` (30-minute timeout) with the optional fields, and writes the mesh bytes or a JSON error to the sink. |

An operation hook replaces the transport for one buffered operation; the
manifest names the operation it serves. Hooks for media operations
(`model_3d_generation`, `text_to_speech`, `audio_generation`) use the
`sink_operation` stage and write to the response sink. Engine-specific hooks live
next to their bridge as `<stem>_hooks.cpp`.

Request hooks run in order after `remove` and `set`. A streaming operation with
request hooks forwards a body that is not JSON unchanged; otherwise it forwards
the re-serialized result. llama.cpp's own server class calls the same functions.

Other launch primitives used by built-ins:

- A `custom_args` phase in `prepare_order` validates the custom arguments at that
  point instead of after the last phase.
- A `hook` phase in `prepare_order` runs the definition's `hooks.launch` hook and
  stores its result under `/launch/hook`. A launch hook may log and may fail the
  load.
- `prepare_order` may contain a separate `install` phase for a managed program.
  It must follow `backend` and precede both `program` and `ports`; the `program`
  phase then only looks up the installed executable. Without it, `program` must
  precede `ports`.
- `launch.requires` rejects other operating systems or machine architectures
  with its literal `error` before any logging or installation.
- `custom_args.defaults` appends arguments, as llama.cpp's runtime defaults do,
  unless the user's arguments already name the flag or one of its aliases.
- An artifact with `"check": "nonempty"` accepts any nonempty resolved path
  without an existence probe.
- Install cases may select on `machine_arch`; a miss reports
  `install.errors.machine_arch`.
- JSON transports also serve `embeddings`, `reranking`, `tokenize`, `slots`
  (`GET`) and `slot_action`, whose path substitutes `{slot_id}` and `{action}`.
- `text_to_speech` uses the byte transport for buffered and streaming delivery.
  Both records must share one transport and request; each declares the formats
  of its delivery. A definition serves either speech or audio generation.
- `unsupported_messages` replaces the generic rejection text for buffered chat
  completion, completion and responses with a literal message; the envelope
  keeps `unsupported_operation` and `model_not_applicable`.
- `launch.backend.fixed` may name a backend per operating system.
- Ports use `standard` allocation (the server's port chooser), `probe` (the first
  free port from 8001 whose `span - 1` successors are also free, retried
  `attempts` times from the next candidate) or `offset` (an earlier probed port
  plus an offset inside its span).
- `streaming_transcription` binds a `realtime` transport with method `TCP` on any
  declared port; its address is `tcp://127.0.0.1:<port>` from the last load whose
  preparation succeeded, and empty before one has.
- `files_from: "variant_directory"` downloads every file under the variant's
  directory (case-insensitive); an empty selection raises `files_error`.
- Install cases may list `""` under `machine_arch` for an unrecognized host.
- `cache_resolution: "sentinel_file"` resolves to the first sentinel file itself,
  not its directory.
- Expressions `when` (a value only when its condition holds), `parent_path` and
  `path_join` (`std::filesystem` semantics; a non-string component raises the
  same JSON type error as converting it to a path).
- The `posix_executable_loader_path` environment preset prepends the executable
  directory to a nonempty inherited `LD_LIBRARY_PATH` on non-Windows hosts.
  `environment.order: "set_first"` places literal `set` entries before preset
  entries.
- `hooks.start_failure` runs when the process fails to become ready and the
  load was not cancelled; its text is appended to the start error after `: `.
- A built-in load log's `prepared` hook also runs once the environment is
  resolved, with the phase name `environment` and the entries in
  `/launch/environment`.

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
Catalog hashes also cover ordered identities, origins, whether each built-in runs
from a compiled server class or an embedded definition, and the installed feature
manifest, and exclude ports, paths and credentials.

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
and distribution packaging require their own environment-specific qualification;
see [platform qualification](platform-qualification.md).

`test/catalog_fourth_exercise.py --freeze FILE --output FILE` adds the external
definition `test/fixtures/catalog-fourth.json` to an isolated server and drives
it through the server and the frozen CLI with the deterministic CPU engine. When
the freeze file does not exist, the script first writes a manifest of the current
build (`build/lemond`, `build/lemonade`, the generated registry headers, every
file under `build/resources` and `src/cpp/resources`, the definition schema and
the fake engine) and then checks that none of those artifacts change. The UI
exercise freezes the same artifact set. Freeze manifests and transcripts are
written on demand and are not committed.
