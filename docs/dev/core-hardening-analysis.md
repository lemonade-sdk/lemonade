# Core Hardening — Current-State Analysis

This document records the current structural state of the C++ codebase as the baseline for
the core-hardening effort (see `core-hardening-plan.md`). Every claim below was verified
against the tree at the branch point; file:line references are from that snapshot and will
drift as the plan lands.

## 1. Executive summary

The codebase has one real library (`lemonade-server-core`, an OBJECT library) and four
executables, but the CLI and tray bypass the library and recompile server sources by
relative path. There is no typed JSON anywhere on the API surface: ~4,000+ string-keyed
JSON operations, zero round-tripping DTOs outside the job engine, and several live bug
classes caused directly by that. The HTTP layer has no handler abstraction: `server.cpp`
is an 8,277-line translation unit containing the route table, ~60 handler bodies,
static-file serving, and a download job manager, with a ~40-line
auto-load/validate/error-map preamble copy-pasted across 13 inference handlers. The
Anthropic and Ollama compatibility layers are hand-written json→json translators bolted
onto a single `OllamaApi` class, each independently reimplementing SSE parsing, error
shaping, and response unwrapping.

The good news: the backend layer already demonstrates the target architecture. The
descriptor + registry + factory system (`backend_descriptor.h`, `backend_registry.h`),
the capability interfaces with a compile-time `capability_mask_of<T>()`
(`server_capabilities.h:154`), the shared `WrappedServer::forward_*` / `StreamingProxy`
proxy layer, and the strict path-reporting parser in `routing_policy_parser.cpp` are all
in-repo precedents for exactly the patterns the hardening generalizes.

## 2. Build & target structure

### 2.1 Targets today

| Target | Kind | Sources |
|---|---|---|
| `lemonade-server-core` | OBJECT library (`CMakeLists.txt:1091`) | ~100 TUs: the 51-entry `SOURCES_CORE` list, platform sources, backend registry |
| `lemond` | exe (`CMakeLists.txt:1096`) | only `server/main.cpp`; links server-core |
| `lemonade` (CLI) | exe (`src/cpp/cli/CMakeLists.txt:70`) | ~24 TUs; does **not** link server-core |
| `lemonade-tray` (macOS/Linux) | exe (`tray/CMakeLists.txt:215,236`) | ~9 TUs; no server-core |
| `LemonadeServer` (Windows) | exe (`tray/CMakeLists.txt:174`) | tray sources + **all of server-core** (embeds the server) |
| 82 test executables | `add_cpp_ci_test` | three linking styles, see 2.3 |

### 2.2 The duplication problem

There is no `src/cpp/common/`. Shared utility code lives under `src/cpp/server/utils/`
and the CLI/tray **reach into it by relative path and recompile it**:

- Compiled into 3 targets each: `path_utils.cpp`, `url_utils.cpp`, `json_utils.cpp`,
  `platform/path_*.cpp`, `process_manager.cpp` (+ its platform files),
  `backend_descriptor_registry.cpp`.
- Compiled into 2: `http_client.cpp` (1,409 L), `recipe_options.cpp`,
  `model_registry.cpp`.
- Compiled into **12 targets**: `routing_policy.cpp` (server-core + 11 test exes).
  `routing_policy_parser.cpp` into 5, `directory_watcher.cpp` into 3.
- `cli/main.cpp:830–940` **reimplements** the UDP beacon listener inline even though it
  includes `network_beacon.h` — the `.cpp` just isn't in the CLI's source list.

Consequences: layering is unenforced (nothing stops a "utility" from growing a server
dependency), incremental builds recompile the same code up to 12×, and because
`lemonade-server-core` is an OBJECT library, the 50 tests that link it each absorb all
~100 object files — a change to `server.cpp` relinks all 50.

### 2.3 Test linking styles (all three exist)

