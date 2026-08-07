# Adding a backend

Lemonade backends are **self-describing**. A backend declares *what it is* in a plain-data **descriptor** and implements *how it runs* in a **server class**, and both live together in the backend's own folder. A registry collects every descriptor, and the router, the CLI, `/system-info`, and the generated docs all read it — so there are no scattered `if (recipe == "...")` sites to update.

Adding a backend is **one folder plus three small appends**:

| You edit | What goes there |
|----------|-----------------|
| `CMakeLists.txt` → `LEMON_BACKENDS` | **one line**: `"<recipe>\|<stem>"` |
| `src/cpp/include/lemon/backends/<stem>/<stem>.h` | the descriptor (header-only `inline const`) |
| `src/cpp/include/lemon/backends/<stem>/<stem>_server.h` | the `WrappedServer` subclass + `create()` declaration |
| `src/cpp/server/backends/<stem>/<stem>_server.cpp` | the implementation + `create()` definition |
| `src/cpp/resources/backend_versions.json` | version pin(s) — skip if there's no downloaded binary (e.g. cloud) |
| `src/cpp/resources/server_models.json` | the models |

No router edits, no CLI edits, no doc edits, no support-matrix edits.

That covers the backend's wiring. You still write an integration test ([Testing](#testing)). If the backend introduces a new endpoint or capability instead of serving one that already exists, you also write that endpoint, its router plumbing, and its API docs ([Adding a new endpoint or capability](#adding-a-new-endpoint-or-capability)).

Everything for one backend lives in `lemon::backends::<stem>`. The descriptor is header-only so it links into **both** the `lemonade` CLI and `lemond`; the server class and `create()` are server-only (compiled into `lemond`).

## The descriptor — `<stem>/<stem>.h`

Plain data. The single object the registry, CLI, `/system-info`, and docs all read.

```cpp
#pragma once
#include "lemon/backends/backend_descriptor.h"

namespace lemon { namespace backends { namespace myrecipe {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "myrecipe",
    /*display_name*/    "My Backend",
    /*binary*/          "my-server",        // "" = no subprocess (e.g. cloud)
    /*config_section*/  "myrecipe",         // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,           // true auto-exposes "<recipe>_backend" + "--<recipe>"
    /*uses_ctx_size*/   true,               // opt in to the shared ctx_size option
    /*dynamic_models*/  false,              // true = models discovered at runtime (cloud)
    /*options*/ {                           // backend-specific knobs (common ones are automatic)
        {"myrecipe_args", "--myrecipe-args", "", "ARGS", "Custom args to pass", "My Options"},
    },
    /*support*/ {                           // OS / device families ({} = no local gating)
        {"myrecipe", "cpu", {"linux", "windows"}, {{"cpu", {"x86_64"}}}},
    },
    /*default_labels*/  {},                 // labels injected when a model omits them
    /*required_checkpoints*/ {"main"},      // unconditional files; conditional ones checked in load()
};

}}}  // namespace lemon::backends::myrecipe
```

`SlotPolicy` controls accelerator sharing: `Standard` (counts toward LRU slots), `ExclusiveNpu` (evicts all NPU servers first), `CoexistByType` (one per model type), `Unmetered` (never counted, never auto-evicted — cloud).

## The server class + factory — `<stem>/<stem>_server.{h,cpp}`

The server class is a `WrappedServer` subclass. Implement `load()`, `unload()`, and only the capability interfaces you serve (`ITranscriptionServer`, `IImageServer`, `ITextToSpeechServer`, …). `WrappedServer` provides default "unsupported" `chat_completion`/`completion`/`responses`, so a non-chat backend does not stub them. Alongside it, a free `create()` builds the instance.

`<stem>_server.h`:

```cpp
#pragma once
#include "lemon/backends/backend_registry.h"   // BackendContext
#include "lemon/wrapped_server.h"

namespace lemon { namespace backends {

class MyServer : public WrappedServer, public ICompletionServer {
    // load(), unload(), the capability methods you serve …
};

namespace myrecipe {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);  // server-only
}

}}  // namespace lemon::backends
```

`<stem>_server.cpp`:

```cpp
#include "lemon/backends/myrecipe/myrecipe_server.h"
// … MyServer method definitions …

namespace lemon { namespace backends { namespace myrecipe {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return std::make_unique<MyServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}
}}}  // namespace lemon::backends::myrecipe
```

## Register it: one line

```cmake
set(LEMON_BACKENDS
    ...
    "myrecipe|myrecipe"   # "<recipe>|<stem>"
)
```

The `foreach` in `CMakeLists.txt` compiles `<stem>/<stem>_server.cpp` and regenerates the registry headers, binding `<stem>::descriptor` to `<stem>::create`.

## What you get for free

