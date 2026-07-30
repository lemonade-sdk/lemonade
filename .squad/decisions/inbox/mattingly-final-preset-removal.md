# Final Frontend Preset Removal

Date: 2026-07-30T12:06:47.516-06:00

## Compatibility retained

- Direct browser-local model records remain under `model_tunings`; backend records remain under `backend_tunings`.
- The isolated migration in `src/app/src/modelConfiguration.ts` recognizes only the legacy neutral suffixes `@@default`, `@@s_default`, and `@@s__default`.
- Each recognized legacy record is promoted to the model-name key without data transformation. If a direct record already exists with different data, the legacy record is preserved in `storage_migration_conflicts_v1` rather than overwritten. A migration marker makes the operation idempotent.
- No active preset state, starter data, preset storage key, or preset migration is retained.

## Removed artifacts

- `src/app/src/presetStore.ts`
- `src/app/src/presetPrompts.ts`
- `src/app/src/components/PresetManager.tsx`
- `src/app/tests/preset-intent.runtime.cjs`
- `src/app/scripts/screenshot-presets.mjs`
- `src/app/docs/PRESETS_REDESIGN.md`
- `src/app/docs/UPDATE_PRESET_CONTRACT.md`
- `src/app/docs/screenshots/presets/`
- Obsolete `lemonade_user_presets` and `lemonade_applied_presets` storage migrations.
- Preset navigation, icons, styles, feature flags, starter data, dead helpers, and related test assertions.

## Protected scope

`src/app/src/components/McpPanel.tsx`, MCP documentation, server code, packaging, Apps, Logs/Telemetry, and unrelated dirty worktree changes were not altered as part of this pass. Remaining preset wording is limited to the explicitly protected MCP panel and MCP documents.

## Validation

- `npm run typecheck` passed from `src/app`.
- Storage migration runtime test passed.
- MCP runtime contract test passed.
- Global model settings runtime test passed.
- Icon library contract test passed.
- Model-configuration Playwright coverage passed: 3 tests.
- Accessibility suite passed: 140 tests.
- Focused source search found no frontend references outside the protected MCP files and documents.
