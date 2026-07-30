# Lovell Final Preset Removal Review

Date: 2026-07-30T12:30:04.678-06:00
Reviewer: Lovell
Requested by: Kyle Poineal
Original author: Mattingly

## Review outcome

The frontend removal is substantially complete:

- No active frontend imports or definitions remain for `presetStore`, `presetPrompts`, `PresetManager`, `change_preset`, preset starter data, or preset storage constants.
- `RecipeOptions`, `SamplingParams`, model tuning, backend tuning, and related APIs are isolated in `src\app\src\modelConfiguration.ts`; active imports were updated.
- Model Configuration UI coverage passed, and the GUI Chat Tools/MCP picker runtime and accessibility/focus coverage passed.
- Direct model tuning migration of `@@default`, `@@s_default`, and `@@s__default` records was verified as lossless and idempotent, including a case-insensitive `Default` sentinel.
- Completion search found only protected MCP wording:
  - `src\app\src\components\McpPanel.tsx:332,404`
  - `src\app\docs\CLIENT_MCP.md:5,9,22,52`
  - `src\app\docs\MCP_AUTH_RECOVERY.md:7,38,39`
  These files were not attributed to this task.

## Blocking defect

`src\app\src\storage.ts` leaves historical preset data and keys in browser storage when normal migration runs. The migration sets `lemonade_storage_migrated_v2` but does not remove:

- `lemonade_user_presets`
- `lemonade_applied_presets`
- `lemonade:user_presets`
- `lemonade:applied_presets`
- `lemonade:backend_presets`
- `lemonade:running_presets`

An isolated runtime probe confirmed all six keys remain after `storageKey('probe')` runs. This violates the requirement that backward compatibility be limited to neutral model-tuning migration and that active/user-visible preset state, data, and keys not be retained. The migration must remove these historical keys idempotently, with focused coverage for the cleanup and rerun behavior.

Rejected artifact: `src\app\src\storage.ts` (Mattingly, original author). Eligible reviser: Swigert.

## Task-attributed changed/deleted files

Modified:

- `src\app\ACCESSIBILITY.md`
- `src\app\DESIGN.md`
- `src\app\README.md`
- `src\app\package.json`
- `src\app\src\api.ts`
- `src\app\src\components\BackendManager.tsx`
- `src\app\src\components\ChatView.tsx`
- `src\app\src\components\ConnectView.tsx`
- `src\app\src\components\EffectiveSettingsModal.tsx`
- `src\app\src\components\Icon.tsx`
- `src\app\src\components\ModelDetailPanel.tsx`
- `src\app\src\components\ModelManager.tsx`
- `src\app\src\components\localIcons.tsx`
- `src\app\src\features\audio\ttsSettings.ts`
- `src\app\src\storage.ts`
- `src\app\src\styles\styles.css`
- `src\app\src\tools\lemonadeTools.ts`
- `src\app\src\tools\mcpRuntime.ts`
- `src\app\tests\a11y.spec.ts`
- `src\app\tests\features.spec.ts`
- `src\app\tests\global-model-settings.runtime.cjs`

Added:

- `src\app\src\modelConfiguration.ts`

Deleted:

- `src\app\docs\PRESETS_REDESIGN.md`
- `src\app\docs\UPDATE_PRESET_CONTRACT.md`
- `src\app\scripts\screenshot-presets.mjs`
- `src\app\src\components\PresetManager.tsx`
- `src\app\src\presetPrompts.ts`
- `src\app\src\presetStore.ts`
- `src\app\tests\preset-intent.runtime.cjs`
- `src\app\docs\screenshots\presets\00-initial-load-desktop.png`
- `src\app\docs\screenshots\presets\01-presets-grid-desktop.png`
- `src\app\docs\screenshots\presets\02-presets-grid-mobile.png`
- `src\app\docs\screenshots\presets\03-starter-card-hover.png`
- `src\app\docs\screenshots\presets\04-starter-edit-attempt.png`
- `src\app\docs\screenshots\presets\04-starter-slideover-readonly.png`
- `src\app\docs\screenshots\presets\05-custom-preset-create.png`
- `src\app\docs\screenshots\presets\05-default-preset-slideover.png`
- `src\app\docs\screenshots\presets\06-starter-cards-all-desktop.png`
- `src\app\docs\screenshots\presets\08-models-page-recipe-badges.png`
- `src\app\docs\screenshots\presets\09-starter-slideover-mobile.png`
- `src\app\docs\screenshots\presets\10-presets-grid-mobile-after-close.png`

## Explicitly excluded dirty paths

Concurrent unrelated changes were not attributed to Mattingly and were not requested for revert: `src\app\src\components\McpPanel.tsx`, `src\app\src\components\AppsView.tsx`, `src\app\src\components\LogViewer.tsx`, `src\app\src\features\chatHistory\historySettings.ts`, and the dirty `src\cpp` paths.

## Commands run

- `npm run typecheck` — passed.
- `npm run test:storage` — passed.
- `npm run test:global-model-settings` — passed.
- `npm run test:mcp-runtime` — passed.
- `npm run test:icons` — passed.
- `npx playwright test tests/features.spec.ts --grep "Configuration tab"` — 3 passed.
- `npm run test:a11y` — passed.
- Frontend completion searches for `preset`, `Preset`, `change_preset`, removed store symbols, and old preset storage constants — no active references outside the protected MCP wording listed above.
- Neutral model-tuning migration probe — passed lossless/idempotent/default-sentinel checks.
- Historical preset-key cleanup probe — failed; all six historical keys remained.

VERDICT: REJECTED
