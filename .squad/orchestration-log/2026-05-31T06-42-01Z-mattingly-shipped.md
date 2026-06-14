# Orchestration Log: mattingly — Presets v1.4 Shipped
**Date:** 2026-05-31T06:42:01Z
**Agent:** mattingly (claude-sonnet-4.6)
**Session Topic:** Presets v1.4 (capability-keyed presets, staged bindings, sampling wire-through)
**Run:** 2/2 (shipping per Kyle's answers)

## Task
Ship Presets v1.4 UI changes in `prototype/ui-redesign/` per Kyle's responses to 7 open questions.

## Output
Decision file: `.squad/decisions/inbox/mattingly-presets-v1.4-shipped.md`
Files changed:
- `prototype/ui-redesign/src/presetStore.ts`
- `src/components/PresetManager.tsx`
- `src/api.ts`
- `src/styles/styles.css`
- `tests/features.spec.ts`

## Summary
- Implemented capability-keyed presets with `applies_to: Capability[]`
- Moved top-level `recipe` to optional `engine_hint` (advanced section)
- Staged-binding semantics: apply stores binding, "Will apply on next load" shown
- Sampling params (`temperature`, `top_p`, `top_k`, `repeat_penalty`) wired into `/api/v1/chat/completions` requests
- Import policy: v1.4 requires `applies_to`; legacy `recipe` schema rejected
- Build passes; 2 Playwright tests passing
