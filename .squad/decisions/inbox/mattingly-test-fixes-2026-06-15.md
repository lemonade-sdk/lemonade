# Decision: Test failure fixes — 2026-06-15

**Author:** Mattingly (UI / Frontend)
**Date:** 2026-06-15
**Status:** Implemented and pushed (kpoin/ui-testing, commits c225d4bd–c0101518)
**Requested by:** Kyle (kpoin)

---

## Context

External collaborator reported 5 test failures on `kpoin/ui-testing`. Directive: fix all 5 without weakening test assertions. A full-suite run revealed a 6th pre-existing failure (A05 Dashboard contrast) that was also fixed.

Final result: **50 passed, 7 skipped, 0 failed** (same 7 skips as before, no new skips).

---

## Decisions made

### 1. `<main>` gets `tabIndex={-1}` (not moved to a different element)

**What:** Added `tabIndex={-1}` to `<main id="main-content">` in App.tsx.

**Why tabIndex=-1 specifically:** Makes the element programmatically focusable (anchor links, `el.focus()`) without adding it to the sequential Tab order. This is the correct ARIA pattern for skip-link targets. `tabIndex=0` would add `<main>` to the Tab order, which is undesirable (Tab would stop on the entire main content area).

**Alternatives considered:** Using `<div tabIndex={-1}>` as an inner wrapper. Rejected — the `<main>` landmark itself should be the focus target so screen readers hear the landmark role when focus moves there.

---

### 2. Button reset uses `outline: 0` not `outline: none`

**What:** Changed `button {}` reset from (no outline property) to `outline: 0`.

**Why not `outline: none`:** CSS `outline: none` sets `outline-style: none` but does NOT set `outline-width` to zero. Chromium's `getComputedStyle().outlineWidth` returns the UA-default width (3px) even when `outline-style: none`, causing the A29 test (`outlineWidth === '0px'`) to fail. `outline: 0` explicitly sets `outline-width: 0`.

**Why this doesn't break keyboard focus rings:** Our global `:focus-visible { outline: 2px solid var(--accent); }` rule has higher specificity (pseudo-class selector = 0-1-0) than the type selector `button` (0-0-1). `:focus-visible` is not overridden.

**Risk:** Suppresses browser default focus ring for all `<button>` elements. This is acceptable because we have an explicit `:focus-visible` ring. We must never remove the `:focus-visible` rule without replacing with an alternative visible focus indicator.

---

### 3. PresetCard: overlay-button pattern instead of `article[role=button]`

**What:** `<article role="button" tabIndex=0>` replaced with `<article>` containing `<button class="recipe-card__overlay-btn">` (absolute, inset: 0, z-index: 0). Card content at z-index: 1.

**Why:** WCAG 4.1.2 / axe `nested-interactive` rule forbids interactive elements containing other interactive elements. `<article role="button">` containing `<button>Clone</button>` is a violation.

**Why overlay button specifically:** The entire card surface should be clickable to open the slideover (UX requirement). The inner Clone/Apply/Export buttons are secondary actions. The overlay-button pattern allows the full card to be the primary interaction without nesting interactive roles.

**Behavioral note:** `focus-within` CSS that reveals action buttons continues to work because focusing the overlay button fires `:focus-within` on the parent `<article>`. Keyboard users Tab to the overlay button, see actions appear, can Tab again to reach Clone/Apply.

**Trade-off:** The overlay button has no visible text — its accessible name comes entirely from `aria-label="Open Preset: {name}"`. Screen readers announce "Open Preset: Balanced, button". This is an improvement over the previous `aria-label="Preset: Balanced"` on a `role="button"` div that didn't clearly indicate it's activatable.

---

### 4. Rail listbox empty state: `<p>` sibling, not `<li>` inside `<ul>`

**What:** `{conversations.length === 0 && <li className="rail__empty">No conversations yet</li>}` moved outside `<ul role="listbox">` and changed to `<p className="rail__empty">`.

**Why:** `role="listbox"` requires all children to be `role="option"` (or `role="group"` containing options). A plain `<li>` without `role="option"` violates `aria-required-children`. Moving it outside the listbox (as a sibling `<p>`) is semantically correct — the empty state is informational text about the listbox, not a listbox option.

**Alternative considered:** `<li role="option" aria-disabled="true">No conversations yet</li>`. Rejected — a disabled option implies there IS a selectable item but it's currently disabled. An empty state message is fundamentally different.

---

### 5. Dashboard contrast: color tokens, not opacity

**What:** Removed `opacity: 0.5` from `.dash2-slot-legend__item--idle`. Added `color: var(--text-tertiary)` overrides for idle label and idle TPS text.

**Why:** Axe evaluates contrast based on the actual rendered pixel color, not the CSS token value. `opacity: 0.5` composites `--text-secondary (#C7C2B5)` against `--surface-base (#1a1813)` to produce a rendered color of approximately `#716d64` (contrast 3.44:1 < 4.5:1 required). Using `--text-tertiary (#A8A39A)` at full opacity gives ~7:1 contrast — accessible and still visually distinct from active items (which use `--text-secondary` and `--text-primary`).

**General rule established:** Never use `opacity` to dim text in the UI. Always use a token with sufficient contrast at full opacity.

---

### 6. A03 test selector: `[data-view="presets"]` not `.manager`

**What:** `await page.waitForSelector('.manager')` changed to `await page.waitForSelector('[data-view="presets"]')` in the A03 test.

**Why this is a test fix, not lowering the bar:** The `waitForSelector` is a readiness guard (ensures the view rendered before running axe). `.manager` is ModelManager's root class — it was the wrong selector for the Presets view. Correcting it to the actual Presets view root selector is required to let axe run. The axe assertion itself is unchanged.

---

## Files changed

| File | Purpose |
|------|---------|
| `prototype/ui-redesign/src/App.tsx` | `tabIndex={-1}` on main; `'Dash'` → `'Dashboard'` nav label |
| `prototype/ui-redesign/src/styles/styles.css` | `outline: 0` button reset; overlay-button CSS; Dashboard contrast fix |
| `prototype/ui-redesign/src/components/ChatView.tsx` | Rail listbox `aria-label` + empty state as `<p>` |
| `prototype/ui-redesign/src/components/PresetManager.tsx` | PresetCard overlay-button pattern |
| `prototype/ui-redesign/tests/a11y.spec.ts` | A03 `waitForSelector` corrected |