- **Standard options:** `merge_args`, `auto_evict`, `evict_idle_timeout`, `downsize_idle_timeout`, `evict_weight_factor`, `pinned`. `ctx_size` is opt-in via `uses_ctx_size`.
- **Generated CLI flags** for every descriptor option with a `cli_flag`, plus `--<recipe>` when `selectable_backend = true`.
- **Install/download** via the backend's `BackendSpec` (binary + install params).
- **`/system-info`** `recipes` entry (display name, options schema, support matrix).
- **Generated docs** — your backend appears automatically in [`backends-reference.md`](backends-reference.md), the README "Supported Configurations" matrix, and the multi-model NPU-exclusivity list. A CI job (`backend-docs-drift`) fails if the committed docs are stale. The descriptor's `modality`, `experimental`, `web_display_name`, and each support row's `device_summary` supply the editorial bits the matrix needs.

## Testing

The descriptor wires your backend in, but nothing exercises it. Write an integration test in `test/`; these run against a live server. `test/server_sd.py` is the model to follow: it pulls the model, calls the endpoint on each backend variant, and asserts the response is real output (a valid `RIFF` or `PNG` header and a substantial byte count) rather than an empty body or a backend error served with a success status. Test the failure paths too: a missing required field, and an error raised by the backend.

- Existing modality: the endpoint already has a test (another image, transcription, or LLM backend). Add your recipe as a `--wrapped-server` case to that script instead of writing a new one.
- New modality: add `test/server_<modality>.py` modeled on `server_sd.py`, and a `test_models` entry under the modality in `test/utils/capabilities.py`.

Add the test to CI in both matrices of `.github/workflows/cpp_server_build_test_release.yml`, the Windows block and the Linux block, with one row per backend variant:

```yaml
- name: <modality>-<recipe>
  script: server_<modality>.py
  extra_args: "--wrapped-server <recipe>"
  backends: "vulkan rocm"
  runner: [Linux, vulkan, rocm, lemon-prod]
```

If your backend uses ROCm via TheRock, add its recipe to `_THEROCK_RECIPES` in `test/utils/server_base.py`. Otherwise a cold runner folds the one-time TheRock download into the first request's timeout instead of `TIMEOUT_ROCM_INSTALL`, and the ROCm job flakes.

## Adding a new endpoint or capability

Serving an existing capability interface (`ITranscriptionServer`, `IImageServer`, `ITextToSpeechServer`) needs nothing beyond the folder above. The endpoint, router plumbing, and API docs already exist, and the endpoint's test already covers any backend that serves it.

A new capability, a request type with no existing endpoint, is a larger change. The descriptor cannot generate the request path, so you also edit the router and server:

| You add | Where |
|---------|-------|
| Capability interface `I<Thing>Server` | `src/cpp/include/lemon/server_capabilities.h` |
| A `ModelType` value and its label mapping | `src/cpp/include/lemon/model_types.h`. Add it to `Router::get_pinned_model_counts` so loaded models of the new type are counted. |
| Router method that `dynamic_cast`s to your interface and dispatches | `src/cpp/server/router.h`, `src/cpp/server/router.cpp` |
| Endpoint handler, registered with `register_post` or `register_get` | `src/cpp/server/server.cpp`. One call registers all four `/api/v0`, `/api/v1`, `/v0`, `/v1` prefixes ([invariant 1](../../AGENTS.md)). |
| API documentation | `docs/api/` |

Pick the API-doc file by protocol: extend `docs/api/openai.md` for an OpenAI-compatible endpoint, add a file alongside `docs/api/llamacpp.md` when you mirror another server's standard, or use `docs/api/lemonade.md` for a Lemonade-specific endpoint. Follow the API reference structure from the [documentation guide](documentation.md): an H2 `METHOD /path` heading, a status badge, a one-sentence description, a parameters table, a curl example, and the response format.

## Escape hatches

| Need | Hook |
|------|------|
| Device depends on the chosen backend variant (whisper npu vs cpu) | override `WrappedServer::effective_device(opts)` |
| Eviction rule depends on the variant | override `WrappedServer::effective_slot_policy(opts)` |
| Availability decided at runtime (cloud creds) | override `WrappedServer::availability()` |
| Conditional / grouped checkpoints (sd-cpp flux, whisper npu_cache) | validate in `load()`; list only unconditional files in `required_checkpoints` |
| Custom per-model fields without editing `ModelInfo` | read `model_info.extra<T>("my_field", fallback)` (populated from unknown `server_models.json` keys) |
| Models supplied at runtime, not from `server_models.json` | set `dynamic_models = true` and provide them in the class (see cloud's `discover_models()`) |
| Per-create setup before load (ryzenai `set_model_path`) | do it in `create()` |

## The simplest end-to-end example

**Moonshine** is the minimal case: a single descriptor option, no backend selection, CPU-only, one capability interface. See `src/cpp/server/backends/moonshine/` and `include/lemon/backends/moonshine/`.

> Note: collections (`collection.omni`) are orchestrator-driven, not `WrappedServer` subprocesses, and are the one explicit exception to this model.
