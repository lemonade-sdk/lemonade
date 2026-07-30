# Mattingly History

## 2026-07-30T12:06:47.516-06:00

- Completed the final frontend removal of the failed Presets feature without changing server, packaging, or MCP panel behavior.
- Direct model configuration now lives in `src/app/src/modelConfiguration.ts`, with browser-local `model_tunings` and `backend_tunings` storage.
- The only retained compatibility path promotes legacy `modelName@@default`, `modelName@@s_default`, and `modelName@@s__default` records to neutral model keys; it is idempotent and archives conflicts without overwriting records.
- Verified typechecking, storage migration, MCP runtime, global model settings, icon contract, model-configuration, and accessibility coverage.