1. Link all of server-core (50 targets) — heavyweight, relink-the-world.
2. Recompile a hand-picked subset of server sources (18 targets) — fast and isolated,
   but the source lists are maintained by hand in the root CMakeLists.
3. Header-only (14 targets) — the payoff of logic-in-headers units like `gguf_reader.h`
   (481 L), `auto_tune.h` (332 L), `job_expr.h` (320 L), `origin_utils.h`,
   `custom_args.h`.

Style 2 and 3 are what a proper library split makes the default: tests link the small
library they exercise.

## 3. HTTP layer

### 3.1 Server side — no handler abstraction

- `server.cpp` is 8,277 lines: `setup_routes` alone is ~429 lines (`:1099–1528`),
  `setup_static_files` ~362, plus ~60 `handle_*` member functions declared in
  `server.h:107–260`.
- **Quad-prefix registration exists in three idioms**: `register_get`/`register_post`
  lambdas (`:1150`, `:1158` — the POST variant also hand-writes four identical 405-GET
  bodies and a hardcoded `endpoint != "jobs"` special case), a
  `for (const char* prefix : {...})` loop (`:1215`, `:1369`), and four regex routes
  written out four times verbatim each (`:1201`, `:1230`, `:1278`, `:1498`).
- **The 13× preamble**: `auto_load_model_if_needed(...)` + identical
  try/catch → `create_model_error` → `get_http_status_from_error` → `set_content` →
  span-error block appears at 13 call sites (`:3965, :4101, :4223, :4283, :4388, :4710,
  :4778, :5030, :5167, :5221, :5323, :5651`). `:3961–3990` and `:4219–4247` are the same
  code with a different span label.
- **Live inconsistency bug**: `parse_required_json_body` (`:5717`) is used by only 7 of
  ~32 body-parsing handlers; the other 25 call `json::parse(req.body)` raw. Malformed
  JSON to `/v1/chat/completions` → 400; the same body to `/v1/embeddings` → 500.
- **Three overlapping error mechanisms**: anon-namespace helpers in server.cpp
  (`:117, :155, :162, :222`), `ErrorResponse::create/from_exception`
  (`error_types.h:120`), and inline raw-string JSON literals. A `bad_request` lambda is
  redefined identically at `:3107`, `:5740`, `:5980`. Aggregates: 94 `catch`, 114
  `res.status = 400`, 293 `"application/json"` literals in one file.
- **SSE framing is hand-constructed** (`"data: " + dump() + "\n\n"`) in ~10 files
  (router.cpp ×4, wrapped_server.cpp ×2, collection_orchestrator.cpp,
  streaming_proxy.cpp, cloud_server.cpp ×3).
- **Middleware**: exactly one `set_pre_routing_handler` (`:1101` — logging, CORS, auth).
  No exception handler, so an escaping exception becomes httplib's default 500 instead
  of a JSON error.
- **Four parallel route registries** on the same `httplib::Server` (`Server`,
  `OllamaApi`, `McpServer`, `McpClientManager`), each with private copies of
  set-json/set-error/parse-body helpers (`mcp_client.cpp:71,:76,:82`;
  `anthropic_api.cpp:161,:209`; `ollama_api.cpp:95`).

### 3.2 Client side — three unrelated HTTP stacks

1. **CLI**: `LemonadeClient` (`cli/lemonade_client.cpp`, 1,724 L) over cpp-httplib —
   `make_client`, `make_request`, hand-rolled SSE frame splitter. Genuinely shared
   within the CLI.
2. **Tray**: `tray_ui.cpp:179` is a near line-for-line copy of `LemonadeClient`'s
   bootstrap (same TLS guard, IPv6 bracketing, URL assembly); the env-var → bearer
   header block is pasted twice there and a third time in `tray/main.cpp:82–112`.
3. **Server outbound**: `lemon::utils::HttpClient` (libcurl, 1,409 L) — HF downloads,
   backend forwarding, telemetry. One CLI file crosses stacks
   (`cli/recipe_import.cpp:180`).

