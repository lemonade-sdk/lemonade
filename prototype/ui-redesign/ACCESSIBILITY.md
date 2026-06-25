# Accessibility Plan — Lemonade UI Redesign Prototype

**Date:** 2026-06-14  
**Branch:** `kpoin/ui-accessibility`  
**Scope:** `prototype/ui-redesign/` only  
**Status:** Phase 1 ✅ complete, Phase 2 ✅ mostly complete (items 16–18 deferred to Phase 3), Phase 3 (GUI3 preset a11y) ✅ complete, Group C (BackendManager) ✅ complete, Group D (MCP Gateway panel) ✅ complete  
**Test status (2026-06-25):** All 103 automated tests passing, 7 skipped, 0 failed on `feat/gui3-mcp-dashboard`

---

## Group D — MCP Gateway panel (2026-06-25, `feat/gui3-mcp-dashboard`)

Adds a new read-only MCP dashboard section to `ConnectView.tsx` exposing the existing `POST /mcp` Streamable HTTP endpoint.

### D1 — #2417: Endpoint URL visibility + copy-to-clipboard

- **Status:** ✅ **Fixed 2026-06-25**
- **What:** MCP endpoint URL is derived from the current server base URL + `/mcp`. Displayed in a labelled read-only `<input>` with a "Copy" `<button>`. The button has `aria-label="Copy MCP endpoint URL to clipboard"`. A persistent `<div role="status" aria-live="polite" aria-atomic="true">` always rendered in DOM carries the copy confirmation message ("Copied") — empty at rest so NVDA live region announcements trigger correctly on update.
- **WCAG:** 4.1.2 (Name, Role, Value), 4.1.3 (Status Messages)

### D2 — #2417: Health/status indicator

- **Status:** ✅ **Fixed 2026-06-25**
- **What:** Pings the MCP endpoint (initialize + tools/list) on connection and shows connected/unavailable/checking/idle. Implemented as `<div role="status" aria-live="polite" aria-atomic="true">` with a text label ("Connected", "Unavailable", etc.) beside a decorative dot. Not color-only — text label is always present.
- **WCAG:** 1.4.1 (Use of Color), 4.1.3 (Status Messages)

### D3 — #2417: Exposed tools list

- **Status:** ✅ **Fixed 2026-06-25**
- **What:** Calls `POST /mcp` with JSON-RPC 2.0 `initialize` then `tools/list` (protocol 2025-06-18, POST-only, no SSE). Renders returned tools (name + description) in a `<ul aria-label="MCP tools">`. Loading/error/empty states are all accessible (role="alert" on error, `aria-live`/`aria-busy` on loading). Auth header (`Authorization: Bearer <key>`) passed via existing `api.apiKey`. Refresh button has `aria-label="Refresh MCP tools list"`.
- **WCAG:** 4.1.2 (Name, Role, Value)

**Tests added:** A80–A88 (9 tests) in `tests/a11y.spec.ts`.  
**Files changed:** `McpPanel.tsx` (new), `ConnectView.tsx`, `styles/styles.css`, `tests/a11y.spec.ts`, `ACCESSIBILITY.md`.

---

## Group C — BackendManager (2026-06-22, `feat/gui3-backend-a11y`)

Fixes three NVDA/keyboard issues in `BackendManager.tsx`:

### C1 — #2343: Matrix cell keyboard operability

- **Status:** ✅ **Fixed 2026-06-22**
- **What:** Clickable `<div>` cells in the backend matrix were mouse-only — no keyboard focus, no ARIA role, no selected state.
- **Fix:** Overlay-button pattern (same as `recipe-card__overlay-btn` in PresetManager). Each `.cell--selectable` div now has `position: relative`. A `<button class="cell__select-btn">` with `position: absolute; inset: 0; z-index: 0` covers the full cell. The button has `aria-pressed` (selected state) and an `aria-label` including the recipe label and backend identifier. Action buttons (`.cell__actions`) have `position: relative; z-index: 1` so they remain clickable above the overlay. `:focus-visible` ring from global CSS applies automatically to the button.
- **WCAG:** 4.1.2 (Name, Role, Value), 2.1.1 (Keyboard)

### C2 — #2344: Action button qualified accessible names

- **Status:** ✅ **Fixed 2026-06-22**
- **What:** Install, Update, Uninstall, and Setup guide buttons had generic labels ("Install", "Update", "Uninstall") — indistinguishable when multiple backends share the same action.
- **Fix:** Added `aria-label` to each action button: `Install ${RECIPE_LABELS[recipe]} (${backend})`, `Update …`, `Uninstall …`, `Setup guide for … (${backend})`. Visible text unchanged.
- **WCAG:** 4.1.2 (Name, Role, Value)

### C3 — #2351: Toast and notice live regions

- **Status:** ✅ **Fixed 2026-06-22**
- **What:** `backends__toast` was conditionally mounted (`{toastMsg && <div>…</div>}`); `context-rail__notice` likewise. Mounting with content does not trigger NVDA live region announcements.
- **Fix:** Added two always-present `<div role="status" aria-live="polite" aria-atomic="true" className="sr-only">` elements alongside the visual elements:
  - `data-backends-toast-live` — mirrors `toastMsg` (install/update/uninstall progress and completion messages)
  - `data-backends-preset-notice-live` — mirrors `presetNotice` (preset assignment confirmation and incompatibility notices)
  Visual toast and notice remain conditionally rendered; only the sr-only live regions are always in DOM.
- **WCAG:** 4.1.3 (Status Messages)

**Tests added:** A51–A58 (8 tests) in `tests/a11y.spec.ts`.  
**Files changed:** `BackendManager.tsx`, `styles/styles.css`, `tests/a11y.spec.ts`, `ACCESSIBILITY.md`.

---

## Summary Table

