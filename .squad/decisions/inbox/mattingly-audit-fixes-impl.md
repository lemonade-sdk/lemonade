# Audit Fixes #1–4 — Implementation Summary

**Author:** Mattingly  
**Date:** 2026-06-13  
**Branch:** `kpoin/ui-audit-fixes` (cut from `kpoin/ui-testing` @ af66ea14)  
**Build:** ✅ Exit 0 (`npm run build` in `prototype/ui-redesign/`)

---

## Fix #1 (P0) — Guard the onToolCalls hang path

**File:** `prototype/ui-redesign/src/hooks/useChatStreaming.ts`  
**Lines changed:** ~186–283 (the `onToolCalls` callback body)

**Problem:** If any `runtime.execute()` call throws an uncaught exception, the outer `runCompletion` Promise never settles, leaving the stream stuck in "streaming" state with no way to abort.

**Approach:** Wrapped the entire `onToolCalls` callback body in a `try/catch`. On catch:

1. Iterates `allToolCalls`, marks any entry still at `status: 'running'` as `status: 'error'` with `Error: <msg>` in the result field.
2. Updates `activeStreams` to surface the errored tool calls in the UI.
3. Clears the token buffer for the conversation.
4. Calls `onError(convoId, 'Tool execution failed: <msg>')` — this surfaces an error banner to the user via the existing error path.
5. Calls `cleanup(convoId)` to remove the stream from `activeStreams`/`liveStats`.
6. Calls `resolve()` to settle the outer Promise.

**Key design note:** The inner `try { await runCompletion(); resolve(); } catch (err) { reject(err); }` (for the recursive tool-round call) does **not** re-throw from its catch block — `reject(err)` doesn't propagate as a thrown exception — so the outer catch is not triggered by that path. The outer catch only fires on unexpected throws from `Promise.all` / executor code.

---

## Fix #2 (P1) — Surface load errors in ModelManager

**Files:** `prototype/ui-redesign/src/components/ModelManager.tsx`, `prototype/ui-redesign/src/styles/styles.css`  
**Lines changed (tsx):** state declaration ~585, `handleLoad` ~851–863, `handlePullAndLoad.onComplete` ~953–960, `renderModelRow` ~1603–1607  
**Lines changed (css):** inserted `.row__load-error` rule at ~1927

**Problem:** `handleLoad` and `handlePullAndLoad` caught errors with `console.error` only — no user-facing feedback.

**Approach:**

- Added `loadError: { modelName: string; message: string } | null` state.
- Both catch blocks now set `loadError` using `friendlyErrorMessage(err)` (already imported) and schedule an auto-clear after 6 s via `window.setTimeout` — clear is conditional so it won't erase a newer error for a different model.
- `setLoadError(null)` at the start of `handleLoad` clears any stale error for the same or different model when retrying.
- `renderModelRow` renders a `.row__load-error` div below `row__content` when `loadError.modelName === name`.
- CSS uses `var(--danger)` / `var(--danger-soft)` / `border-top` with the same rgba as `hf-zone__empty--error` — visually consistent with existing error surfaces.

---

## Fix #3 (P1) — Wire active preset into image composer defaults

**File:** `prototype/ui-redesign/src/components/ChatView.tsx`  
**Lines changed:** `imageDefaultsForModel` function ~295–305, `defaultImageSettings` useMemo ~636–641

**Problem:** Applying an image preset (e.g. "Sharp": 30 steps, cfg 8.0) had no effect on the composer's `imageSettings` state — it stayed at the default 20 steps / 7.0 cfg.

**Approach:**

- Extended `imageDefaultsForModel` to accept a third optional parameter `activePresetRecipeOptions?: Record<string, unknown> | null`. This is spread last (after `loadedRecipeOptions`), making it the highest-priority source.
- Updated the `defaultImageSettings` useMemo to pass `currentPreset?.recipe_options` when `currentCapability === 'image'`, and added `currentPreset` and `currentCapability` as dependencies.
- The existing useEffect (line ~652) already guards the `setImageSettings(defaultImageSettings)` call with `!imageSettingsTouchedRef.current`, so user-edited values are never clobbered. The preset feeds through `defaultImageSettingsKey` → useEffect → `setImageSettings` only when the user hasn't manually changed the controls.

**Layering priority (lowest → highest):** `DEFAULT_IMAGE_SETTINGS` → `model.image_defaults` → `model.recipe_options` → `loadedModel.recipe_options` → `activePreset.recipe_options`.

---

## Fix #4 (P1) — Unify modeSupportsChatCompletions

**File:** `prototype/ui-redesign/src/components/ChatView.tsx`  
**Line changed:** ~731

**Before:**
```ts
const modeSupportsChatCompletions = currentLoadedModel
  ? canUseChatCompletions(currentLoadedModel)
  : (currentCapability === 'chat' || currentCapability === 'omni');
```

**After:**
```ts
const modeSupportsChatCompletions = currentCapability === 'chat' || currentCapability === 'omni';
```

**Rationale:** `currentCapability` is derived from `currentModelSnapshot`, which already prefers custom/known model info over raw `capabilityFromLoaded` — so it correctly resolves `'chat'` for custom models that have no `recipe` field (those would return `'unknown'` from `canUseChatCompletions`). The old guard was the root cause of the tools toggle being silently disabled for custom models. `canUseChatCompletions` is not removed — it remains in `modelCapabilities.ts` for any future callers.