### 3.3 What is already good

`WrappedServer::forward_request/forward_get_request/forward_multipart_request/
forward_streaming_request` + `StreamingProxy` form a real shared proxy layer; per-backend
code mostly parameterizes it. Outliers: `CloudServer` (1,098 L, reimplements streaming
against `HttpClient` directly) and `Router::chat_completion_stream` /
`completion_stream` / `responses_stream` (`router.cpp:2539, :2693, :2808` — ~110-line
near-clones). Small duplications inside `wrapped_server.cpp` itself: the ~10-line
"backend dead → watchdog reset → throw" preamble ×4 and the catch-tail ×2.

## 4. JSON layer

### 4.1 Scale

`nlohmann/json.hpp` in 51 of 264 files; roughly 4,000+ string-keyed operations. Top
offenders: `server.cpp` ~578, `ollama_api.cpp` ~350, `router.cpp` ~312,
`cli/lemonade_client.cpp` ~281, `model_manager.cpp` ~269, `runtime_config.cpp` ~259,
`system_info.cpp` ~228, `anthropic_api.cpp` ~198. `json::parse` appears 165× (7 use the
non-throwing form). The dominant idiom is `operator[]` + `contains()` — the
silent-miss-prone combination; `.at()` is nearly unused.

### 4.2 No typed DTOs

Zero occurrences of `NLOHMANN_DEFINE_TYPE_*`, `adl_serializer`, or free
`to_json/from_json` pairs. The only true round-tripping DTO family is
`jobs/job_types.h` (`Job`/`StepRecord`/`Case`). Everything else is serialize-only
one-offs (`Telemetry::to_json`, `CostInfo::to_json`, `model_info_to_json`, …) with no
compile-time link between producer and consumer.

### 4.3 Live bug classes caused by stringly typing

- **`"stream"` handled three different ways**: fully guarded (`server.cpp:3637`);
  unguarded `contains && get<bool>()` at `:4009, :4145, :4816, :5668` — a client sending
  `"stream": "true"` throws `type_error.302` out of the handler; `.value("stream",
  false)` elsewhere — and Ollama defaults it to **true** (`ollama_api.cpp:755`) while
  everything else defaults false.
- **`choices[0]` unwrap duplicated 13+ times with divergent guards**; some omit
  `is_array()`; `router.cpp:2574` indexes a non-const json so a missing `"delta"`
  silently materializes null and downstream `contains("content")` quietly yields
  nothing.
- **Silent degradation**: `JsonUtils::get_or_default` (38× in model_manager.cpp) means a
  renamed registry field becomes `""`/`0.0` with no diagnostic.
- **Asymmetric pairs**: registry ingest (`model_manager.cpp:2943` and a near-identical
  ~70-line clone at `:3022`) vs. `model_info_to_json` (`server.cpp:3189`) use different
  key sets; `mcp_client.h` parse/serialize likewise.
- **`ordered_json` boundary**: the job engine's `ordered_json` forces
  dump-then-reparse at `server.cpp:438` and `:467`.
- **Key constants**: none. `"model"` in 30 files, `"error"` 235 sites, `"ctx_size"` in
  20 files.

### 4.4 The in-repo gold standard

`routing_policy_parser.cpp:17–62` (`require_object`, `reject_unknown_keys`,
`required_field`, path-reporting errors, typed `RoutePolicy` output, schema files locked
by `test/test_schema_lock.py`) is the only strict parser in the tree and the model to
generalize. nlohmann is pinned at ≥ 3.11.3 (`CMakeLists.txt:214`), which supports
everything a compile-time field-list codec needs — no dependency bump required.

## 5. Protocol compatibility layers (Anthropic / Ollama)

The Anthropic Messages endpoint is implemented as methods on `OllamaApi`
(`anthropic_api.cpp:401` `OllamaApi::register_anthropic_routes`) — it lives there for no
structural reason. Both compat layers decompose into the same four pieces, hand-rolled
twice:

