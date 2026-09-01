# Core Hardening — Coding Standards

Companion to `core-hardening-analysis.md` and `core-hardening-plan.md`. These rules
apply to all new code immediately and to existing code as the plan's phases touch it.
Each rule cites real instances found in the tree so the migration targets are concrete.

## 1. Returning "maybe a value"

### Rule

- A function that may or may not produce a value returns `std::optional<T>`.
- A function that produces a value **or a reason it couldn't** returns
  `Expected<T, E>` (the `lemon-json` `ParseError` result type from Phase 1 generalizes
  to `lemon::Expected` in `lemon-core`).
- `std::pair<bool, T>`, `bool f(T& out)`, and sentinel values (`-1`, `""`, `nullptr`
  where a value was expected) are not acceptable for new code.

`std::optional` is already used in 50 files — this codifies existing practice, not a
new idiom.

### Current violations to migrate

| Pattern | Instance | Replacement |
|---|---|---|
| `std::pair<bool, T>` | `tray_ui.h:68` `fetch_server_state()` → `std::pair<bool, std::vector<LoadedModelInfo>>` | `std::optional<std::vector<LoadedModelInfo>>` |
| bool + out-param | `model_types.h:134` `find_deployment_mode(labels, ModelType& out)`, `:180` `deployment_mode_of(label, ModelType& out)` | `std::optional<ModelType>` |
| bool + **two** out-params | `websocket_server.h:155` `authenticate_connection(wsi, std::string& out_token, bool& out_authenticated)` | return a small struct or `std::optional<AuthResult>` |
| bool + out-param | `gguf_reader.h:198` `read_gguf_metadata(GgufMetadata& out, path)` | `std::optional<GgufMetadata>` (or `Expected` with a parse reason) |
| bool + out-param + side-channel response | `server.h:308–309` `parse_n_from_form(req, res, json& out)` / `extract_image_from_form` | absorbed by the Phase 2 endpoint base (typed request parsing) |
| bool + out-param | `job_manager.h:31` `remove(id, bool& active_out)`, `recipe_import.h:30` `list_remote_recipe_directories(vector& out, ...)` | `std::optional` / `Expected` |

Sentinel precedent worth noting: `Telemetry::cache_tokens = -1` meaning "not reported"
(`wrapped_server.h:45`) needed a comment and a special JSON-null rendering to stay
safe — exactly the cost `std::optional<int>` removes.

## 2. Enums over raw strings

### Rule

Any closed vocabulary — a value that is compared with `== "literal"`, switched on, or
validated against a known set — is an `enum class`, not a `std::string`. Strings are
for open-ended user data, never for states, modes, kinds, levels, or variants.

### The measured problem

Top enum-like string comparisons in the tree today: `== "rocm"` ×22, `== "system"` ×16,
`== "true"` ×13 (a **stringly boolean**), `== "rocm-stable"` ×12, `== "cloud"` ×12,
`== "auto"` ×9, plus job statuses (`"completed"`, `"cancelled"`), log levels
(`log_level_ == "debug" || log_level_ == "trace"` in `wrapped_server.h:104`), model
modes (`"chat"`, `"embed"`, `"image"`), and boolean-ish `"yes"`/`"false"` ×9.

Existing enums show the cost of hand-rolled conversion: `model_state_to_string`,
`model_type_to_string`, `device_type_to_string` (`model_types.h:37,:83,:98`),
`slot_policy_to_string` (`backend_descriptor.h:37`) — 12 files carry one-way
`*_to_string` converters, and the reverse direction, where it exists at all, is a
bool+out-param (`deployment_mode_of`). `BackendOption::type_name`
(`backend_descriptor.h:18`) is a stringly enum — `"ARGS" | "SIZE" | "BACKEND" | "BOOL"`
spelled as literals across 13 files.

### The mechanism: `LEMON_ENUM` + one vocabulary header

A single X-macro declaration in `lemon-core` generates everything both directions:

```cpp
// include/lemon/core/enum.h provides the machinery; a vocabulary is declared once:
LEMON_ENUM(RocmChannel,
    (Stable,  "rocm-stable"),
    (Nightly, "rocm-nightly"));
```

