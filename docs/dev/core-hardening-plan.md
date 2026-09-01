# Core Hardening — Plan

Companion to `core-hardening-analysis.md` and `core-hardening-standards.md`. Goal:
split the tree into layered libraries
with thin executables, put a compile-time typed JSON codec under the whole API surface,
and rebuild HTTP handling around CRTP endpoint and protocol-adapter bases so a compat
layer (Anthropic, Ollama, future protocols) is only a remap.

Every phase is independently landable, keeps all four executables building on all three
platforms, and keeps the existing Python integration tests green. Phases are ordered so
each one makes the next one mechanical.

## Target library layout

```
src/cpp/
  core/            → lemon-core        (STATIC)  paths, process, strings, url, logging,
                                                 error types, platform abstractions,
                                                 archive, beacon, single_instance
  json/            → lemon-json        (header-only INTERFACE)  field descriptors, codec,
                                                 strict parser, JSON-pointer errors
  net/             → lemon-net         (STATIC)  HttpClient (libcurl), ApiClient
                                                 (httplib, the one true loopback/remote
                                                 client), SSE reader/writer, multipart
  api/             → lemon-api         (STATIC)  typed DTOs shared by server, CLI, tray:
                                                 chat/completions/embeddings/rerank,
                                                 stream deltas, errors, model info,
                                                 config, health/system-info
  serverlib/       → lemon-server     (STATIC)  router, model manager, backends,
                                                 endpoints/, adapters/, jobs, websocket
  server/main.cpp  → lemond            (exe)     thin main
  cli/             → lemonade          (exe)     thin: arg parsing + lemon-net + lemon-api
  tray/            → lemonade-tray / LemonadeServer (exe)  thin: UI + lemon-net + lemon-api
```

Dependency rule, enforced by `target_link_libraries` visibility (a lower layer never
links a higher one):

```
core ← json ← net ← api ← serverlib
```

STATIC rather than OBJECT so tests link only the layer they exercise, the layering is
enforced by the linker, and a change to `server.cpp` stops relinking 50 test binaries.
(`lemonade-server-core` OBJECT lib is dissolved into `lemon-*`.)

## Phase 0 — Library skeleton and mechanical moves (no behavior change)

1. Create the five library targets; move `src/cpp/server/utils/` → `src/cpp/core/` and
   `src/cpp/server/utils/platform/` → `src/cpp/core/platform/` (headers from
   `include/lemon/utils/` follow). Move `network_beacon.*`, `single_instance.h`,
   `error_types.h`, logging into `core/`. Move `http_client.*`, `streaming_proxy.*`
   into `net/`.
2. CLI and tray: delete every relative-path recompile
   (`cli/CMakeLists.txt:18–47`, `tray/CMakeLists.txt:128–150`) and link `lemon-core` /
   `lemon-net` instead. Delete the inline beacon re-implementation in
   `cli/main.cpp:830–940` in favor of the real `network_beacon.cpp`.
3. Tests: routing-suite and other "hand-picked subset" tests (18 targets) link the new
   small libraries instead of listing sources; the 50 server-core tests link
   `lemon-server`.
4. Unify the three HTTP client bootstraps now (it is pure motion): promote
   `LemonadeClient`'s `make_client`/bearer/IPv6/TLS logic into `lemon::net::ApiClient`;
   CLI keeps `LemonadeClient` as a thin veneer over it; tray drops its two copies
   (`tray_ui.cpp:179`, `tray/main.cpp:82–112`).

Exit: zero `.cpp` compiled into more than one product target; `ctest` and Python suites
green; per-target TU counts recorded in the PR description.

## Phase 1 — lemon-json: compile-time codec

The codec is the foundation for phases 2–4, so it lands first with its own unit tests.

### Design

A single declaration per type; keys are derived from member names at compile time —
no manually written `j["key"]` anywhere at call sites:

```cpp
struct ChatMessage {
    std::string role;
    JsonValue content;                       // string | content-part array (raw passthrough)
    std::optional<std::string> name;
    std::optional<std::vector<ToolCall>> tool_calls;

    LEMON_JSON(ChatMessage, role, content, name, tool_calls);
};
```

`LEMON_JSON(...)` expands to a `static constexpr auto lemon_fields()` returning a tuple
of `field_descriptor{member-pointer, name}` (names from preprocessor stringification —
compile-time, spelled once, never at a use site). Over that tuple, fold expressions
generate:

- `to_json(const T&) -> json` — omits empty `std::optional`, emits declared order.
- `from_json_strict(const json&) -> Expected<T, ParseError>` — type-checked per field,
  `ParseError` carries a JSON-pointer path (`/messages/2/tool_calls/0/id`) and reason;
  policy enum for unknown keys: `Reject` (config, registry) or `Preserve` (API bodies —
  unknown keys are captured into a `json extras` member and re-emitted, so passthrough
  to backends keeps fields we don't model).
- `from_json_lenient` — defaults applied, used only where today's behavior must be
  preserved during migration.

Supported field kinds: scalars, `std::optional<T>`, `JsonOptional<T>` (tri-state, see
below), `std::vector<T>`, `std::map`, nested `LEMON_JSON` types, enums declared with
`LEMON_ENUM` (the X-macro enum + wire-name mechanism defined in
`core-hardening-standards.md` §2, which also delivers the central vocabulary header
`include/lemon/core/vocab.h` in this phase), `JsonValue` (escape hatch for genuinely
dynamic subtrees — content parts, tool arguments), and renamed keys via
`lemon::json::named<&T::member>("wire_name")` for the few wire names that aren't valid
identifiers.

### Tri-state fields: `JsonOptional<T>`

`std::optional<T>` conflates two wire states that this API surface must distinguish:
**key absent** and **key present with `null`** (OpenAI's `"stop": null` vs. omitted,
nullable telemetry like `cache_tokens`, and PATCH-style partial config updates where
"not mentioned" must not clobber and `null` means "clear"). `JsonOptional<T>` is the
codec's tri-state field type — unset / explicitly null / value — adapted from the
donor implementation vendored at `docs/dev/reference/json_optional_reference.md`
(bitfield flags for `is_set`/`is_null`, an `adl_serializer` that exploits the fact
that `from_json` only runs when the key exists, and `underlying_type_t`/trait helpers
the codec's field loop dispatches on).

Uniform free-function queries are part of the API, overloaded across all three field
kinds so call sites don't care which one a DTO chose:

```cpp
is_set(req.temperature)      // JsonOptional: was the key present at all?
is_null(req.stop)            // JsonOptional: present and explicitly null?
has_value(req.model)         // T (always true) / optional / JsonOptional
```

Adaptation notes against the reference (deliberate deltas, recorded so review happens
once):

1. **Unset must omit the key on serialize.** The reference's `SAFE_JSON_TO` /
   `adl_serializer::to_json` emit `null` for an unset field, so unset → null after one
   round trip and the tri-state collapses. An `adl_serializer` *cannot* omit a key (it
   only sees the value slot), so tri-state emission lives in the codec's field loop —
   which knows the key — with the serializer kept only for nested/non-codec contexts:
   unset → key omitted, null → `"key": null`, value → serialized value. This is what
   makes round trips lossless.
2. **Single source of truth for state.** The reference stores `is_null` alongside
   `value.has_value()`, which can drift (and its default constructor starts as
   `is_set=0, is_null=1`). The adaptation derives null-ness from
   `is_set && !value.has_value()` where possible, keeps the flag byte only for
   `is_set`, and `static_assert`s the invariants in tests.
3. **Portability of the flag bits.** The anonymous-struct-in-union bitfield is a
   compiler extension (accepted by MSVC, GCC, and AppleClang — the three compilers CI
   requires — but warned under `-Wpedantic`). The adaptation keeps the same public
   API but may back it with a plain flags byte + accessors if the warning budget
   demands; either way the layout stays one byte.
4. Small API trims: one `has_value()` (const), `value_or` takes by const-ref,
   implicit `operator const T&` (which throws on unset) becomes explicit `get()` /
   `operator*` only, and `[[nodiscard]]` per the standards doc. The pqxx block is
   inert here and dropped from the adapted header.

`std::optional<T>` remains the right type for plain omit-or-value fields where `null`
has no distinct meaning; DTO declarations choose per field.

C++17-only (member-pointer tuples + folds), header-only, works with the pinned
nlohmann ≥ 3.11.3, and interoperates with plain `nlohmann::json` at the edges. The
`ordered_json`/`json` job-engine boundary gets a direct converter to end the
dump-then-reparse at `server.cpp:438/:467`.

### First DTO wave (in `lemon-api`)

Ordered by leverage:

1. `ErrorBody` / `ApiError` — replaces the three error mechanisms; one
   `to_http_status()` mapping.
2. `ChatCompletionRequest/Response`, `Choice`, `Usage`, `StreamChunk`/`StreamDelta` —
   ends the 13× divergent `choices[0]` unwrap and the four unguarded
   `["stream"].get<bool>()` sites.
3. `EmbeddingsRequest/Response`, `RerankRequest/Response`, `CompletionRequest`.
4. `ModelInfo` wire form — one symmetric codec replacing the duplicated ~70-line ingest
   blocks (`model_manager.cpp:2943`, `:3022`) and `model_info_to_json`
   (`server.cpp:3189`).
5. `config.json` / `RuntimeConfig` schema, `backend_versions.json` schema — strict,
   unknown-key-rejecting (per the routing-policy precedent).
6. Health / system-info structs — `system_info.h` types gain codecs instead of the
   hand-built dict functions.

Tests: round-trip property tests per DTO; a schema-lock-style test asserting the wire
key set of each DTO (renames become deliberate).

## Phase 2 — lemon-server endpoints: CRTP handler framework

### Design

```cpp
struct ApiContext {                 // owned by Server, passed by ref
    Router& router;
    ModelManager& models;
    RuntimeConfig& config;
    Telemetry& telemetry;
};

template <typename Derived>
class Endpoint {
public:
    // Base owns: body parse via Derived::Request codec (uniform 400 with pointer
    // path), auto-load preamble, dispatch, ApiError → status/body mapping, SSE vs
    // buffered branch, span lifecycle, quad-prefix + auto-405-GET registration.
    static void register_routes(httplib::Server& s, ApiContext& ctx);
protected:
    // Derived provides, checked at compile time (static_assert on detection idiom):
    //   static constexpr const char* path();            // "chat/completions"
    //   using Request  = ...;  using Response = ...;    // LEMON_JSON DTOs
    //   static constexpr bool streamable = ...;
    //   Result handle(ApiContext&, Request&&);          // buffered
    //   void   handle_stream(ApiContext&, Request&&, SseWriter&);  // if streamable
};
```

- One `register_quad(server, method, path, thunk)` helper replaces all three
  registration idioms, the four hand-written 405 bodies, and the four verbatim regex
  quadruplicates.
- `SseWriter` in `lemon-net` becomes the only place `"data: ...\n\n"` framing exists;
  `SseReader` the only SSE parser (replacing the copies in `lemonade_client.cpp`,
  `ollama_api.cpp`, `anthropic_api.cpp`, `cloud_server.cpp`).
- An `set_exception_handler` is installed so an escaping exception is a JSON 500, not
  httplib's default.
- Endpoints live one-per-file under `serverlib/endpoints/`; `server.cpp` shrinks to
  listener setup, middleware, static files, and endpoint registration.

### Migration order

1. Extract the error module and `register_quad`; port the non-inference endpoints
   (health, models, system-info, pull/delete/load/unload) — highest count, simplest
   shapes.
2. Port the inference endpoints (chat/completions/embeddings/rerank/classify/responses)
   onto the shared auto-load preamble — deletes the 13× block.
3. Standardize body parsing: `parse_required_json_body` semantics become the base-class
   default, fixing the 400-vs-500 inconsistency across the remaining 25 handlers.
4. Move the download-job manager and static-file serving out of `server.cpp` into their
   own files.

Exit: `server.cpp` < ~1,000 lines; zero raw `json::parse(req.body)` in handlers; a CI
grep gate forbids `req.body` and `["` string-key access inside `endpoints/`.

## Phase 3 — Protocol adapters: compat layers as pure remaps

```cpp
template <typename Derived>
class ProtocolAdapter {
    // Base owns the full pipeline: parse → Derived::map_request → model resolve /
    // auto-load → Router dispatch → (buffered) Derived::map_response |
    // (streaming) canonical-delta loop feeding Derived::Transcoder → SSE/NDJSON
    // framing per Derived::framing() → Derived::map_error on any failure.
};

class OpenAiAdapter    : ProtocolAdapter<OpenAiAdapter> { /* identity remap */ };
class AnthropicAdapter : ProtocolAdapter<AnthropicAdapter> {
    ChatCompletionRequest map_request(const AnthropicMessagesRequest&, Warnings&);
    AnthropicMessagesResponse map_response(const ChatCompletionResponse&, Warnings&);
    struct Transcoder {   // canonical deltas in, Anthropic events out
        void on_start(...); void on_text(...); void on_tool_delta(...);
        void on_stop(StopInfo);
    };
    AnthropicError map_error(const ApiError&);
    static constexpr auto routes = ...;   // POST /api/messages, /v1/messages
};
class OllamaAdapter    : ProtocolAdapter<OllamaAdapter> { /* NDJSON framing */ };
```

- The canonical delta stream (`StreamDelta` from lemon-api) is produced once by the
  router streaming path; transcoders never see SSE text, only typed deltas — the ~330
  lines of `stream_openai_sse_to_anthropic_sse` reduce to the event-emission logic.
- Anthropic moves out of `OllamaApi` into its own adapter; the existing conversion
  functions become the bodies of `map_request`/`map_response`, retyped to DTOs (this is
  a port, not a rewrite — the mapping rules and warning behavior are preserved
  verbatim, including the Anthropic upstream passthrough path).
- Divergent-behavior inventory (stream default true for Ollama, warning accumulation,
  Ollama's auto-load options) is expressed as adapter traits, not re-derived.
- A new protocol = one adapter file with remaps + a transcoder; conformance is a
  fixture suite replayed through all adapters against golden wire outputs.

## Phase 4 — Streaming/router cleanup and remaining consolidation

1. Collapse `Router::chat_completion_stream` / `completion_stream` /
   `responses_stream` (~110-line near-clones) into one parameterized implementation
   producing canonical deltas.
2. Fold the ×4 "backend dead → watchdog reset → throw" preamble and ×2 catch-tail in
   `wrapped_server.cpp` into a private helper; give `CloudServer` the shared
   `SseReader` instead of its private reassembly.
3. Sweep remaining stringly hotspots onto DTOs: `runtime_config.cpp` getters over the
   typed config, `telemetry.cpp` OTLP chains, `backend_manager.cpp` version chains,
   CLI (`lemonade_client.cpp`, `bench.cpp`, `chat_repl.cpp`) onto lemon-api DTOs —
   which also makes CLI/tray output shapes compile-time-checked against the server.

## Verification & guardrails (every phase)

- Build all presets (`default`, `windows`, `debug`), `ctest -L cpp-ci`, and the Python
  suites `server_cli2.py` / `server_endpoints.py` (plus `server_llm.py` when routing or
  streaming paths are touched).
- Behavioral parity: capture golden request/response fixtures for every public endpoint
  (including the malformed-JSON status codes, which change deliberately in Phase 2 step
  3 and are called out as such) before each phase, replay after.
- CI grep gates added incrementally: no `#include "../server/` from cli/ or tray/; no
  `json::parse(req.body)` outside the endpoint base; no `"data: "` framing outside
  `SseWriter`; DTO wire-key lock tests; the coding-standards gates from
  `core-hardening-standards.md` §4 (no new `std::pair<bool`, no stringly bools, enum
  wire names covered by the lock tests).
- Distro constraints respected: `BUILD_TESTING=OFF` still builds no test targets; the
  web-app package.json split and system-nlohmann ≥ 3.11.3 path are untouched;
  `add_cpp_ci_test` remains the only test registration mechanism.

## Explicit non-goals

- No change to the `WrappedServer` virtual hierarchy at the Router boundary (see
  analysis §6) — the descriptor/registry/factory system is kept and imitated, not
  replaced.
- No wire-format changes to any public API (except the documented 400-vs-500 fix).
- No new third-party dependencies; no C++ standard bump.
- Tauri/web frontend untouched.

## Sequencing summary

| Phase | Deliverable | Depends on | Rough size |
|---|---|---|---|
| 0 | 5 libraries, thin exes, one client bootstrap | — | mostly mechanical, large diff / low risk |
| 1 | lemon-json codec + first DTO wave + tests | 0 | new code, no callers broken |
| 2 | Endpoint CRTP base, server.cpp dismantled | 1 | many small PRs, one endpoint family each |
| 3 | ProtocolAdapter, Anthropic/Ollama as remaps | 1, 2 | port of existing conversions |
| 4 | Router stream unification, long-tail sweeps | 2, 3 | incremental |
