# Presets Redesign — Audit & Recommendations

**Author:** Mattingly (UI agent)  
**Date:** 2026-06-19  
**Branch:** `kpoin/ui-testing`  
**Scope:** `prototype/ui-redesign/` — design audit + phased plan. No source code was modified.

---

## Executive Summary

**Top 3 Findings**

1. **Starter presets are read-only with no path to customization from the card.**  
   Kyle's exact pain — "I can't even figure out how to edit the default starters" — is 100% confirmed by the code (`const isReadOnly = preset.starter` in `SlideoverContent`, line 749) and by the screenshots (slideover footer shows only "Export" + "Clone", no "Save"; all inputs are `disabled`). The _only_ affordance is a `Clone` button at the bottom of a scrollable panel. On a first visit there's no obvious CTA on the card itself that says "clone to customize."

2. **Recipe/engine preference is invisible and passive.**  
   All 9 starters embed an `engine_hint` (`llamacpp`, `sd-cpp`) that biases backend selection — but this value never appears on cards, never appears prominently in the slideover, and lives in a collapsed `<details>` section labeled "Advanced engine options." A preset cannot _drive_ recipe selection when a model supports multiple recipes. The `engine_hint` is a whisper, not a driver.

3. **Mobile layout is single-column with a large AutoOpt block above the fold.**  
   On 390px, the AutoOpt summary card with raw CLI args (`--threads auto --batch-size 512…`) occupies ~40% of the first viewport. Starters are below a "Bundled Starters 10" heading, and only the Default card is visible before scroll. The AutoOpt rail collapses correctly, but the summary block stays visible inline. A new user has no idea what "AutoOpt #1 · balanced local baseline" means or why it dominates the screen.

---

**Top 3 Recommendations**

1. **Add "Customize" (duplicate-and-open) CTA directly on each starter card.**  
   One button on the card face — no need to open the slideover first. Clones the starter, opens the edit slideover on the clone immediately. Phase A, ≤ 1 day.

2. **Promote `engine_hint` to `recipe_preference` — make it first-class and visible.**  
   Show a recipe badge on every card. Add a recipe picker row in the main slideover body (not buried in Advanced). When applying a preset to a multi-recipe model, check `recipe_preference` and either auto-select or present a one-click confirmation. Phase B, ~3–4 days.

3. **Move AutoOpt summary below the fold on mobile; make it collapsible inline.**  
   On mobile the AutoOpt summary block should collapse by default (show name + toggle arrow). Starters should be the first thing visible. Phase A, ≤ half day.

---

## Part 1 — Current State Audit

### 1.1 Screenshots

All screenshots captured on 2026-06-19 against the live webpack-dev-server at `http://localhost:8080` using headless Chromium via Playwright. See `docs/screenshots/presets/`.

| File | Viewport | Description |
|------|----------|-------------|
| `01-presets-grid-desktop.png` | 1440×900 | Presets grid — all starter cards visible, AutoOpt rail open |
| `02-presets-grid-mobile.png` | 390×844 | Mobile view — AutoOpt summary dominates, only Default card visible |
| `03-starter-card-hover.png` | 1440×900 | Hover state on starter card |
| `04-starter-slideover-readonly.png` | 1440×900 | **Default starter opened — only Clone+Export in footer, all inputs disabled** |
| `04-starter-edit-attempt.png` | 1440×900 | Confirmation: no editable title input, no Save button |
| `05-default-preset-slideover.png` | 1440×900 | Default preset — "No preset overrides" empty state |
| `05-custom-preset-create.png` | 1440×900 | New custom preset form — engine hint buried in Advanced section |
| `06-starter-cards-all-desktop.png` | 1440×900 | Full grid — no recipe/engine badges visible on cards |
| `08-models-page-recipe-badges.png` | 1440×900 | Models page — recipe badges visible there, not carried back to presets |
| `09-starter-slideover-mobile.png` | 390×844 | Starter slideover at mobile width |
| `10-presets-grid-mobile-after-close.png` | 390×844 | Grid after slideover close |

### 1.2 Root Cause: Why Starters Cannot Be Edited

**`src/components/PresetManager.tsx`, line 749:**

```tsx
const isReadOnly = preset.starter;
```

This single boolean gates every interactive element in `SlideoverContent`. When `isReadOnly` is `true`:

