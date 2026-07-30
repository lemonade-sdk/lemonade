# Final model configuration migration repair

Date: 2026-07-30T12:06:47.516-06:00
Owner: Kranz

## Decision

The neutral model tuning migration now groups recognized historical persisted-key suffixes by model. Suffix matching is case-insensitive, and the deterministic precedence for a model without a direct record is `@@default`, then `@@s_default`, then `@@s__default`. A direct model key always remains authoritative.

Every differing non-winning record is retained in `lemonade:storage_migration_conflicts_v1` under the established encoded model/source archive key. If an archive key already contains a different value, a stable numeric disambiguator is used rather than overwriting it. The migration writes the archive and canonical tuning map before deleting any historical keys, and uses the neutral `model_tunings_migrated_v3` marker for idempotent reruns.

## Validation

- `node tests/model-configuration-migration.runtime.cjs`
- `node tests/storage.runtime.cjs`
- `npm run typecheck -- --pretty false`

All three checks passed. The focused migration probe covers all three suffix forms, mixed-case `Default`, direct-key precedence, collision archival, historical-key cleanup, and rerun stability.
