# Decision: Accessibility Phase 1 + Phase 2 Implementation

**Author:** Mattingly  
**Date:** 2026-06-14  
**Branch:** `kpoin/ui-accessibility`  
**Status:** Implemented — Phase 1 complete, Phase 2 items 11–15 complete, items 16–18 deferred  

---

## Context

ACCESSIBILITY.md (written 2026-06-14 planning pass) identified 19 accessibility items across 3 phases for `prototype/ui-redesign/`. This decision documents what was implemented in Phase 1 and Phase 2, the key technical choices made, and what remains.

---

## What shipped

### Phase 1 (all 10 items — quick wins)

- **Skip link:** `<a href="#main-content" class="skip-link">` as first child of root. Shows on `:focus`, hidden offscreen otherwise.
- **`<main>` landmark:** `<div className="view-container">` → `<main id="main-content" className="view-container">` in `App.tsx`.
- **Focus rings:** Removed `outline: none` from `input, textarea` global reset. Added `:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }`. Sliders excluded (use thumb box-shadow instead).
- **Composer textarea `aria-label`:** `aria-label="Message"` on `ChatView.tsx` composer textarea.
- **Persistence checkbox:** Already compliant (implicit `<label>` wrapping). No change.
- **Preset slideover input labels:** `aria-label="Preset name"` and `aria-label="Description"` added.
- **Preset slideover dialog semantics:** `role="dialog" aria-modal="true" aria-label="Preset details"` + ESC handler.
- **Prefers-reduced-motion:** `@media (prefers-reduced-motion: reduce)` block kills all animations/transitions; `.bottom-sheet { transform: none !important }` so sheet snaps rather than slides.
- **Status dot ARIA:** `role="status"` + `aria-label` added to the titlebar status indicator.

### Phase 2 (items 11–15)

- **`useFocusTrap` hook** (`src/hooks/useFocusTrap.ts`): Custom hook, no new npm dependency. Collects focusable children, traps Tab/Shift+Tab, excludes elements inside `aria-hidden="true"` ancestors.
- **Bottom sheet + preset slideover focus traps:** Both panels activate the trap when open; focus returns to trigger on close.
- **div→button conversions** (ModelManager): Three `div.row__content` elements converted to `<button type="button" aria-expanded>`. `CopyInlineButton` extracted to avoid button-in-button nesting. BackendManager's `div.cell__actions` left as-is (non-interactive container).
- **`aria-live` streaming output:** Two hidden `.sr-only` regions. `aria-live="assertive"` announces "Assistant is responding" / "Response complete" on stream start/end. `aria-live="polite"` receives debounced `streamingContent` flush (400ms default; 100ms on sentence-boundary). No per-token updates; no screen reader spam.
- **Color contrast fixes:** `--text-disabled` raised to `#7A776E` (dark, ~4.6:1) and `#767676` (light, 4.5:1). New `--accent-fg` token resolves to `--accent` in dark and `--accent-deep` in light; all `color: var(--accent)` foreground uses in styles.css migrated to `var(--accent-fg)`.

---

## Key decisions

### No `focus-trap-react` dependency

Custom `useFocusTrap` hook is ~50 LOC and covers the exact patterns needed (Tab wrap, Shift+Tab wrap, aria-hidden exclusion). Adding `focus-trap-react` for 50 LOC would be over-engineering for a prototype. Justified in commit message.

### `--accent-fg` token instead of per-rule `[data-theme="light"]` overrides

The yellow `--accent` (#FCD846) on white surfaces fails WCAG AA (1.4:1). Rather than patching 20+ individual CSS rules with `[data-theme="light"]` overrides, introduced `--accent-fg` as a semantic alias that resolves to `--accent` in dark and `--accent-deep` (a deeper amber that passes) in light. Single token migration covers all instances.

### aria-live debounce strategy for streaming

Token-by-token DOM updates would interrupt screen readers constantly. Sentence-boundary detection (`/[.!?\n]/` on last 2 chars) triggers a 100ms flush for natural pauses; otherwise 400ms debounce. The visible streaming cursor and markdown rendering are completely unchanged — only the hidden live region is debounced. Screen readers hear complete phrases rather than individual tokens.

### CopyInlineButton extraction from row buttons

Converting `div.row__content` to `<button>` created button-in-button violations (CopyInlineButton is itself a button). Extracted CopyInlineButton outside the main expand button using a `.row__summary` grid wrapper (`1fr auto`). This is semantically correct HTML and fixes the nested interactive element problem.

---

## What was deferred (Phase 3)

- **Keyboard shortcut system** (item 16): Requires central `useKeyboardShortcuts` hook, cheat sheet modal, focus-in-input guards. Scope exceeds Phase 2 budget.
- **Font scale control** (item 17): Requires `--font-scale` token, `calc()` rewrite of font-size tokens, A−/A+ UI in a settings panel. Deferred.
- **Message article/ol structure** (item 18): Large ChatView refactor; message rendering spans multiple components. Deferred.
- **Focus traps for composer model-search + AccountMenu** (partial): Bottom sheet and preset slideover traps are done. The composer model-search popover and AccountMenu dialog still need traps + ESC handlers. Noted in ACCESSIBILITY.md.

---

## Files changed

| File | Change |
|------|--------|
| `src/App.tsx` | Skip link, `<main>`, status dot ARIA |
| `src/components/ChatView.tsx` | Textarea aria-label, aria-live regions, bottom-sheet focus trap |
| `src/components/ModelManager.tsx` | div→button row conversions, CopyInlineButton extraction |
| `src/components/PresetManager.tsx` | Dialog semantics, ESC, focus trap, focus return, input labels |
| `src/hooks/useFocusTrap.ts` | New hook file |
| `src/styles/styles.css` | Focus rings, skip link, sr-only, reduced motion, accent-fg migration, row__summary |
| `src/styles/tokens.css` | --text-disabled fixes, --accent-fg token |
| `ACCESSIBILITY.md` | Status updated, Phase 1/2 items marked ✅ DONE |

---

## Impact on other agents

All changes are contained in `prototype/ui-redesign/`. No C++, no Tauri `src/app/`, no server code touched.

---

*Written: 2026-06-14 by Mattingly*
