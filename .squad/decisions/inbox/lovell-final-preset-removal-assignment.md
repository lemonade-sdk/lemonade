# Lovell Final Preset Removal — Strict-Lockout Assignments

Date: 2026-07-30T12:51:04.120-06:00
Reviewer: Lovell
Requested by: Kyle Poineal
Verdict: REJECTED — two independent artifacts require reassignment.

## Lockout rule

Neither rejected artifact may be revised by its original author. The assignments below are independent and intentionally narrow. The previously prohibited surfaces remain prohibited: `src\app\src\components\McpPanel.tsx`, `src\app\src\components\AppsView.tsx`, `src\app\src\components\LogViewer.tsx`, `src\app\src\features\chatHistory\historySettings.ts`, all `src\cpp` paths, and unrelated dirty worktree changes.

## 1. Unused `.zone__starters` CSS

- **Artifact:** `src\app\src\styles\styles.css:12725-12728`, the unused `.zone__starters` rule left after deleting `PresetManager.tsx`.
- **Original author:** Mattingly owns the Phase 1 removal submission for lockout purposes. The surviving historical rule line predates that submission and is attributed by Git to Jeremy Fowers; neither provenance permits self-revision by Mattingly.
- **Correction:** Delete only the `.zone__starters` rule. Do not restore PresetManager, starter UI, preset state, or related styles.
- **Eligible revision owner:** **Swigert**, limited to this CSS cleanup and any directly focused stale-selector assertion. Swigert must not change model-tuning migration or storage behavior in this assignment.

## 2. Legacy multi-suffix model-tuning migration loss

- **Artifact:** `src\app\src\modelConfiguration.ts`, `migrateLegacyModelConfigurationStorage()`, where multiple `@@default`, `@@s_default`, or `@@s__default` records for one model overwrite `next[modelName]` before all legacy keys are deleted.
- **Original author:** Mattingly, as the Phase 1 model-configuration implementation.
- **Correction:** Group recognized suffix records by model, select a deterministic winner, preserve every discarded differing record in `storage_migration_conflicts_v1`, preserve a differing direct model record rather than overwrite it, remove legacy keys only after preservation succeeds, and keep reruns idempotent. Add focused runtime coverage for all three suffixes, case-insensitive `Default`, direct-key conflicts, collision archival, and rerun behavior.
- **Eligible revision owner:** **Kranz**, independent of Mattingly and Swigert, limited to `modelConfiguration.ts` and the focused model-tuning migration test/probe. Swigert authored the immediately prior storage-key cleanup and is explicitly not eligible for this migration correction; `storage.ts` cleanup is not to be reopened.

No other files or surfaces are authorized by this assignment. Lovell remains the final reviewer and will not edit code.
