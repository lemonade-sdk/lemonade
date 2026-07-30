# Swigert Final Preset Storage Cleanup

Date: 2026-07-30T12:06:47.516-06:00
Reviewer: Lovell
Reviser: Swigert

## Outcome

- Removed the two obsolete raw preset keys from the neutral legacy-copy map.
- Added an idempotent, deletion-only cleanup for all six historical preset keys before the migration marker short-circuit.
- Kept `clearClientStorage()` able to remove both raw and namespaced historical keys.
- Preserved the neutral direct model-tuning store and its lossless `Default`/default-sentinel migration path; no model-tuning migration code was changed.

## Unavoidable compatibility exception

The six historical key names remain as literals only in the deletion allowlist and focused cleanup assertions. They are never read, copied, merged, returned, or exposed as active preset data; no preset collections, flags, starter data, or user-visible preset semantics remain.

## Validation

- `npm --prefix src\app run test:storage` passed, including first-run cleanup and rerun cleanup with the migration marker already set.
- `npm --prefix src\app run test:global-model-settings` passed.
- `npm --prefix src\app run typecheck` passed.
- Completion search found no active preset storage consumers; production references to the six historical names are limited to the deletion-only cleanup in `storage.ts`.
