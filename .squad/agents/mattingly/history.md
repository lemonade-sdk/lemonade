# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** UI / Frontend — Tauri desktop app + web app

## Core Context

Leading UI POC on `feat/ui-testing`. React stays (ROI analysis showed ~300 LOC savings for 47-file rewrite = inverted ROI). Four explicit framework-change conditions set by Lovell; do not re-litigate without them.

**Critical constraint:** Debian native packaging requires `src/web-app/` to use only `/usr/share/nodejs` modules. Kranz is source of truth.

**Detailed history (2026-05-15 to 2026-05-24):** framework evaluation, ROI re-exam, UI/UX competitive review, static prototype v1.0–v1.1, API wiring to live lemond, v1.3 presets audit + shipped. See `history-archive.md`.

## Active Learnings

### 2026-05-31: Prototype README rewritten

Updated `prototype/ui-redesign/README.md` to reflect the actual React 19 + TypeScript + webpack + Playwright stack (not the old v1.1 static HTML demo). Added sections covering prerequisites, install, dev/build/test scripts, Presets v1.4 features, project structure, real-server pointing, and troubleshooting. README now accurately describes the active POC on branch `feat/ui-testing` and links to `.squad/decisions.md` for design rationale.

### 2026-05-16–2026-05-31: Presets journey — v1.3 capability-keying → v1.4 shipped

v1.3 renamed Recipes→Presets, rekeyed from engine-list to capability-list: `preset.applies_to: [chat, image, …]`. Replaced `MODEL_ENGINES` with `MODEL_LABELS` (array of capability labels per model). Compatibility is label-intersection: `preset.applies_to.some(c => model.labels.includes(c))`. Added image presets (Sharp, Quick). 8 starters total (6 chat + 2 image). Backend hint field kept for power users (biases backend choice; Router picks final backend).

Discovered React port `PresetManager.tsx` drifted back toward backend recipes. Audited against v1.3 and identified 8 UX problems: technical terminology, missing compatibility guards, only loaded models first-class, sampling not applied.

**v1.4 shipped per Kyle's answers to 7 open questions:**
- Schema: `applies_to: Capability[]`, optional `engine_hint` (advanced), `recipe_options` for load flags
- Staged bindings: apply stores local binding, shows "Will apply on next load" (does NOT call `api.loadModel()` immediately)
- Sampling wired: `temperature`, `top_p`, `top_k`, `repeat_penalty` merged into `/api/v1/chat/completions`
- Import policy: v1.4 requires `applies_to`; legacy rejected
- Files: `presetStore.ts`, `PresetManager.tsx`, `api.ts`, `styles.css`, `tests/features.spec.ts`
- Build passes; 2 Playwright tests passing

See `.squad/orchestration-log/` for agent run summaries. See `.squad/decisions/decisions.md` for full decision trail.

**Older learnings (2026-05-15 to 2026-05-24)** archived in `history-archive.md`: framework evaluation, ROI analysis, UI/UX competitive review, static prototype v1.0–v1.1 audit, API wiring, v1.3 presets, HuggingFace integration, UI perf fixes.

