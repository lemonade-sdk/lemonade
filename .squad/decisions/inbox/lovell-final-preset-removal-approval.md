# Lovell Final Preset Removal Approval

Date: 2026-07-30T12:06:47.516-06:00
Reviewer: Lovell
Requested by: Kyle Poineal

## Verdict

REJECTED

The focused frontend checks pass, and the six historical preset keys are now removed idempotently before the storage-migration marker short-circuit. The final tree is not yet eligible for approval because two loss/inactive-code defects remain.

## Blocking findings

1. `src\app\src\styles\styles.css:12725-12728` retains the unused `.zone__starters` rule. Its only historical consumer is the deleted `PresetManager.tsx`; no current source references it. This violates removal of inactive Presets starter UI code.

2. `src\app\src\modelConfiguration.ts` can lose data when more than one supported legacy suffix exists for the same model (`@@default`, `@@s_default`, or `@@s__default`). The loop overwrites `next[modelName]`, then deletes every legacy key. A lossless migration must retain a deterministic value and archive every discarded conflicting record, or otherwise preserve all values without silent loss.

## Lockout-safe correction assignment

Swigert owns the correction. Remove only the stale `.zone__starters` rule and revise the direct-tuning migration to handle same-model legacy collisions losslessly, with focused runtime coverage for all three suffixes, case-insensitive `Default`, direct-key conflicts, and idempotent rerun behavior. Do not modify `McpPanel.tsx`, `AppsView.tsx`, `LogViewer.tsx`, `features\chatHistory\historySettings.ts`, or any `src\cpp` path.

## Checks completed

Passed:

- `npm --prefix src\app run typecheck`
- `npm --prefix src\app run test:storage`
- `npm --prefix src\app run test:global-model-settings`
- `npm --prefix src\app run test:mcp-runtime`
- `npm --prefix src\app run test:icons`
- `npx playwright test tests/features.spec.ts --grep "Configuration tab"` — 3 passed
- `npm --prefix src\app run test:a11y` — passed
- Direct model-tuning probe — single-record, direct-conflict, case-insensitive, and idempotent checks passed; same-model collision probe exposed the loss above

## Source and UX audit

- No active Preset UI, component, type, flag, starter data, Preset icon, Preset test, or `change_preset` implementation remains.
- Model Configuration APIs and types are in `src\app\src\modelConfiguration.ts`, outside `presetStore.ts`.
- Configuration, README, Files tabs, and built-in Chat Tools/MCP runtime and accessibility coverage remain present.
- Remaining six historical key literals occur only in deletion-only cleanup and its focused assertions.
- Remaining MCP prose uses of “preset” are pre-existing protected wording in `McpPanel.tsx`, `docs\CLIENT_MCP.md`, and `docs\MCP_AUTH_RECOVERY.md`; these paths are not task-owned.

## Final task-attributed files

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
- `src\app\tests\storage.runtime.cjs`

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

Concurrent excluded paths remain outside this review: `src\app\src\components\AppsView.tsx`, `src\app\src\components\LogViewer.tsx`, `src\app\src\components\McpPanel.tsx`, `src\app\src\features\chatHistory\historySettings.ts`, and dirty `src\cpp` paths. Lovell made no code edits and no commit.

REJECTED
