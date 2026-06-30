# Adding a backend

Lemonade backends are **self-describing**. A backend declares *what it is* in a
plain-data **descriptor** and implements *how it runs* in a **server class**, and
both live together in the backend's own folder. A registry collects every
descriptor, and the router, the CLI, `/system-info`, and the generated docs all
read it — so there are no scattered `if (recipe == "...")` sites to update.

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

Everything for one backend lives in `lemon::backends::<stem>`. The descriptor is
header-only so it links into **both** the `lemonade` CLI and `lemond`; the server
class and `create()` are server-only (compiled into `lemond`).

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

`SlotPolicy` controls accelerator sharing: `Standard` (counts toward LRU slots),
`ExclusiveNpu` (evicts all NPU servers first), `CoexistByType` (one per model
type), `Unmetered` (never counted, never auto-evicted — cloud).

## The server class + factory — `<stem>/<stem>_server.{h,cpp}`

The server class is a `WrappedServer` subclass. Implement `load()`, `unload()`,
and only the capability interfaces you serve (`ITranscriptionServer`,
`IImageServer`, `ITextToSpeechServer`, …). `WrappedServer` provides default
"unsupported" `chat_completion`/`completion`/`responses`, so a non-chat backend
does not stub them. Alongside it, a free `create()` builds the instance.

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

The `foreach` in `CMakeLists.txt` compiles `<stem>/<stem>_server.cpp` and
regenerates the registry headers, binding `<stem>::descriptor` to `<stem>::create`.

## What you get for free

- **Standard options:** `merge_args`, `auto_evict`, `evict_idle_timeout`,
  `downsize_idle_timeout`, `evict_weight_factor`, `pinned`. `ctx_size` is opt-in
  via `uses_ctx_size`.
- **Generated CLI flags** for every descriptor option with a `cli_flag`, plus
  `--<recipe>` when `selectable_backend = true`.
- **Install/download** via the backend's `BackendSpec` (binary + install params).
- **`/system-info`** `recipes` entry (display name, options schema, support matrix).
- **Generated docs** — your backend appears automatically in
  [`backends-reference.md`](backends-reference.md), the README "Supported
  Configurations" matrix, and the multi-model NPU-exclusivity list. A CI job
  (`backend-docs-drift`) fails if the committed docs are stale. The descriptor's
  `modality`, `experimental`, `web_display_name`, and each support row's
  `device_summary` supply the editorial bits the matrix needs.

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

**Moonshine** is the minimal case: a single descriptor option, no backend
selection, CPU-only, one capability interface. See
`src/cpp/server/backends/moonshine/` and `include/lemon/backends/moonshine/`.

> Note: collections (`collection.omni`) are orchestrator-driven, not
> `WrappedServer` subprocesses, and are the one explicit exception to this model.