- Line 899: Title renders as `<h2>` (no input), not `<input className="slideover__title-input">`
- Line 910: Description renders as `<p>`, not `<textarea>`
- Line 920: Capability buttons are `disabled={isReadOnly}`
- Lines 933, 969, 987–1007: All sliders/selects have `disabled={isReadOnly}`
- Lines 863, 868, 875: `updateSelectedSystemPrompt`, `addCustomSystemPrompt`, `deleteSelectedCustomPrompt` all guard with `if (isReadOnly) return`
- Line 812: `currentPreset` useMemo short-circuits to the unmodified `preset` object when `isReadOnly`
- **Line 1088**: Footer shows `<button …>Clone</button>` for starters instead of `<button …>Save</button>`

The script confirmed: `Save button visible: false, Title editable: false, Clone btn: true`.

**There is no "Edit" affordance at all on the card face.** The `PresetCard` component (lines 691–733) only shows one action button: for starters, this is `Clone`. There is no pencil icon, no tooltip that says "clone to edit," and no visual affordance on hover. A first-time user clicking a starter card opens a read-only panel and must scroll to the bottom to find `Clone`.

**`STARTERS` is a compiled constant** in `presetStore.ts` (lines 115–132). These objects have `starter: true` hardcoded and are not stored in `localStorage`. Even if you stripped `isReadOnly`, edits would vanish on page reload because `setUserPresets` only persists user presets. A proper "unlock and edit" flow requires cloning to `userPresets` first.

### 1.3 UX Problems Found

#### F1 — Edit-default-starter UX gap (HIGH)
**Root cause** confirmed above. Kyle's literal first request ("edit default starters") produces zero result. The only path is: open card → scroll to bottom of a long slideover → click Clone → get taken to a copy. No card-face CTA. No inline "clone to edit" on first view.

#### F2 — Engine hint is invisible and passive (HIGH)
`engine_hint` is present in all 9 STARTERS (all chat starters → `llamacpp`; image starters → `sd-cpp`) but:
- Not shown on preset cards
- In the slideover, it lives under a `<details>` collapse labeled "Advanced engine options" — closed by default
- It has no effect on recipe selection for the applied model; it merely "biases" the backend choice (per v1.4 spec)
- A user cannot build a preset that _requires_ `ryzenai-llm` vs just hinting at it

#### F3 — AutoOpt block dominates mobile viewport (HIGH)
The `autoopt-summary` block with full CLI args renders inline above the card grid. On 390px this is ~40% of the first viewport. AutoOpt is a power-user optimization feature; it should not outrank the preset list in the visual hierarchy on mobile.

#### F4 — "Your presets" empty state is below the fold on first visit (MEDIUM)
On desktop, the AutoOpt summary + "Bundled Starters 10" section + ~3 rows of cards push the "Your presets" zone off-screen. A new user may not realize they can create custom presets.

#### F5 — No recipe context on preset cards (MEDIUM)
Each card shows: capability chip, params, system prompt name, tools toggle. It does _not_ show which backend/recipe it targets. A user comparing "Balanced" vs "Thorough" sees only temp/ctx differences — they have no idea both assume `llamacpp`. This matters heavily for AMD NPU users comparing `flm` vs `ryzenai-llm`.

#### F6 — "Apply to a model" in slideover requires knowing model names (MEDIUM)
The `<select>` inside the slideover lists all models. Compatible models are enabled; incompatible ones are disabled but still present. There's no indication which models currently have a preset applied. The flow is: open preset → scroll to Apply section → pick from a long dropdown → click Apply → get success toast that says "Will apply on next load." The staging-only nature is a footgun — users may assume their setting took effect immediately.

#### F7 — "Applied to models" zone at bottom only appears when bindings exist (LOW)
This zone is conditional: `{appliedModelNames.length > 0 && (…)}`. A new user never sees this zone and has no context for the staged binding concept until they've already applied something.

#### F8 — Engine hint label is opaque to non-technical users (LOW)
"Engine hint" and `engine_hint` are developer vocabulary. No tooltip or description explains what choosing `llamacpp` vs `flm` means for the user. In the Advanced collapse it feels like a debug field rather than a key configuration knob.

#### F9 — Model page (Models view) shows recipe badges; Presets page doesn't cross-reference (LOW)
`08-models-page-recipe-badges.png` confirms recipe badges (llama.cpp, RyzenAI, etc.) appear on model rows — but this information never surfaces in the Presets view. A user must context-switch between views to understand which preset works for which model recipe.

#### F10 — Mobile: slideover is full-width but short (MEDIUM)
On 390px, the slideover renders full-width. The slideover is very long (system prompt, tools, behavior sliders, AutoOpt, advanced, apply section) — scrollable but with no visual progress indicator. On read-only starters the Clone button is at the very bottom; users may not realize there's more content below.

