# Adding a backend

Lemonade backends are **self-describing**. A backend declares *what it is* in a
plain-data **descriptor** and implements *how it runs* in a **server class**. A
registry collects every descriptor, and the router, the CLI, `/system-info`, and
the generated docs all read it — so there are no scattered `if (recipe == "...")`
sites to update.

Adding a backend is **one folder's worth of files plus three small appends**:

| You edit | What goes there |
|----------|-----------------|
| `CMakeLists.txt` → `LEMON_BACKENDS` | **one line**: `"<recipe>\|<stem>"` |
| `src/cpp/server/backends/<stem>_descriptor.cpp` + `.h` | the descriptor (plain data) |
| `src/cpp/server/backends/<stem>_factory.cpp` + `.h` | `create()` + the `WrappedServer` subclass |
| `src/cpp/resources/backend_versions.json` | version pin(s) — skip if there's no downloaded binary (e.g. cloud) |
| `src/cpp/resources/server_models.json` | the models |

No router edits, no CLI edits, no doc edits, no support-matrix edits.

## The descriptor (plain data — CLI-safe)

The descriptor is the single object every consumer reads. It links into **both**
the `lemonade` CLI and `lemond`, so it must not reference server classes.

`src/cpp/include/lemon/backends/<stem>_descriptor.h`:

```cpp
#pragma once
#include "lemon/backends/backend_descriptor.h"
namespace lemon { namespace backends {
extern const BackendDescriptor <stem>_descriptor;
} }
```

`src/cpp/server/backends/<stem>_descriptor.cpp`:

```cpp
#include "lemon/backends/<stem>_descriptor.h"
namespace lemon { namespace backends {
const BackendDescriptor <stem>_descriptor = {
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
} }
```

`SlotPolicy` controls accelerator sharing: `Standard` (counts toward LRU slots),
`ExclusiveNpu` (evicts all NPU servers first), `CoexistByType` (one per model
type), `Unmetered` (never counted, never auto-evicted — cloud).

## The factory + server class (server-only)

The factory builds the `WrappedServer` subclass. It is compiled into `lemond`
only (it references server classes), which keeps the `lemonade` CLI link clean.

`src/cpp/include/lemon/backends/<stem>_factory.h`:

```cpp
#pragma once
#include <memory>
#include "lemon/backends/backend_registry.h"
namespace lemon { namespace backends {
std::unique_ptr<WrappedServer> <stem>_create(const BackendContext& ctx);
} }
```

`src/cpp/server/backends/<stem>_factory.cpp`:

```cpp
#include "lemon/backends/<stem>_factory.h"
#include "lemon/backends/<stem>_server.h"
#include "lemon/wrapped_server.h"
namespace lemon { namespace backends {
std::unique_ptr<WrappedServer> <stem>_create(const BackendContext& ctx) {
    return std::make_unique<MyServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}
} }
```

The server class is a `WrappedServer` subclass. Implement `load()`, `unload()`,
and only the capability interfaces you actually serve (`ITranscriptionServer`,
`IImageServer`, `ITextToSpeechServer`, …). `WrappedServer` provides default
"unsupported" `chat_completion`/`completion`/`responses`, so a non-chat backend
does not stub them.

## Register it: one line

```cmake
set(LEMON_BACKENDS
    ...
    "myrecipe|myrecipe"   # "<recipe>|<stem>"
)
```

The `foreach` in `CMakeLists.txt` compiles your two sources and regenerates the
registry headers, binding the descriptor to its `create()`.

## What you get for free

- **Standard options:** `merge_args`, `auto_evict`, `evict_idle_timeout`,
  `downsize_idle_timeout`, `evict_weight_factor`, `pinned`. `ctx_size` is opt-in
  via `uses_ctx_size`.
- **Generated CLI flags** for every descriptor option with a `cli_flag`, plus
  `--<recipe>` when `selectable_backend = true`.
- **Install/download** via the backend's `BackendSpec` (binary + install params).
- **`/system-info`** `recipes` entry (display name, options schema, support matrix).
- **Generated docs** — your backend appears in
  [`backends-reference.md`](backends-reference.md) automatically.

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
`src/cpp/server/backends/moonshine_descriptor.cpp` and `moonshine_factory.cpp`.

> Note: collections (`collection.omni`) are orchestrator-driven, not
> `WrappedServer` subprocesses, and are the one explicit exception to this model.