1. **Request remap in** — `convert_anthropic_to_openai_chat` (`anthropic_api.cpp:407`,
   ~290 lines), `convert_ollama_to_openai_chat` / `_completion`.
2. **Dispatch** — the same Router call and auto-load logic as the OpenAI handlers.
3. **Response remap out** — `convert_openai_chat_to_anthropic` (`:699`),
   `convert_openai_chat_to_ollama`.
4. **Stream transcode** — `stream_openai_sse_to_anthropic_sse` (`:805`, ~330 lines) and
   `stream_sse_to_ndjson`, both re-implementing SSE parse/buffer around a per-protocol
   event emitter; plus per-protocol error shaping (`set_anthropic_error_response`,
   `send_backend_error`).

The OpenAI-native handlers in server.cpp are the same pipeline with an identity remap.
This is the textbook CRTP-adapter shape: one base owns parse → resolve → dispatch →
stream/buffered branch → SSE framing → error mapping; a derived adapter supplies only
the remap functions and its route table. A future protocol becomes one remap file.

## 6. Where CRTP genuinely fits — and where it doesn't

**Fits:**

- **JSON codec** — `JsonEntity<Derived>` (or free-function reflection over a
  `Derived::fields()` constexpr descriptor list): generates `to_json`/`from_json`/
  strict-validate at compile time from a single field declaration; no string keys at
  call sites; unknown-key and type errors carry JSON-pointer paths. C++17-compatible
  (member-pointer + name descriptor tuples; no C++26 reflection needed).
- **Endpoint handlers** — `Endpoint<Derived>`: Derived declares its typed Request/
  Response DTOs, path, and a `handle(Context&, Request) -> Result` body; the base owns
  body parse, validation, auto-load, error mapping, SSE vs. buffered, quad-prefix +
  405-GET registration. Zero virtual dispatch; the route table stores type-erased
  thunks generated per-Derived.
- **Protocol adapters** — `ProtocolAdapter<Derived>` as described in §5. OpenAI is the
  identity adapter; Anthropic and Ollama become pure remaps of their existing
  conversion functions, retyped to canonical DTOs.
- **Backend mixins** — small shared behaviors (e.g. body-mutating streaming forwarders
  currently overridden in llamacpp/vllm/fastflowlm) can be CRTP mixins over the
  existing classes.

**Does not fit (keep virtual):**

- `WrappedServer` at the Router boundary. The Router holds heterogeneous
  `unique_ptr<WrappedServer>` collections, does LRU/eviction over them, and
  `supports_capability<T>` uses `dynamic_cast`. That is inherent runtime polymorphism;
  forcing CRTP there would mean type erasure that re-invents vtables. The existing
  descriptor/registry/factory + capability-mask design is already the right shape —
  the hardening builds on it rather than replacing it.

## 7. Inventory of assets to build on

| Asset | Location | Role in hardening |
|---|---|---|
| Backend descriptor + factory registry | `backends/backend_descriptor.h`, `backend_registry.h` | pattern for endpoint/adapter registries |
| Capability interfaces + compile-time mask | `server_capabilities.h` | precedent for compile-time contract checks |
| `forward_*` + `StreamingProxy` | `wrapped_server.h/.cpp`, `streaming_proxy.*` | the proxy layer the adapter base dispatches into |
| Strict parser discipline | `routing_policy_parser.cpp` + schemas + `test_schema_lock.py` | model for the JSON codec's error reporting |
| Round-trip DTOs | `jobs/job_types.h` | proves the DTO style works in-tree |
| Header-only logic units | `gguf_reader.h`, `auto_tune.h`, `job_expr.h`, `custom_args.h`, … | already library-shaped; move as-is |
| `add_cpp_ci_test` + `cpp-ci-tests` | root CMakeLists | test registration stays; linking gets cheaper |
