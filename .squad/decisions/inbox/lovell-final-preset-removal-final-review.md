# Lovell Final Preset Removal Review

Date: 2026-07-30T12:58:01.624-06:00
Reviewer: Lovell
Requested by: Kyle Poineal
Verdict: APPROVED

## Review outcome

The final Presets removal is approved. Preset UI, navigation, tabs, types, flags, starter data, icons, docs, tests, and inactive source are removed. Model-configuration APIs and types live in `src/app/src/modelConfiguration.ts`, outside the deleted `presetStore.ts`. No active frontend references to `preset`, `Preset`, or `change_preset` remain outside the narrowly justified compatibility paths below.

The neutral migration recognizes `@@default`, `@@s_default`, and `@@s__default` case-insensitively, uses deterministic precedence, preserves direct model records, archives every differing discarded value with collision-safe keys, deletes legacy entries only after durable writes, and is idempotent. Model Configuration and GUI Chat Tools/MCP behavior remain covered and unchanged by this removal.

## Validation

- `npm run typecheck` — PASS
- `npm run test:storage` — PASS
- `node tests/model-configuration-migration.runtime.cjs` — PASS
- `npm run test:global-model-settings` — PASS
- `npm run test:mcp-runtime` — PASS
- `npx playwright test tests/features.spec.ts --grep "Configuration tab"` — PASS (3 tests)
- `npm run test:a11y` — PASS
- Focused frontend `git diff --check` — PASS

## Compatibility exceptions

- The six historical preset storage names remain only in the deletion-only allowlist and focused cleanup assertions: `lemonade_user_presets`, `lemonade_applied_presets`, `lemonade:user_presets`, `lemonade:applied_presets`, `lemonade:backend_presets`, and `lemonade:running_presets`.
- The neutral model-tuning migration retains its legacy suffixes, migration marker, and conflict archive key.
- Protected MCP panel and MCP documents retain pre-existing preset wording; those paths are outside this removal review and were not attributed to this artifact.

## Attributed modified files

- `src/app/ACCESSIBILITY.md`
- `src/app/DESIGN.md`
- `src/app/README.md`
- `src/app/package.json`
- `src/app/src/api.ts`
- `src/app/src/components/BackendManager.tsx`
- `src/app/src/components/ChatView.tsx`
- `src/app/src/components/ConnectView.tsx`
- `src/app/src/components/EffectiveSettingsModal.tsx`
- `src/app/src/components/Icon.tsx`
- `src/app/src/components/ModelDetailPanel.tsx`
- `src/app/src/components/ModelManager.tsx`
- `src/app/src/components/localIcons.tsx`
- `src/app/src/features/audio/ttsSettings.ts`
- `src/app/src/storage.ts`
- `src/app/src/styles/styles.css`
- `src/app/src/tools/lemonadeTools.ts`
- `src/app/src/tools/mcpRuntime.ts`
- `src/app/tests/a11y.spec.ts`
- `src/app/tests/features.spec.ts`
- `src/app/tests/global-model-settings.runtime.cjs`
- `src/app/tests/storage.runtime.cjs`

## Attributed added files

- `src/app/src/modelConfiguration.ts`
- `src/app/tests/model-configuration-migration.runtime.cjs`

## Attributed deleted files

- `src/app/docs/PRESETS_REDESIGN.md`
- `src/app/docs/UPDATE_PRESET_CONTRACT.md`
- `src/app/scripts/screenshot-presets.mjs`
- `src/app/src/components/PresetManager.tsx`
- `src/app/src/presetPrompts.ts`
- `src/app/src/presetStore.ts`
- `src/app/tests/preset-intent.runtime.cjs`
- `src/app/docs/screenshots/presets/00-initial-load-desktop.png`
- `src/app/docs/screenshots/presets/01-presets-grid-desktop.png`
- `src/app/docs/screenshots/presets/02-presets-grid-mobile.png`
- `src/app/docs/screenshots/presets/03-starter-card-hover.png`
- `src/app/docs/screenshots/presets/04-starter-edit-attempt.png`
- `src/app/docs/screenshots/presets/04-starter-slideover-readonly.png`
- `src/app/docs/screenshots/presets/05-custom-preset-create.png`
- `src/app/docs/screenshots/presets/05-default-preset-slideover.png`
- `src/app/docs/screenshots/presets/06-starter-cards-all-desktop.png`
- `src/app/docs/screenshots/presets/08-models-page-recipe-badges.png`
- `src/app/docs/screenshots/presets/09-starter-slideover-mobile.png`
- `src/app/docs/screenshots/presets/10-presets-grid-mobile-after-close.png`

## Excluded prohibited surfaces

No Presets-removal changes were attributed to `src/app/src/components/McpPanel.tsx`, `src/app/src/components/AppsView.tsx`, `src/app/src/components/LogViewer.tsx`, `src/app/src/features/chatHistory/historySettings.ts`, or any `src/cpp` path. Concurrent dirty changes on those surfaces remain outside this approval.

No commit was created.
