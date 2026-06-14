# Session — 2026-05-16T14:30:00Z — Presets v1.3 (rename + capability rekey)

**User:** Kyle Poineal
**Branch:** `feat/ui-testing`

## Flow

1. **Explore** mapped the recipe documentation landscape and deep-dove on OmniRouter. Surfaced the three-way collision around "recipe" (engine id, Collection meta-recipe, user-facing preset) and confirmed OmniRouter routes by capability label, not by backend.
2. **Coordinator** synthesized four considerations and recommended renaming Recipes → Presets with capability-keyed compatibility.
3. **Kyle decided:** *"Presets is fine. capability keyed please."* — narrow approval. Browse-community and OmniRouter UI onboarding explicitly deferred.
4. **Mattingly** built v1.3 in the prototype: UI rename (CSS classes preserved), `applies_to: [capability labels]`, 8 starters (6 chat + 2 image), capability-conditional slide-over (Steps + CFG for image presets), `.cap-chip` visual with colored dots mirroring the existing `cap-badge` palette, expanded mock models across chat/image/ASR/TTS/embedding/reranking/vision.
5. **Open call** surfaced: whether "Backend hint" stays on chat presets. Mattingly kept it; coordinator leans toward dropping. Deferred to next session.

## Terminology landed

- **Preset** — user-facing word (UI everywhere)
- **Engine / Backend** — codebase concept (when the UI needs to refer to it, e.g., "Backend hint")
- **Recipe** — reserved for the two existing codebase meanings (engine id; Collection meta-recipe)

## Artifacts

- Decision: `.squad/decisions.md` — entry `2026-05-16T14:30:00Z: UI prototype v1.3 — Recipes → Presets, capability-keyed`
- Orchestration log: `.squad/orchestration-log/2026-05-16T1430Z-presets-v1-3.md`
- Prototype changes: `prototype/ui-redesign/{index.html, app.js, styles.css}` (tokens.css unchanged)