#### F11 — A11y: "engine_hint" select has no user-facing explanation (LOW)
The Advanced section has no tooltip/help copy explaining what engine_hint does. A screen reader user navigating to the `<select id>` (the label reads "Engine hint") gets no context. This is a UX-level a11y issue — the semantic label is there but the _meaning_ is absent.

#### F12 — A11y: Capability buttons disabled for starters lack `aria-disabled` announcement (LOW)
`disabled={isReadOnly}` on `<button>` elements correctly prevents interaction and is announced by screen readers, but the buttons show `on`/`off` chip styles suggesting they're interactive. The visual affordance contradicts the disabled state.

---

## Part 2 — Recommendations

### R1 — "Customize" button on starter card face (duplicate-to-edit)

**Problem solved:** F1 — the primary pain point Kyle described.

**Proposal:** Add a `Customize` button (or pencil icon + "Customize") directly on the `PresetCard` for starter cards, beside the existing `Clone` label. Clicking it:
1. Calls `handleClone(preset)` (already implemented in `PresetManager`)
2. Immediately opens the edit slideover on the clone
3. The clone's name defaults to `"${preset.name} (mine)"` — more intentional than `"(copy)"`

The existing `handleClone` already does steps 1+2 — the missing piece is the on-card CTA.

**Wireframe:**

```
┌──────────────────────────────────┐
│ STARTER                          │
│ ⚖  Balanced                     │
│ Sensible defaults for everyday…  │
│ ● chat                           │
│ params  temp 0.70 · ctx 16384    │
│ prompt  General  tools  ON       │
├──────────────────────────────────┤
│  [ Clone ]  [ Customize → ]      │  ← both actions visible
└──────────────────────────────────┘
```

> "Clone" keeps the current behavior (clone + open edit). Consider renaming it to "Duplicate" to avoid confusion with git clone. "Customize →" is a higher-intent CTA for the common case.

**A11y notes:**
- `aria-label="Customize Balanced preset"` on the button
- Focus moves to the title input in the new-clone slideover (already handled by `openSlideover` → focus trap)
- The new clone gets `starter: false`, so all `disabled` attributes are removed

**Difficulty:** S. No data model change — `handleClone` already exists. Only `PresetCard` render change.

---

### R2 — Promote `engine_hint` → `recipe_preference`; make it first-class

**Problem solved:** F2, F5, F8, and closes the recipe↔preset integration gap.

**Data model change:**

```ts
// presetStore.ts
export interface Preset {
  // ... existing fields ...
  recipe_preference?: PresetRecipe | 'auto';   // replaces / promotes engine_hint
  engine_hint?: PresetRecipe;                   // keep for backward compat, map to recipe_preference on read
}
```

Rename the field to signal intent: `engine_hint` implies a passive suggestion; `recipe_preference` implies the preset _wants_ a specific backend.

**UX for recipe selection at apply-time:**

When the user applies a preset to a model that supports multiple recipes (e.g., a model with both `llamacpp` and `ryzenai-llm`):

```
┌──────────────────────────────────────────────────────┐
│  Apply "Balanced" to  AMD-Llama-3.1-8B?              │
│                                                       │
│  This preset prefers: [ llama.cpp ]                  │
│  Model also supports: [ RyzenAI ]                    │
│                                                       │
│  Use preferred (llama.cpp) ▸   Pick different ▸      │
└──────────────────────────────────────────────────────┘
```

For `recipe_preference: 'auto'` presets: silently apply; no picker needed.

**UX for "preference unsupported" mismatch:**

```
┌──────────────────────────────────────────────────────┐
│  ⚠ "RyzenAI Quick" prefers RyzenAI,                 │
│    but AMD-Llama-3.1-8B only supports llama.cpp.    │
│                                                       │
│  Apply anyway (llama.cpp)  ·  Cancel                 │
└──────────────────────────────────────────────────────┘
```

Never silently apply a mismatched recipe — always inform the user.

**Visual change:** Show recipe preference on cards:

```
┌──────────────────────────────────┐
│ STARTER                          │
│ ⚖  Balanced          [llama.cpp]│  ← recipe badge, top-right
│ Sensible defaults for everyday…  │
│ ● chat                           │
│ params  temp 0.70 · ctx 16384    │
│ prompt  General  tools  ON       │
└──────────────────────────────────┘
```

Badge uses the same `recipeColor` / `recipeBadgeText` from `ModelManager.tsx` — reuse the existing `BackendBadge` component or extract it to a shared component.