| # | Item | Section | Priority | Effort | Status |
|---|------|---------|----------|--------|--------|
| 1 | `<main>` landmark | Standard A11y | **P0** | S | ✅ Done |
| 2 | `div.onClick` → `<button>` | Standard A11y | **P0** | M | ✅ Done (PresetCard) |
| 3 | Focus rings (global `outline: none`) | Standard A11y | **P0** | S | ✅ Done |
| 4 | Composer textarea `aria-label` | Standard A11y | **P0** | S | ✅ Done |
| 5 | Skip-to-main link | Standard A11y | **P0** | S | ✅ Done |
| 6 | Preset slideover focus trap + ESC | Standard A11y | **P0** | M | ✅ Done |
| 7 | `aria-live` for streaming output | Standard A11y | **P0** | M | ✅ Done |
| 8 | ARIA landmarks audit (nav, complementary) | Standard A11y | P1 | S | ✅ Done |
| 9 | `titlebar__status-dot` screen reader label | Standard A11y | P1 | S | ✅ Done |
| 10 | Persistence-toggle label (`ChatView.tsx:1725`) | Standard A11y | P1 | S | ✅ Done |
| 11 | Preset slideover unlabeled inputs | Standard A11y | P1 | S | ✅ Done |
| 12 | Color contrast audit (both themes) | Standard A11y | P1 | M | |
| 13 | `prefers-reduced-motion` (all animations) | LLM-specific | **P0** | M | ✅ Done |
| 14 | Font size / text scale controls | LLM-specific | P1 | M | |
| 15 | High-contrast theme mode | LLM-specific | P1 | L | |
| 16 | Keyboard shortcut system | LLM-specific | P1 | M | |
| 17 | Response verbosity setting | LLM-specific | P2 | M | |
| 18 | Dyslexia-friendly font option | LLM-specific | P2 | S | |
| 19 | Message role announcements for screen readers | LLM-specific | P2 | S | |
| 20 | Preset param controls programmatic labels (#2338) | GUI3 Presets | **P0** | S | ✅ Done 2026-06-22 |
| 21 | Backend/device fields discoverable (#2339) | GUI3 Presets | **P0** | S | ✅ Done 2026-06-22 |
| 22 | Preset card exposes metadata to AT (#2345) | GUI3 Presets | **P0** | S | ✅ Done 2026-06-22 |
| 23 | Capability chip toggle-button semantics (#2350) | GUI3 Presets | **P0** | S | ✅ Done 2026-06-22 (revised 2026-06-22) |
| 24 | AutoOpt run selection state (#2352) | GUI3 Presets | **P0** | S | ✅ Done 2026-06-22 |
| 25 | MCP endpoint URL + copy-to-clipboard (#2417) | MCP Gateway | **P0** | S | ✅ Done 2026-06-25 |
| 26 | MCP health/status indicator — not color-only (#2417) | MCP Gateway | **P0** | S | ✅ Done 2026-06-25 |
| 27 | MCP tools list accessible labels + live states (#2417) | MCP Gateway | **P0** | S | ✅ Done 2026-06-25 |

---

## 1. Standard A11y (WCAG 2.1 AA baseline)

---

### 1.1 Semantic HTML

#### 1.1.1 `<main>` landmark + skip-link target

- **Status:** ✅ **Fixed 2026-06-15** — `<main id="main-content" tabIndex={-1}>` in App.tsx. The `tabIndex={-1}` is required for in-page anchor focus (skip link Enter) to land on the landmark.
- **What:** The primary page content area needed a `<main>` landmark with skip-link support.
- **Priority:** P0 — WCAG 1.3.6 (Identify Purpose) and skip-link functionality

#### 1.1.2 `div.onClick` / `span.onClick` used as interactive elements

- **Status:** ✅ **Fixed 2026-06-15 for PresetCard** — `<article role="button">` replaced with overlay-button pattern (`<button class="recipe-card__overlay-btn">` at absolute inset:0, card content at z-index:1). ✅ **Fixed 2026-06-22 for BackendManager** — matrix cells use same overlay-button pattern (`<button class="cell__select-btn">`). Remaining `div.onClick` in AccountMenu, ModelManager are P1 deferred.
- **What:** Clickable divs/spans without button semantics — no keyboard activation, no role, not focusable.
- **Current state:**
  - `AccountMenu.tsx` — 2 `div.onClick` instances (account row items)
  - `BackendManager.tsx` — ✅ fixed (overlay button pattern, #2343)
  - `ChatView.tsx` — 1 `div.onClick` (backdrop `aria-hidden="true"` — OK as-is)
  - `ModelManager.tsx` — 3 `div.onClick` instances
  - `PresetManager.tsx` — ✅ fixed (overlay-button pattern on preset card)
- **Target:** Replace clickable `div`/`span` elements with `<button>` (or `<a>` where navigation applies). At minimum add `role="button"`, `tabIndex={0}`, and `onKeyDown` handling (`Enter`/`Space` triggers click).
- **Effort:** M
- **Priority:** P0 — WCAG 4.1.2 (Name, Role, Value)

#### 1.1.3 Message articles

- **What:** Chat messages have no semantic role — assistants and users are visually distinct but structurally flat.
- **Current state:** `ChatView.tsx` renders messages as `<div>` blocks inside a list-like container with no `<ol>`/`<ul>`/`<li>` or `<article>` structure. TBD — needs audit of exact message rendering markup.
- **Target:** Wrap each message in `<article>` with `aria-label="User message"` / `aria-label="Assistant message"`. Group in `<ol aria-label="Conversation">`.
- **Effort:** M
- **Priority:** P1

---

### 1.2 ARIA Roles & Landmarks

#### 1.2.1 Landmark audit

- **What:** Ensure all major regions are properly landmarked.
- **Current state:**
  - `<header className="titlebar">` — renders as `<header>` ✓ (implicit `banner` role)
  - `<nav className="titlebar__nav" aria-label="Primary">` ✓
  - `<aside className="rail">` ✓ (implicit `complementary` role)
  - `<aside className="chat__logs" aria-label="…">` ✓
  - No `<main>` — see 1.1.1
  - `<footer>` — not present; composer bar is a `<div>`
- **Target:** Add `<main>`. Consider `<footer>` or `role="contentinfo"` for the composer area if it's treated as a persistent control region.
- **Effort:** S
- **Priority:** P1

#### 1.2.2 Preset slideover missing dialog semantics

- **What:** The preset detail panel slides in but has no dialog ARIA attributes.
- **Current state:** `PresetManager.tsx:578` — `<aside className="slideover slideover--recipe…" aria-hidden={!selectedPreset}>`. No `role="dialog"`, no `aria-modal`, no `aria-labelledby`. Contrast with the bottom sheet (`ChatView.tsx:1738`) which correctly uses `role="dialog" aria-modal="true"`.
- **Target:** Add `role="dialog" aria-modal="true" aria-labelledby="slideover-title-id"`. Give the title element the matching `id`. Add focus trap (see 1.4.2).
- **Effort:** S (ARIA attrs only); M (combined with focus trap)
- **Priority:** P0 — screen readers will read through the rest of the page when the panel is open

#### 1.2.3 Composer model-search menu

- **What:** The floating model-search popover has `role="dialog"` but no `aria-modal`.
- **Current state:** `ChatView.tsx:1916` — `<div className="composer__model-menu" role="dialog" aria-label="Search models">`. No `aria-modal="true"`, no `aria-expanded` on the trigger button.
- **Target:** Add `aria-modal="true"`. Add `aria-expanded={modelMenuOpen}` to the trigger button. Add ESC-to-close.
- **Effort:** S
- **Priority:** P1

#### 1.2.4 Status dot — no accessible label

- **What:** Connection status indicator is not announced to screen readers.
- **Current state:** `App.tsx:293–296` — `<span className="titlebar__status-dot…" title="Connected"/>`. `title` is tooltip-only; screen readers may or may not read it. No `aria-label`, no `role`.
- **Target:** Add `role="status" aria-label={statusText}` so changes are announced, or add a visually-hidden `<span>` adjacent to the dot with live-region behavior.
- **Effort:** S
- **Priority:** P1

---

### 1.3 Keyboard Navigation

#### 1.3.1 Tab order

- **What:** Logical tab sequence through all interactive elements.
- **Current state:** TBD — needs keyboard walkthrough audit. Known issues:
  - `div.onClick` elements (see 1.1.2) are not in the tab order unless they have `tabIndex={0}`.
  - `PresetManager.tsx:610` adds `tabIndex={0}` to a `.recipe-card` div but no `role="button"` and no `onKeyDown`, so Enter/Space don't activate it.
  - Bottom sheet list items: tab order within the open sheet needs verification.
- **Target:** All interactive elements reachable by Tab in a logical reading order. No focus traps outside deliberate modal contexts.
- **Effort:** M
- **Priority:** P0

#### 1.3.2 ESC to close panels

- **What:** ESC should close any open panel/modal/popover.
- **Current state:**
  - Bottom sheet: ESC closes via `useEffect` keydown in `ChatView.tsx` ✓
  - Preset slideover: no ESC handler
  - Composer model-search menu: no ESC handler visible
  - AccountMenu panel (`role="dialog"`): no ESC handler visible
- **Target:** Every panel/overlay responds to Escape. Pattern: `useEffect` adds `keydown` listener on `document` when open, removes on unmount/close.
- **Effort:** M
- **Priority:** P0

#### 1.3.3 Enter/Space activation

- **What:** `role="button"` elements must fire on Enter and Space.
- **Current state:** `div.onClick` items (see 1.1.2) have no `onKeyDown`. The `tabIndex={0}` preset card at `PresetManager.tsx:610` only handles click, not keyboard.
- **Target:** Add `onKeyDown={e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); handler(); } }}` to all non-`<button>` interactive elements, or replace with `<button>`.
- **Effort:** S (per element; combined with 1.1.2)
- **Priority:** P0

---

### 1.4 Focus Management

#### 1.4.1 Global focus ring suppression

- **What:** Visible keyboard focus indicator is missing on most elements.
- **Current state:** `styles.css:39` — `input, textarea { outline: none; }`. Global button reset likely also removes outlines. Only `slider:focus-visible` has an explicit focus style (`styles.css:2830`). `composer__model-select:focus` at `styles.css:1133` sets no visible outline.
- **Target:** Remove blanket `outline: none`. Add `:focus-visible` ring to all interactive elements using `outline: 2px solid var(--accent); outline-offset: 2px`. Use `:focus-visible` (not `:focus`) to avoid showing rings on mouse clicks. Extend existing slider pattern to buttons, inputs, and cards.
- **Effort:** S
- **Priority:** P0 — WCAG 2.4.7 (Focus Visible)

#### 1.4.2 Focus trap in modals/slideovers

- **What:** Focus must be contained inside open modal dialogs.
- **Current state:**
  - Bottom sheet (`ChatView.tsx:1738`): no explicit focus trap — focus can leave the open sheet via Tab.
  - Preset slideover: no focus trap.
  - Composer model-search menu: no focus trap.
  - AccountMenu dialog (`AccountMenu.tsx:99`): no focus trap.
- **Target:** Implement focus trap — either manually (collect all focusable children, intercept Tab/Shift-Tab at boundaries) or via `focus-trap-react` library (Phase 2 dep). On open, move focus to the first focusable element inside the panel. On close, return focus to the trigger element.
- **Effort:** M
- **Priority:** P0 — WCAG 2.1.2 (No Keyboard Trap, inverse) + screen reader containment

#### 1.4.3 Focus return on close

- **What:** When a panel closes, focus should return to the element that opened it.
- **Current state:**
  - Bottom sheet: focus returns to `sheetTriggerRef` ✓ (`ChatView.tsx` Round 4 implementation)
  - Preset slideover: no focus return — focus is left wherever it was, likely outside the panel area
  - Composer model-search menu: no focus return
  - AccountMenu: no focus return
- **Target:** Store trigger ref before opening each panel; call `.focus()` on close.
- **Effort:** S (per panel, if refs are added)
- **Priority:** P1

---

### 1.5 Color Contrast

#### 1.5.1 Contrast audit — dark theme

- **What:** All text must meet 4.5:1 (normal text) or 3:1 (large text / UI components).
- **Current state (estimated, needs axe/Colour Contrast Analyser verification):**
  - `--text-primary: #F2EFE5` on `--surface-base: #0E0E0B` — ~15:1 ✓
  - `--text-secondary: #C7C2B5` on `--surface-base` — ~9:1 ✓
  - `--text-tertiary: #A8A39A` on `--surface-base` — ~6.5:1 ✓ but check on `--surface-raised: #2D2922`
  - `--text-disabled: #5C594F` on `--surface-base` — **estimated ~2.9:1 ✗** (fails for non-decorative text)
  - `--accent: #FCD846` used as active/badge text on `--surface-2: #1F1D17` — ~13:1 ✓
  - Cap chip colored dots on surface backgrounds — TBD, likely decorative (3:1 threshold)
- **Target:** Any non-decorative text ≥ 4.5:1. Flag `--text-disabled` usage — if used for meaningful (non-decorative) text, increase value. Consider `#7A776E` as a compliant replacement (~4.6:1 on `--surface-base`).
- **Effort:** M (audit + token adjustment)
- **Priority:** P1

#### 1.5.2 Contrast audit — light theme

- **Current state:**
  - `--text-primary: #000000` on `--surface-base: #FFFFFF` — 21:1 ✓
  - `--text-tertiary: #52525B` on `--surface-2: #F6F6F6` — ~7.1:1 ✓
  - `--text-tertiary: #52525B` on `--surface-3: #EFEDE0` — ~6.4:1 ✓
  - `--text-disabled: #999999` on `--surface-base: #FFFFFF` — **2.85:1 ✗** for non-decorative text
  - `--accent: #FCD846` (yellow) as background with `--accent-on: #000` text — 12.3:1 ✓
  - Yellow accent as text on white `--surface-base` — **1.4:1 ✗** — check if this pattern actually occurs
- **Target:** Audit all `var(--accent)` uses in light theme where accent is applied as a foreground color on a light surface (not as a background). Fix or gate those uses behind `[data-theme="light"]` override.
- **Effort:** M
- **Priority:** P1

---

### 1.6 Screen Reader Labels

#### 1.6.1 Composer textarea — no accessible label

- **What:** The main chat input field has no label; screen readers will announce only the placeholder.
- **Current state:** `ChatView.tsx:2149` — `<textarea ref={inputRef} className="composer__input" placeholder={composerPlaceholder} …>`. No `aria-label`, no `<label>`, no `aria-labelledby`.
- **Target:** Add `aria-label="Message"` (or a dynamic label reflecting current mode, e.g. `"Describe image"` in image mode).
- **Effort:** S
- **Priority:** P0

#### 1.6.2 Icon-only buttons — nav

- **Current state:** `App.tsx:271–279` — nav buttons have `title={label}` + `aria-label={label}` ✓ (added Round 2). SVG is `aria-hidden="true"` ✓.
- **Target:** Already compliant.
- **Effort:** —
- **Priority:** —

#### 1.6.5 Model row action buttons — no model-qualified accessible name (#2341)

- **What:** Per-row action buttons (Load, Download, Get & Load, Unload, Delete, cancel-download, Pin, speech toggle, Copy) rendered once per model row had identical accessible names across rows. NVDA users navigating by button role (pressing 'B') heard "Load, Load, Load…" with no way to distinguish targets.
- **Current state (pre-fix):** All Load buttons had visible text "Load" and no `aria-label`; all Delete buttons were icon-only `<button>` elements with a generic `title` but no `aria-label`.
- **Fix (2026-06-22):** Added `aria-label` to every per-row action button in `ModelManager.tsx`, including the icon-only X (delete/cancel) buttons and the pin/speech icon buttons in `renderPinAndSpeechControl`. The model or repo identifier already in scope for each row is embedded in the label: `aria-label="Load Llama-3.1-8B"`, `aria-label="Delete Qwen2.5-7B"`, `aria-label="Cancel download of Phi-3-mini"`, `aria-label="Pin Llama-3.1-8B"`, `aria-label="Download org/repo-id"`, etc. Visible button text is unchanged.
- **Effort:** S
- **Priority:** P0 — WCAG 4.1.2 (Name, Role, Value); WCAG 2.4.6 (Headings and Labels)



- **What:** A checkbox in the bottom sheet lacks an accessible label.
- **Current state:** `ChatView.tsx:1725` — `<input type="checkbox" checked={persistHistory} onChange={handlePersistenceToggle} />`. No `<label>`, no `aria-label`. Adjacent text is not programmatically associated.
- **Target:** Wrap in `<label>` or add `aria-label="Persist conversation history"` (or the actual UI text).
- **Effort:** S
- **Priority:** P1

#### 1.6.4 `titlebar__status-dot` — tooltip only

- See 1.2.4 above.

---

### 1.7 Form Labels

#### 1.7.1 ConnectView

- **Current state:** `ConnectView.tsx:100,114` — explicit `<label htmlFor>` with matching `id` on both inputs ✓. Checkbox at line 123 uses implicit `<label>` wrapping ✓.
- **Target:** Already compliant.
- **Effort:** —
- **Priority:** —

#### 1.7.2 AccountMenu login/register forms

- **Current state:** `AccountMenu.tsx:122–131` — implicit `<label>` wrapping for Name and Password inputs ✓.
- **Target:** Already compliant.
- **Effort:** —
- **Priority:** —

#### 1.7.3 Preset slideover inputs

- **What:** Name and description inputs in the preset edit panel have no labels.
- **Current state:** `PresetManager.tsx:762` — `<input className="slideover__title-input" placeholder="Preset name" …>` — no `<label>`, no `aria-label`. Line 773 — `<textarea className="slideover__desc-input" placeholder="Description (optional)" …>` — no label.
- **Target:** Add `aria-label="Preset name"` and `aria-label="Description"`, or wrap with visible `<label>`.
- **Effort:** S
- **Priority:** P1

#### 1.7.4 ModelManager custom model form

- **Current state:** `ModelManager.tsx:1992–2050` — all inputs use implicit `<label>` wrapping (Name, Capability, Recipe/backend, Checkpoint, Context tokens, Extra labels) ✓.
- **Target:** Already compliant.
- **Effort:** —
- **Priority:** —

#### 1.7.5 LogViewer search input

- **Current state:** `LogViewer.tsx:334` — `<input …>`. TBD — needs audit to confirm label presence.
- **Target:** Ensure `aria-label="Search logs"` or associated `<label>`.
- **Effort:** S
- **Priority:** P1

---

### 1.8 Skip Links

#### 1.8.1 Skip-to-main-content link

- **What:** Keyboard users should be able to jump past the titlebar nav directly to main content.
- **Current state:** No skip link exists anywhere. `index.html` only renders `<div id="root">`.
- **Target:** Add a visually-hidden `<a href="#main-content" className="skip-link">Skip to main content</a>` as the very first child of `<body>` (or `<div id="root">`). Show on `:focus`. Requires the `<main id="main-content">` from 1.1.1.  
  CSS pattern:
  ```css
  .skip-link {
    position: absolute;
    top: -40px;
    left: 0;
    background: var(--accent);
    color: var(--accent-on);
    padding: var(--space-2) var(--space-4);
    z-index: 9999;
    border-radius: var(--radius-sm);
  }
  .skip-link:focus { top: var(--space-2); }
  ```
- **Effort:** S
- **Priority:** P0

---

## 2. LLM-specific Accessibility

---

### 2.1 Font Size Controls

- **What:** User-adjustable text scale, independent of browser zoom (which scales layout too). Useful for low-vision users who want larger text without reflowing the entire layout.
- **Current state:** `tokens.css:56–62` defines fixed `px` font-size tokens (`--text-xs: 11px` … `--text-3xl: 32px`). No scale factor exists. All font-size usage in `styles.css` references these tokens directly. Browser zoom works but scales chrome, not just text.
- **Target:** Introduce a `--font-scale` multiplier token (default `1`). Rewrite font tokens as `calc(var(--font-scale) * <base-px>)`. Expose a small A−/A+ control in the Connect or a new Settings view. Persist in `localStorage` as `lemonade_font_scale`. Apply via `document.documentElement.style.setProperty('--font-scale', value)`.
- **Effort:** M
- **Priority:** P1

---

### 2.2 High-Contrast Theme Mode

- **What:** A third theme beyond dark/light, targeting users who need very high contrast (e.g. Windows High Contrast Mode users on non-Windows, or those with low vision needing ≥7:1 across all text).
- **Current state:** `tokens.css:139` defines `[data-theme="light"]` overrides. `App.tsx:82–91` only supports `'dark' | 'light'` — two themes via `localStorage`. No `forced-colors` or `prefers-contrast` media query handling anywhere.
- **Target:**
  - Add `[data-theme="high-contrast"]` token set with true-black/true-white palette and `≥7:1` everywhere.
  - Add `@media (forced-colors: active)` override block in `styles.css` using `ButtonText`, `HighlightText`, etc. system colors.
  - Add "High contrast" option to the theme toggle cycle (`dark → light → high-contrast → dark`).
- **Effort:** L
- **Priority:** P1

---

### 2.3 Reduced Motion Mode

- **What:** Respect the OS `prefers-reduced-motion: reduce` preference by disabling or replacing animations.
- **Current state:** `styles.css` has extensive animations and transitions with no `prefers-reduced-motion` guard:
  - `@keyframes pulse` (`styles.css:213`) — status dot loading state
  - `@keyframes rail-pulse` (`styles.css:425`) — rail loading shimmer
  - `transform: translateY(100%) → translateY(0)` — bottom sheet slide-in (280ms)
  - 30+ `transition:` declarations throughout (hover states, nav active, composer focus-within, etc.)
  - Token-by-token streaming text append (rapid DOM updates) — not an animation but perceptually motion-heavy
- **Target:** Add a single `@media (prefers-reduced-motion: reduce)` block at the end of `styles.css`:
  ```css
  @media (prefers-reduced-motion: reduce) {
    *, *::before, *::after {
      animation-duration: 0.01ms !important;
      animation-iteration-count: 1 !important;
      transition-duration: 0.01ms !important;
    }
  }
  ```
  For the bottom sheet, additionally set `transform: none` at the reduced-motion breakpoint to skip the slide-in entirely (snap open/closed).  
  Streaming output: batch DOM updates (see 2.5) — this helps both motion sensitivity and screen reader performance.
- **Effort:** M (CSS block is S; streaming batching is M)
- **Priority:** P0 — WCAG 2.3.3 (Animation from Interactions, AAA) and strong user expectation

---

### 2.4 Response Verbosity Settings

- **What:** Allow users to instruct the model to use concise, standard, or detailed responses — without needing to manually add a system prompt. Particularly useful for screen reader users who benefit from shorter, more scannable answers.
- **Current state:** No verbosity preference exists. System prompt is editable in the Presets system (`PresetManager.tsx`) but requires user to know what to write. No per-conversation shortcut.
- **Target:**
  - Add `verbosity: 'concise' | 'standard' | 'detailed'` to the chat state (default `'standard'`).
  - Inject a brief system-prompt prefix when non-standard: `"Respond concisely in 1-2 sentences unless asked for more."` / `"Provide detailed, step-by-step explanations."`.
  - Expose as a toolbar pill in the composer (similar to existing mode/preset pills) or as a per-conversation setting in the bottom sheet.
  - Persist per conversation scope in `localStorage`.
- **Effort:** M
- **Priority:** P2

---

### 2.5 Screen-Reader-Friendly Streaming Output

- **What:** Token-by-token streaming is hostile to screen readers — rapid DOM mutations in an `aria-live` region trigger constant re-reads, creating noise. No `aria-live` region currently exists, so streaming is also completely silent to screen readers.
- **Current state:** Streaming output appends text to message DOM nodes directly as tokens arrive (`ChatView.tsx` streaming hooks). No `aria-live` region. Screen readers get no announcement for in-progress assistant responses.
- **Target — two-level approach:**
  1. **In-progress (during stream):** Wrap the active streaming message in `aria-live="polite" aria-atomic="false"`. Use a debounced flush (200–400 ms) so updates are announced at sentence/clause boundaries rather than per-token. Pattern: accumulate tokens in a ref, flush to state every Nms or on punctuation (`., !, ?, \n`).
  2. **Completed messages:** Static; no live region needed. The full message is in the DOM.
  3. **Role announcement:** On each new assistant message start, announce `"Assistant is responding"` via a visually-hidden `aria-live="assertive"` status node. On finish, announce `"Response complete"`.
- **Effort:** M
- **Priority:** P0 — streaming output is the primary UI interaction and is completely inaccessible today

---

### 2.6 Dyslexia-Friendly Font Option

- **What:** Opt-in OpenDyslexic or Lexend font for users with dyslexia. These fonts increase letter distinctiveness to reduce letter-swapping errors.
- **Current state:** `tokens.css:51–53` — `--font-sans` uses system/Inter stack. No font override mechanism exists. No settings panel for typography.
- **Target:**
  - Add `[data-font="dyslexic"]` body attribute toggle.
  - Load Lexend via `@font-face` (self-hosted in `prototype/ui-redesign/src/` to avoid external CDN dependency) or flag as a Phase 2 dep.
  - In `[data-font="dyslexic"]`, override `--font-sans` to `'Lexend', sans-serif` and bump `--leading-normal` from `1.5` to `1.8`.
  - Expose toggle in a typography/accessibility settings section (same place as font scale — see 2.1).
  - Persist in `localStorage` as `lemonade_font_mode`.
- **Effort:** S (implementation once font is available); dep: Lexend font files (~Phase 2)
- **Priority:** P2

---

### 2.7 Keyboard Shortcuts

- **What:** Power-user shortcuts reduce reliance on pointer and reduce cognitive load for users who benefit from predictable keyboard patterns.
- **Current state:** Only `Enter` (send message, `ChatView.tsx` keydown handler) and `Shift+Enter` (newline) are wired. No other shortcuts. No cheat sheet UI.
- **Target — proposed shortcut set:**

  | Shortcut | Action |
  |----------|--------|
  | `Ctrl+N` / `Cmd+N` | New chat |
  | `Ctrl+Enter` / `Cmd+Enter` | Send message (alt to `Enter`) |
  | `Ctrl+/` | Focus composer |
  | `Ctrl+,` | Open Settings / Connect view |
  | `Ctrl+Shift+T` | Toggle theme (dark ↔ light) |
  | `Ctrl+Shift+L` | Toggle logs pane |
  | `Alt+↑` / `Alt+↓` | Navigate between conversations in rail |
  | `Escape` | Close open panel / cancel stream |
  | `?` (when not in input) | Open keyboard shortcut cheat sheet |

  Implementation approach:
  - Central `useKeyboardShortcuts` hook in `App.tsx` — listens at `document` level, dispatches actions via existing handlers.
  - Guard shortcuts when focus is inside `<textarea>` or `<input>` (only `Escape` and `Ctrl+*` combos should fire).
  - Cheat sheet: modal overlay triggered by `?`, lists all shortcuts in a `<table>`.
- **Effort:** M
- **Priority:** P1

---

### 2.8 Screen Reader Message Role Announcements

- **What:** Screen reader users benefit from knowing which conversational role each message belongs to, beyond just reading the text.
- **Current state:** Message blocks in `ChatView.tsx` are visually distinguished (color, alignment) but have no accessible role labels. TBD — exact markup needs audit.
- **Target:** Each message rendered as `<article aria-label="You said">` or `<article aria-label="Assistant said">` (see 1.1.3). Combined with 2.5, ensures screen reader reads "Assistant is responding" → streams debounced chunks → reads "Response complete".
- **Effort:** S (combined with 1.1.3 message article work)
- **Priority:** P2

---

## Phased Rollout

### Phase 1 — Quick wins (S-effort, P0/P1, no new deps)

Do these first. All are small changes with high compliance impact.

1. **1.1.1** ✅ DONE — Add `<main id="main-content">` wrapper in `App.tsx`
2. **1.8.1** ✅ DONE — Add skip link to `App.tsx` / `styles.css`
3. **1.4.1** ✅ DONE — Remove `outline: none` global reset; add `:focus-visible` ring using `--accent`
4. **1.6.1** ✅ DONE — Add `aria-label="Message"` to composer textarea (`ChatView.tsx:2149`)
5. **1.6.3** ✅ DONE — Persistence toggle checkbox already wrapped in `<label>` (implicit association confirmed); no change needed
6. **1.7.3** ✅ DONE — Add `aria-label` to preset slideover name/desc inputs (`PresetManager.tsx:762,773`)
7. **1.2.2** ✅ DONE — Add `role="dialog" aria-modal="true" aria-label` to preset slideover (`PresetManager.tsx:578`)
8. **2.3** ✅ DONE — Add `@media (prefers-reduced-motion: reduce)` block to `styles.css`
9. **1.2.4** ✅ DONE — Add `role="status" aria-label` to `titlebar__status-dot` (`App.tsx:293`)
10. **1.3.2 (partial)** ✅ DONE — Add ESC handler to preset slideover close

### Phase 2 — Structural (M-effort, P0/P1, possible new deps)

11. **1.1.2** ✅ DONE — Convert `div.onClick` to `<button>` in ModelManager (3 rows); BackendManager `div.cell__actions` is a non-interactive container (left as-is); PresetCard already had `role="button"` + `onKeyDown`; AccountMenu divs were already `<button>` elements
12. **1.4.2** ✅ DONE — Implement focus traps in bottom sheet (ChatView) and preset slideover (PresetManager) using custom `useFocusTrap` hook (no new dep); composer model-search and AccountMenu dialog: PARTIAL — remaining work in Phase 3
13. **1.3.2 (complete)** ✅ DONE — ESC for preset slideover; PARTIAL — composer model-search, AccountMenu still need ESC
14. **2.5** ✅ DONE — Add `aria-live` debounced streaming output (`aria-live="polite"` + 400ms/sentence-boundary flush) + assertive status announcements ("Assistant is responding" / "Response complete")
15. **1.5.1 / 1.5.2** ✅ DONE — `--text-disabled` fixed in both themes (dark: `#7A776E` ~4.6:1; light: `#767676` exactly 4.5:1); `--accent-fg` token added to gate yellow accent foreground in light theme; all `color: var(--accent)` foreground uses in styles.css migrated to `var(--accent-fg)`
16. **2.7** — Implement keyboard shortcut system — DEFERRED to Phase 3 (scope increase beyond Phase 2 budget)
17. **2.1** — Add `--font-scale` token + A−/A+ UI control — DEFERRED to Phase 3
18. **1.1.3** — Convert message list to `<ol>` with `<article>` per message — DEFERRED to Phase 3

### Phase 2 Group E — Chat rail + Account menu (2026-06-22)

23. **Chat conversation rail** ✅ DONE — Full listbox keyboard navigation in `ChatView.tsx`. Each `[role="option"]` now has `aria-selected`, roving `tabIndex` (selected=0, others=-1), unique `id` for both desktop rail (`rail-conv-{id}`) and mobile sheet (`sheet-conv-{id}`). `handleRailKeyDown` / `handleSheetKeyDown` wire ArrowUp/Down, Home/End, Enter/Space. Delete buttons carry qualified `aria-label="Delete conversation: {title}"` and `tabIndex={-1}` (not a Tab stop; reachable via NVDA browse mode). CSS: `.rail__item:focus-within .rail__item-delete { opacity: 1 }` shows delete button on keyboard focus.

24. **Account menu dialog** ✅ DONE — `AccountMenu.tsx` promoted to a complete modal dialog. Added `aria-modal="true"` on the panel, `ref` on both trigger and panel, `useFocusTrap(panelRef, open)` for Tab containment, Escape keydown handler (`closePanel()`) that restores focus to the trigger via `requestAnimationFrame`. The × close button now calls `closePanel()` instead of `setOpen(false)`. **Modal-vs-popover decision: MODAL.** The panel already declared `role="dialog"` and `aria-haspopup="dialog"`; it contains multi-mode forms (sign-in, create, settings) where interaction is modal in nature. Upgrading to full modal is consistent with the existing declaration and prevents screen-reader virtual-cursor from escaping into page content while the panel is open.

### Phase 3 — Enhancements (L-effort, P2, new deps) + GUI3 Preset A11y

19. **2.2** — High-contrast theme (`[data-theme="high-contrast"]` + `forced-colors` handling) — new token set
20. **2.6** — Dyslexia-friendly font (Lexend self-hosted font files — new asset dep)
21. **2.4** — Response verbosity preference in composer toolbar
22. **2.8** — Full message role announcement polish (combined with Phase 2 article work)

#### GUI3 Preset A11y — ✅ DONE 2026-06-22 (branch `feat/gui3-presets-a11y`)

All five items from the blind NVDA screen-reader user's feedback on UI 3 beta:

23. **#2338** ✅ DONE — All Preset parameter controls labelled via `htmlFor`/`id` (temperature, top_p, context size, top_k, repeat penalty, steps, CFG scale, engine hint, AutoOpt result, llamacpp_args, sdcpp_args) and via `aria-label` (image width, image height which share one visual label). File: `PresetManager.tsx` lines ~1000–1075.
24. **#2339** ✅ DONE — `llamacpp_backend` and `llamacpp_device` converted to `<input list=>` + `<datalist>` exposing known values (backends: auto/cpu/cuda/vulkan/kompute/metal/rpc/opencl/mmap; devices: Auto/CPU/CUDA0/CUDA1/Vulkan0/Vulkan1/Metal). File: `PresetManager.tsx` lines ~1060–1067.
25. **#2345** ✅ DONE — PresetCard overlay button gains `aria-describedby` pointing to a `sr-only` span containing: starter/manual-args state, applies_to capability list, parameter summary, prompt name, tools state. File: `PresetManager.tsx` lines ~700–726.
26. **#2350** ✅ DONE (revised) — Capability chip container changed from `role="radiogroup"` to `role="group" aria-label="Applies to capabilities"`; each chip button changed from `role="radio" aria-checked={…}` to `aria-pressed={…}` (toggle-button semantics). Radiogroup requires arrow-key navigation (ARIA APG / WCAG 2.1.1); toggle buttons are keyboard-correct with Tab + Enter/Space. File: `PresetManager.tsx` lines ~937–943.
27. **#2352** ✅ DONE — AutoOpt run buttons gain `aria-pressed={selectedAutoRunId === run.id}`, updated on selection change. File: `PresetManager.tsx` line ~528.

### GUI3 A11y Series — targeted fixes (branches feat/gui3-*)

**#2342 — Download progress: native progressbar semantics + status announcements** ✅ DONE (2026-06-22, `feat/gui3-download-a11y`)
- `DownloadManager.tsx` line ~283: replaced `<div aria-label="NN%">` with `role="progressbar"` + `aria-valuenow` / `aria-valuemin={0}` / `aria-valuemax={100}` + `aria-label` including the model name (`"Downloading Llama-3.1-8B: 42%"`). Visual `<span>` text gets `aria-hidden="true"` to avoid double-reading.
- Added always-present sr-only `role="status" aria-live="polite" aria-atomic="true"` live region inside the panel. Announces status transitions only (start / complete / error / cancelled / paused / resumed) — never on every percentage tick. Cleared when panel closes to prevent stale re-reads on reopen.
- Tests A59–A62 added (4 new tests): role/valuenow/min/max, model-name in label, live region present.

### Group F — Forms, Omni picker, icon names (2026-06-22)

**Branch:** `feat/gui3-forms-icons-a11y`  
Closes #2347 #2349 #2353

23. **#2347 (Item 10)** ✅ DONE — `OmniComponentPicker` full combobox semantics: `role="combobox"`, `aria-expanded`, `aria-controls` (points to listbox), `aria-activedescendant` (tracks active option), `aria-autocomplete="list"`, arrow-key navigation (ArrowDown/Up moves active option, Enter selects, Escape closes). Options converted from `<button role="option">` to `<div role="option">` (buttons cannot own `role="option"`). HF search action moved outside the `role="listbox"` to avoid invalid owned-element violation. Clear button gained `aria-label`. Label now associated via `htmlFor`/`id` pair. Keyboard focus indicator (`.omni-component-picker__option--focused`) added to CSS.
24. **#2349 (Item 12)** ✅ DONE — `ConnectView.tsx` cloud provider form: three bare inputs (name, base URL, API key) now have `<label className="sr-only" htmlFor=...>` with matching `id`. Edit-API-key inline input got `aria-label`. Marketplace search got `aria-label="Search marketplace apps"`. An `aria-describedby` hint span was added for the base URL format hint. Server URL and API key fields in the Server section were already correctly labeled.
25. **#2353 (Item 16)** ✅ DONE — Swept `LogViewer.tsx` (search input → `aria-label="Filter logs"`, Clear → `aria-label="Clear log output"`, Reconnect → `aria-label="Reconnect to log stream"`), `MarkdownMessage.tsx` (code-block copy button → `aria-label="Copy code"` in generated HTML), and `OmniComponentPicker` clear button → `aria-label`.

---

## Running the Accessibility Tests

The test suite lives in `prototype/ui-redesign/tests/a11y.spec.ts`.  
It uses **Playwright** (already a dev dependency) plus **@axe-core/playwright** for automated WCAG scans.

### Prerequisites

```bash
cd prototype/ui-redesign
npm install          # installs @axe-core/playwright if not already present
```

### Commands

```bash
# Run only a11y tests (headless, auto-starts dev server on port 8080)
npm run test:a11y

# Same via npx
npx playwright test tests/a11y.spec.ts

# Headed (visual browser)
npx playwright test tests/a11y.spec.ts --headed

# All tests (a11y + features)
npm test
```

> Playwright's `webServer` config in `playwright.config.ts` starts `npm run dev` automatically if nothing is already listening on port 8080. If you already have the dev server running, it reuses it (`reuseExistingServer: true`).

### Test groups (84 tests)

| Group | Tests | What it checks |
|-------|-------|----------------|
| axe-core scans | A01–A05 | WCAG 2.1 AA on Chat, Models, Presets, Connect, Dashboard — no serious/critical violations |
| Skip link | A06–A09 | Off-screen until focused; Tab once = skip link; Enter = focus `<main>`; visible ring |
| Landmarks | A10–A13 | `<main>` unique; `<nav aria-label="Primary">`; `role="status"` on status dot; aria-label values |
| Keyboard nav | A14–A16 | Nav buttons reachable; Tab reaches composer; Shift+Tab reverses |
| Focus trap — bottom sheet | A17–A20 | 390px mobile; opens with focus inside; Tab wraps; Esc closes; focus returns to trigger |
| Focus trap — slideover | A21–A24 | Preset slideover; focus moves in; Tab wraps; Esc closes; focus returns to card |
| aria-live regions | A25–A27 | Assertive + polite regions in DOM at load; both are `.sr-only` |
| :focus-visible rings | A28–A30 | Keyboard = outline present; mouse click = no ring; textarea keyboard ring present |
| prefers-reduced-motion | A31–A34 | Bottom sheet transition near-zero; normal = 280ms; all transitions; `transform: none` snap |
| Preset param labels (#2338) | A35–A37 | temperature/ctx-size/top_k sliders labelled via htmlFor/id |
| Backend/device discoverable (#2339) | A38–A39 | llamacpp_backend and llamacpp_device inputs have datalist with ≥3 options |
| Preset card metadata (#2345) | A40 | Card button aria-describedby includes applies_to, prompt, tools |
| Capability toggle-button semantics (#2350) | A41–A43 | Container has role=group + aria-label; buttons are plain buttons with aria-pressed; exactly 1 pressed=true, all others false |
| AutoOpt selection state (#2352) | A44–A45 | aria-pressed exposed; updates on click |
| Backend matrix + action/live regions | A51–A58 | Matrix cell buttons expose selection + labels; action buttons include recipe/backend; persistent status live regions exist |
| Model row qualified names | A46–A50 | Load/Delete/Download/Get&Load buttons include model name in aria-label; no bare generic names |
| Download progress bar | A59–A62 | `role="progressbar"` present; aria-valuenow/min/max correct; model name in label; sr-only status live region exists |
| Conversation rail listbox | A63–A66 | `role="listbox"` + label; selected option `aria-selected="true"` + tabIndex=0; ArrowDown moves focus; delete button name includes title |
| Account menu dialog | A67–A70 | `aria-haspopup` + `aria-expanded`; open = `role="dialog"` + `aria-modal`; focus moves in; Escape closes + focus restores |
| Omni picker combobox (#2347) | A71–A74 | `role="combobox"` + `aria-expanded`; focus opens/Escape closes; ArrowDown opens + listbox visible; `htmlFor`/`id` label pair |
| Connect form labels (#2349) | A75–A76 | Cloud provider fields found by `getByLabel`; marketplace search has `aria-label` |
| Icon button names (#2353) | A77–A79 | LogViewer search `aria-label`; Clear button `aria-label`; Omni picker clear `aria-label` |

### Known limitation

Tests A25–A27 only verify that the aria-live regions **exist**. Verifying that the polite region receives debounced content during streaming requires mocking `POST /api/v1/chat/completions` with a chunked SSE response via `page.route()`. That mock infrastructure is tracked as a TODO in the test file.

---

*Last updated: 2026-06-22 by Mattingly (GUI3 preset a11y items #2338 #2339 #2345 #2350 #2352; BackendManager #2343 #2344 #2351; model row qualified names #2341; download progress bar semantics #2342; Group E: conversation rail listbox + account menu modal dialog; Group F: combobox semantics, durable form labels, icon names)*
