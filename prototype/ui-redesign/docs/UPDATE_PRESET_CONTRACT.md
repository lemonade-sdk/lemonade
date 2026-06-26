# Update preset while loaded — `window.api` / HTTP contract (#2356)

**Status:** UI POC complete on `feat/gui3-update-preset-while-loaded`. Backend
pieces (the real reload + the Lemonade-tools wiring) are **DEFERRED to the
backend team** — `lemond` is off-limits to the UI POC.

This document is the proposed contract so the UI-triggered and tool-triggered
preset updates behave identically once the backend implements it.

## What the POC ships (UI only)

- An **"Update preset"** button that appears next to **Unload** in the model
  detail panel when a model is **loaded** AND a **different** preset is linked
  to it (linked ≠ running).
- A client-side **live-vs-reload classification** of the change.
- The correct UX for each path: live = inline "applied live, no reload" status;
  reload = an automatic "Reloading…" flow (no manual unload/load).
- A clean `api.updatePreset(...)` hook the UI calls.
- Screen-reader announcements (polite live region), keyboard operability, focus
  management, and Playwright/axe coverage (A154–A163).

## What is DEFERRED to the backend (lemond)

1. **Real reload mechanics.** For `mode='reload'`, lemond must reinitialize the
   model **in place** (perform the unload+load itself); the client must never
   issue a manual unload/load pair.
2. **Live apply.** For `mode='live'`, lemond must apply request-time fields
   (system prompt, sampling) to the running process without reinitializing.
3. **Lemonade-tools parity.** The same update logic must back a tool such as
   "change the preset", so tool-triggered and UI-triggered updates are
   consistent. The tool should call the same code path / endpoint below.

## Proposed HTTP endpoint

Per project invariant #1, register under all four prefixes:
`/api/v0/update-preset`, `/api/v1/update-preset`, `/v0/update-preset`, `/v1/update-preset`.

```
POST /api/v1/update-preset
Content-Type: application/json

{
  "model_name": "Llama-3.1-8B",     // the loaded model to update
  "preset_id":  "p-live",            // client-local preset id (opaque to lemond)
  "mode":       "live" | "reload",   // UI's classification of the change
  "recipe_options": { ... },          // load-time options (sent when mode="reload")
  "sampling":       { ... },          // request-time generation params (mode="live")
  "system_prompt":  "..." | null      // resolved system prompt text (mode="live")
}
```

**Behaviour**

| `mode`   | lemond action                                                                 | Response |
|----------|-------------------------------------------------------------------------------|----------|
| `live`   | Apply request-time fields to the running process. **No** reinitialization.     | `200` once applied |
| `reload` | Reinitialize the model in place (lemond does the unload+load). No client unload/load. | `200` once the model is ready again |

The endpoint is idempotent for an unchanged preset (returns `200` no-op).

## `window.api` / client method

```ts
api.updatePreset(
  modelName: string,
  presetId:  string,
  mode: 'live' | 'reload',
  payload?: {
    recipe_options?: Record<string, unknown>; // for mode='reload'
    sampling?: Record<string, unknown>;        // for mode='live'
    system_prompt?: string | null;             // for mode='live'
  },
): Promise<unknown>;
```

Defined in `src/api.ts`. In the POC it POSTs to `/api/v1/update-preset`; the
Playwright suite mocks that endpoint.

## Live-vs-reload field classification

Implemented in `src/presetStore.ts` as `classifyPresetChange(running, next)`,
returning `'none' | 'live' | 'reload'`.

| Field group        | Fields                                                              | Kind     | Rationale |
|--------------------|--------------------------------------------------------------------|----------|-----------|
| Reload-requiring   | `recipe_options` (ctx_size, backend, device, args, steps, cfg, …), `engine_hint` | `reload` | The runtime binds these at init; changing them needs reinitialization. |
| Live-updateable    | `sampling` (temperature/top_p/top_k/repeat_penalty), `system_prompt_id`, `system_prompts`, `tools_enabled` | `live` | Applied per request at generation time. |
| (everything else)  | name, description, applies_to, id, …                               | `none`   | No effect on a running model. |

The first reload-requiring difference wins (→ `reload`); otherwise any
live-updateable difference → `live`; otherwise `none`.

## Open questions for @fl0rianr

1. Confirm the **endpoint shape** (a dedicated `update-preset` endpoint vs.
   overloading `load` with an `update_preset` flag).
2. Confirm the **live-vs-reload field split** above — in particular whether any
   `recipe_options` sub-fields are in fact live-applyable, and whether sampling
   is truly request-time for every backend.
3. Confirm whether `preset_id` should be opaque to lemond (UI resolves all
   fields) or whether lemond should resolve presets from a shared registry.