**In slideover:** Move engine/recipe picker out of `<details>` into the main body, below the capability selector. For `auto` presets, show a passive info note: "No recipe preference — Lemonade picks the best available."

**A11y notes:**
- Recipe badge on card: `aria-label="Backend: llama.cpp"` and `title`
- Recipe picker in slideover: `<fieldset>` + `<legend>Backend preference</legend>`
- Mismatch dialog: must be an ARIA dialog (`role="alertdialog"`) with focus trap

**Difficulty:** M (3–4 days). Data model rename (backward-compatible), UI changes to cards + slideover, apply-time mismatch logic.

---

### R3 — "Duplicate to customize" from card + rename clone label

Already covered in R1. Noted separately for the roadmap.

---

### R4 — Collapse AutoOpt summary by default; fix mobile hierarchy

**Problem solved:** F3.

**Proposal:**
- `autoopt-summary` block: wrap in a `<details>` that is _closed_ by default. The summary line shows "AutoOpt #1 — active" + a toggle arrow. Full args only expand on click.
- On mobile, this entire block should be collapsed by default with `open={!isMobile}` controlled by `window.innerWidth < 480`.
- AutoOpt rail (left column): already collapses correctly on mobile.

**Wireframe (mobile, collapsed):**

```
Presets   Default · 9 starters · 0 yours
[ + New Preset ]  [ + Import ▾ ]

▶ AutoOpt: #1 (tap to expand)   ← one line, closed

● BUNDLED STARTERS 10
┌───────────────────────────────┐
│ STARTER  Default              │
│ …                             │
└───────────────────────────────┘
```

**A11y notes:**
- Native `<details>/<summary>` is inherently keyboard accessible
- Ensure the summary line has `aria-label="AutoOpt #1 — active, tap to expand"`

**Difficulty:** S (< half day).

---

### R5 — Show "Applied preset" badge on Models page model rows

**Problem solved:** F9 — surfacing preset state where models live.

**Proposal:** In `ModelManager.tsx`, the `renderModelRow` function already reads `loadApplied()`. Add a small preset chip beside the backend badge when a preset is applied to that model:

```
┌──────────────────────────────────────────────────────┐
│  [llama.cpp]  AMD-Llama-3.1-8B                       │
│  chat · tool-calling · 7.8 GB                        │
│  Preset: Balanced  ← small muted chip, clickable     │
│  [ Load ]  [ Delete ]                                 │
└──────────────────────────────────────────────────────┘
```

Clicking the chip navigates to Presets tab, opens the applied preset's slideover.

**A11y notes:**
- Chip is a `<button>` with `aria-label="Applied preset: Balanced — click to view"`.

**Difficulty:** M (1–2 days). Read from presetStore in ModelManager, add chip, nav handler.

---

### R6 — Improve "Apply to a model" flow in slideover

**Problem solved:** F6, F7.

**Proposal:**
- Replace the flat dropdown + "Will apply on next load" toast with a two-step affordance:
  1. **Model picker** — shows only compatible models, with a "currently applied" indicator if one exists.
  2. **Confirmation inline** — after picking, show a small summary: "Balanced → AMD-Llama-3.1-8B · Takes effect on next Load."
- For the staged binding concept: add a small informational blurb in the Apply section explaining _why_ it's staged ("Settings apply when you explicitly reload the model, so you won't lose an active conversation.").
- **"Applied to models" zone** — show even when empty, with copy: "No models have a preset staged yet. Pick a model below to stage one."

**A11y notes:**
- `role="status"` live region for the apply-success confirmation (currently using a toast `<p>` with no live region — this is the existing P1 a11y item).

**Difficulty:** S–M.

---

### R7 — Redesign new-preset wizard flow for recipe-first users

**Problem solved:** The "Kyle wants a ryzenai-llm + ctx 8192 + temp 0.3" scenario.

**Proposal:** Add an optional "Start from…" initial step when creating a new preset:

```
┌──────────────────────────────────────────────────────┐
│  New Preset — start from:                            │
│                                                       │
│  [ 🗒 Blank ]  [ ⚖ Clone a starter… ]               │
│  [ 🖥 My model defaults… ]                            │
│                                                       │
│  Backend preference (optional):                      │
│  ○ Auto   ○ llama.cpp   ● RyzenAI  ○ FLM  ○ vLLM   │
└──────────────────────────────────────────────────────┘
```

The backend preference row here is the key missing piece: it lets Kyle declare intent at creation time rather than hunting through Advanced options post-creation.

