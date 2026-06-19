# Decision: Presets Redesign Audit — Key Recommendations

**Agent:** Mattingly (UI)  
**Date:** 2026-06-19  
**Branch:** `kpoin/ui-testing`  
**Source doc:** `prototype/ui-redesign/docs/PRESETS_REDESIGN.md`  
**Status:** Pending review by Kyle / Kranz

---

## Summary

Mattingly completed a full design audit of the Presets UI on 2026-06-19, including:
- 12 Playwright screenshots at desktop (1440×900) and mobile (390×844)
- Code-level root cause analysis of the "can't edit starters" gap
- Recipe↔preset integration gap analysis
- Three-phase recommendation roadmap

---

## Key Decisions Needed

### D1 — Recipe preference field rename (Phase B gate)

**Proposal:** Rename `engine_hint` to `recipe_preference` in `Preset` interface (backward-compat).  
**Impact:** Signals intent change from passive hint → active preference. Drives new apply-time recipe selection UX.  
**Decision needed:** Approve rename and backward-compat migration strategy.

### D2 — Starter editability model (Phase A)

**Two options:**
- **Option A (recommended):** Duplicate-to-customize. Starter cards get a "Customize →" button that clones + opens edit. Starters remain read-only. Simplest.
- **Option B:** Unlock-and-edit. Add an "Unlock" flow that moves a starter into user presets for direct editing. More complex; requires UX to handle "what if you want the original back."

**Decision needed:** Kyle confirms Option A or B.

### D3 — AutoOpt rail visibility

**Proposal:** AutoOpt summary collapses by default on mobile; AutoOpt rail collapses on initial load (not just toggle).  
**Decision needed:** Does Kyle want AutoOpt visible by default for power users, or collapsed by default for new users?

---

## Committed Artifacts

| Artifact | Path |
|----------|------|
| Full audit + recommendations | `prototype/ui-redesign/docs/PRESETS_REDESIGN.md` |
| Screenshot script (reusable) | `prototype/ui-redesign/scripts/screenshot-presets.mjs` |
| Desktop grid screenshot | `docs/screenshots/presets/01-presets-grid-desktop.png` |
| Mobile grid screenshot | `docs/screenshots/presets/02-presets-grid-mobile.png` |
| Starter readonly evidence | `docs/screenshots/presets/04-starter-slideover-readonly.png` |
| New preset form | `docs/screenshots/presets/05-custom-preset-create.png` |
| _(+ 7 more screenshots)_ | `docs/screenshots/presets/` |

---

## Top Findings (for Scribe)

1. **Starters are permanently read-only** (`isReadOnly = preset.starter`, `PresetManager.tsx:749`). The only affordance is "Clone" at the bottom of a scrollable panel — no on-card CTA. Kyle's pain is 100% real and code-confirmed.

2. **Recipe/engine preference is invisible and passive.** `engine_hint` exists in all starters but never appears on cards, lives in a collapsed "Advanced" section, and has no effect on which backend actually loads.

3. **Mobile: AutoOpt block dominates the first viewport.** The full CLI args string takes ~40% of first-paint at 390px. Starters are below it.

## Top Recommendations (for Scribe)

1. **Phase A (≤1 day): Add "Customize →" button to starter card face.** Calls `handleClone` (already exists) and opens the edit slideover immediately. No data model change.

2. **Phase B (~1 week): Promote `engine_hint` → `recipe_preference`, surface on cards, add recipe picker in slideover body, add mismatch dialog at apply-time.**

3. **Phase A (≤half day): Collapse AutoOpt summary by default on mobile.** Wrap in `<details>` closed by default; expose starters as the first content below the header.
