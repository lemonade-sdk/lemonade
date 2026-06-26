# Update preset while loaded — design & `window.api` contract (#2356)

**Status:** UI POC on `feat/gui3-update-preset-while-loaded` (PR #2429).
**Revised** per maintainer feedback (@fl0rianr) to drop the dedicated
`update-preset` endpoint and the client-provided `mode` parameter. `lemond`
remains off-limits to this POC.

The previous revision of this doc proposed a `POST /api/v{0,1}/update-preset`
endpoint that took a client-classified `mode: 'live' | 'reload'`. That put
runtime-capability responsibility on the UI and required a new backend endpoint.
It is **withdrawn**. The simplified design below uses the existing request and
load/unload paths only.

## Guiding principle (from review)

> The UI should not be the source of truth for runtime capability.

Preset changes fall into two kinds, and each already has a home in the existing
architecture — no new endpoint and no client `mode` flag are needed:

- **Request-time fields** (`system_prompt`, `sampling`/temperature, `tools`):
  applied by **request composition** in the frontend on the next generation
  request. No model runtime state changes; no reload.
- **Load-time fields** (`ctx_size`, `backend`, `device`, model args via
  `recipe_options`): require a real reload. The client performs an
  **unload + load** (exactly as `main` does today). A named `reloadModel`
  helper may wrap this so a future in-place backend reload can drop in, but for
  now it is literally unload->load.

## Active-preset binding (UI state)

The UI stores a per-model **active-preset binding** (`linkedPresetId`) in
client-local storage (invariant #11 — never in `lemond`). This already exists:

- `activePresetForModel(modelName)` resolves the linked preset.
- Request composition already reads it:
  - sampling — `api.ts` spreads `samplingForModel(model)` into chat bodies.
  - system prompt — `ChatView.tsx` pushes `systemPromptTextForPreset(currentPreset)`.

So for request-time changes, **rebinding the active preset is the whole
operation** — the next request automatically carries the new values. There is
nothing to POST.

## Live vs reload classification

`classifyPresetChange(running, next)` in `src/presetStore.ts` returns
`'none' | 'live' | 'reload'`:

| Field group      | Fields                                                                                  | Kind     | Why |
|------------------|-----------------------------------------------------------------------------------------|----------|-----|
| Reload-requiring | `recipe_options` (ctx_size, backend, device, args, steps, cfg, ...), `engine_hint`      | `reload` | Bound at init; needs reinitialization. |
| Live-updateable  | `sampling`, `system_prompt_id`, `system_prompts`, `tools_enabled`                       | `live`   | Applied per request at generation time. |
| Everything else  | name, description, applies_to, id, ...                                                   | `none`   | No effect on a running model. |

First reload-requiring diff wins (-> `reload`); else any live diff -> `live`;
else `none`.

### Correctness fix (same-ID edits)

The current implementation short-circuits on identical ids:

```ts
if (running.id === next.id) return 'none';   // presetStore.ts:570 — BUG
```

This means **editing a preset in place** (same id, but changed `temperature`,
`system_prompt`, or `ctx_size`) classifies as `none`, so the model keeps running
stale values and no Apply/Reload affordance appears. **Remove that early
`return 'none'`** and let the `RELOAD_FIELDS` / `LIVE_FIELDS` comparisons run
regardless of whether the id matches. Only return `none` when no field in either
group actually differs.

## UI affordance

When the active preset differs from the running preset (now also true for
same-id edits after the fix), the detail panel shows a button next to **Unload**:

- **live-only changes** -> label **"Apply preset"**.
  Action: rebind the active preset; request composition handles the rest. The
  prior "POST live" call is removed.
- **load-time changes** -> label **"Reload to apply preset"**, and the loaded
  model is marked **"needs reload"**.
  Action: `reloadModel(modelName, recipeOptions)` = `api.unloadModel()` then
  `api.loadModel(modelName, recipeOptions, modelInfo)`.

a11y: the button's accessible name conveys which path it takes ("Apply preset
for {name}" vs "Reload {name} to apply preset"); status uses the existing polite
live region; focus management preserved. New/changed controls get aria-labels
and Playwright coverage (next ids **A164+**). Existing A154–A163 must be updated
to the new (no-`mode`, no-endpoint) workflow so the suite stays green.

## `window.api` surface

No `updatePreset(...)` and no `mode` parameter. The POC uses only existing
methods:

```ts
api.loadModel(modelName, recipeOptions?, modelInfo?): Promise<unknown>;
api.unloadModel(modelName?): Promise<unknown>;
```

A thin client helper expresses the load-time path (future-proofing only — it is
unload->load today):

```ts
// src/api.ts (or a small wrapper)
async reloadModel(
  modelName: string,
  recipeOptions?: Record<string, unknown>,
  modelInfo?: unknown,
): Promise<unknown> {
  await this.unloadModel(modelName);
  return this.loadModel(modelName, recipeOptions, modelInfo);
}
```

If a real in-place backend reload ever lands, only this helper's body changes;
callers and tests stay the same. The old `api.updatePreset(...)` method and its
`/api/v1/update-preset` POST are removed.

## Lemonade tools (`src/tools/lemonadeTools.ts`)

Add a **`change_preset`** tool that drives the same preset-update workflow
directly, so chat-/agent-triggered preset changes behave identically to the UI
button:

- Inputs: `model_name` (optional -> most-recently-used loaded model),
  `preset` / `preset_id` / `preset_name` (reuse the existing
  `presetSpecifier` / `resolvePreset` helpers already used by `load_model`).
- Steps:
  1. Resolve model + preset; reject incompatible presets via `isCompatible`
     (same error shape as `load_model`).
  2. Rebind the active preset (`applyPresetBinding`).
  3. `classifyPresetChange(running, next)`:
     - `live` -> no further action; report "applied live".
     - `reload` -> `reloadModel(...)` (unload->load); report "reloaded".
     - `none` -> report no-op.
- Return the usual `toolPayload` summary with an `answer_instruction`.

This replaces the deferred "tool calls the update-preset endpoint" plan: the
tool now executes the workflow with the same primitives as the UI.

## What is NOT in this POC

- No `lemond` changes, no new HTTP endpoint, no server-side preset registry.
- No client `mode` flag is sent anywhere.
- Presets remain 100% client-local (invariant #11).

## Open questions for @fl0rianr

1. Should the active-preset binding **persist across an unload/reload** (the UI
   re-binds before reloading, so the reloaded model runs the new preset), and is
   that the behaviour you want for the tool path too?
2. Request-time **tool** changes: confirm tools belong on the request-composition
   side for every backend (we compose them with sampling + system prompt), with
   no reload — i.e. nothing tool-related is load-time.
3. If a future backend gains real in-place reload, do you want it surfaced as a
   distinct capability flag, or is the silent `reloadModel` swap (unload->load ->
   in-place) acceptable since the contract is identical to callers?