Selecting "Clone a starter…" opens an inline picker of STARTERS — same as clicking Clone on a card, but surface-able from the "+ New Preset" CTA.

**A11y notes:**
- Radio group for backend preference: `<fieldset role="radiogroup">` with `<legend>Backend preference</legend>`.
- Focus on first option when dialog opens.
- ESC closes without creating.

**Difficulty:** M (1–2 days).

---

### R8 — Clarify terminology: "engine_hint" → plain language in the UI

**Problem solved:** F8, F11.

**Proposal:**
- In any place "engine_hint" appears in UI copy, replace with "Backend preference" or "Preferred backend".
- Add a tooltip or `<p class="preset-help">` in the recipe picker: "Tells Lemonade which backend to use when this preset is applied to a model that supports multiple backends."
- Remove the word "hint" from the label entirely — it undersells the importance of the setting.

**Difficulty:** S (< half day).

---

## Part 3 — Phased Plan

### Phase A — Quick Wins (≤ 1 day each)

| # | Change | Effort | Fixes |
|---|--------|--------|-------|
| A1 | Add "Customize →" button to starter `PresetCard` face | S | F1 |
| A2 | Rename `Clone` → `Duplicate` on starter cards | XS | F1 clarity |
| A3 | Collapse `autoopt-summary` by default on mobile | S | F3 |
| A4 | Replace "engine_hint" label with "Backend preference" everywhere in UI | S | F8, F11 |
| A5 | Show "Applied to models" zone even when empty (with empty-state copy) | XS | F7 |
| A6 | Add `aria-label` to recipe select and live region to apply-success toast | S | existing P1 a11y debt |

**Total Phase A:** ~1.5–2 days of implementation.

---

### Phase B — Recipe Integration (≤ 1 week)

| # | Change | Effort | Fixes |
|---|--------|--------|-------|
| B1 | Rename `engine_hint` → `recipe_preference` in data model (backward-compat read) | S | F2 |
| B2 | Show recipe badge on `PresetCard` | S | F5 |
| B3 | Move recipe picker to main slideover body (out of Advanced) | S | F2, F8 |
| B4 | Apply-time recipe mismatch dialog | M | F2 (gap closure) |
| B5 | Multi-recipe model → recipe auto-select on apply | M | F2 (gap closure) |
| B6 | "Start from…" step in `+ New Preset` flow with backend picker | M | F2 |

**Total Phase B:** ~4–5 days. B4+B5 are the hardest pieces (requires knowing model's supported recipes at apply-time, which is available via `model.recipes`).

---

### Phase C — Polish & Longer-Term

| # | Change | Effort | Notes |
|---|--------|--------|-------|
| C1 | Applied-preset chip on Models page rows | M | Requires nav handler + cross-view state |
| C2 | Full mobile vertical layout pass for Presets | M | Bottom-sheet for slideover on 390px |
| C3 | Visual hierarchy redesign: de-emphasize AutoOpt rail; promote preset grid | M | AutoOpt is power-user; presets are day-1 |
| C4 | Preset filter chips (by capability, by recipe) | M | Useful once users have many presets |
| C5 | Model-page "active preset" summary in hero section | L | Cross-cutting state; owned by `ChatView` |
| C6 | Preset preview in Chat composer (current applied preset name + chip) | M | Helps users understand the active config |
| C7 | `prefers-reduced-motion` guard for slideover open/close animation | S | Existing ACCESSIBILITY.md item |
| C8 | High-contrast theme for preset badges | M | ACCESSIBILITY.md item #15 |

---

## Appendix — Code References

| Claim | File | Line(s) |
|-------|------|---------|
| `isReadOnly = preset.starter` | `PresetManager.tsx` | 749 |
| Starter cards only show Clone action | `PresetManager.tsx` | 722–730 |
| All form fields `disabled={isReadOnly}` | `PresetManager.tsx` | 920, 933, 969, 987–1007 |
| `engine_hint` in STARTERS | `presetStore.ts` | 116–124 |
| `engine_hint` buried in `<details>` | `PresetManager.tsx` | 1045–1063 |
| `handleClone` exists and works | `PresetManager.tsx` | 455–471 |
| Recipe badges in ModelManager | `ModelManager.tsx` | 75–121 |
| `CHAT_RECIPE_OPTIONS` with 4 backends | `ModelManager.tsx` | (grep match) |
| `withLoadedRecipeOptions` | `ModelManager.tsx` | 50–67 |

---

*Screenshots: `docs/screenshots/presets/*.png`  
Script: `scripts/screenshot-presets.mjs`*