expands to:

- `enum class RocmChannel { Stable, Nightly };`
- `constexpr std::string_view to_string(RocmChannel)` — total, switch-generated, no
  fallthrough default.
- `constexpr std::optional<RocmChannel> rocm_channel_from_string(std::string_view)` —
  the parse direction returns `optional`, per rule 1.
- `constexpr std::array<RocmChannel, N> values` + `count` — iteration for CLI help,
  validation messages, and exhaustive tests.
- Integration with the Phase 1 JSON codec: a `LEMON_ENUM` type is directly usable as a
  DTO field; wire serialization uses the declared wire name, and an unknown wire value
  is a `ParseError` with a JSON-pointer path — never a silent default.

**The vocabulary file.** Cross-cutting pre-defined types and keys live in one header,
`include/lemon/core/vocab.h`, so the closed vocabularies of the system are declared in
a single reviewable place: `LlamaBackendVariant` (vulkan/rocm/metal/cpu/cuda),
`RocmChannel`, `ConfigScope` (system/user), `JobStatus`, `LogLevel`,
`BackendOptionType` (replacing the `"ARGS"|"SIZE"|"BACKEND"|"BOOL"` strings),
`DeploymentMode` (chat/embeddings/reranking/…), and the wire-stable names for the
existing `ModelState` / `ModelType` / `DeviceType` / `SlotPolicy` enums (whose
hand-rolled converters are then deleted). Subsystem-local enums stay in their own
headers using the same macro; `vocab.h` is only for vocabularies shared across layers.

### Why not magic_enum (as the primary mechanism)

`magic_enum` was considered and is deliberately **not** the wire mechanism:

1. **Wire names are not identifiers.** `"rocm-stable"`, `"rocm-nightly"`, `"sd-cpp"`
   contain dashes; magic_enum derives names from C++ identifiers, so every such value
   needs a hand-written `customize::enum_name` specialization — at which point the
   names are hand-maintained anyway, without the single-declaration guarantee.
2. **Wire stability.** magic_enum couples the serialized form to the C++ identifier: a
   rename refactor silently changes the API/config wire format. `LEMON_ENUM` makes the
   wire name an explicit, reviewable string that a wire-key lock test can pin.
3. It relies on compiler pretty-function parsing with a bounded value range
   (±128 by default) and adds a third-party dependency the plan's non-goals exclude.

magic_enum remains a fine optional convenience for **debug-only** printing of
third-party or internal enums; if that need materializes it can be vendored like
`aixlog.hpp` — but nothing on a wire, in a config file, or in a user-facing message
may depend on it.

## 3. Booleans and small rules that follow

- **No stringly bools.** `"true"/"false"/"yes"` comparisons (13+ sites) become real
  `bool` at the parse boundary (the JSON codec / CLI11 does the conversion; inner code
  never sees the string).
- **Multi-flag results are structs.** Two or more related out-params or a
  `pair`/`tuple` of unnamed values become a named struct with named fields
  (`authenticate_connection` above; `ProcessInfo` in `wrapped_server.h:415` is the
  existing good example).
- **`[[nodiscard]]`** on every `optional`/`Expected`/status-returning function added
  under this standard, so an ignored failure is a compiler warning.
- **`std::string_view`** for non-owning read-only string parameters in new `lemon-core`
  / `lemon-json` code (matching `deployment_mode_of`'s existing signature style).
- **Enum exhaustiveness**: switches over `LEMON_ENUM` types omit `default:` so adding
  an enumerator produces `-Wswitch` warnings at every site that must handle it.

## 4. Enforcement

- New-code enforcement starts immediately via review; migration of the instances
  listed above is folded into the plan's phases (rule 1 items land with the phase that
  touches their file; the vocabulary header lands in Phase 1 alongside the JSON codec,
  since the codec consumes it).
- CI grep gates (added with Phase 1): no new `std::pair<bool`, no `== "true"` /
  `== "false"` outside parse boundaries, and the wire-key lock tests cover enum wire
  names exactly as they cover DTO field names.
