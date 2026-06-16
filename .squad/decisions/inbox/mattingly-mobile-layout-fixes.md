# Mobile Layout Fixes — Decision Note

**Author:** Mattingly (UI Agent)  
**Date:** 2026-06-13  
**Branch:** kpoin/ui-mobile-layout  
**Status:** Implemented — build exits 0

---

## Context

Kyle tested the prototype on his phone (192.168.3.35:8080) and reported the UI looks "squished." This pass audited the prototype for mobile/responsive issues and fixed the highest-priority ones.

---

## Audit Findings

### A — Viewport meta tag
**Status: ✅ Already correct.**  
`src/index.html` line 5 already has:
```html
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
```

### B — CSS partials / media queries
Only two CSS files exist on this branch: `src/styles/styles.css` (6147→6245 lines) and `src/styles/tokens.css`. (No 22-partial split on this branch yet — that was described as a future commit.)

Existing media queries before this pass:
- `@media (max-width: 768px)` — tablet; hides rail, stacks panels, reduces padding
- `@media (max-width: 900px)` — dashboard grid collapses
- `@media (max-width: 720px)` — custom model form grid, account menu name hidden; image settings columns
- `@media (max-width: 1100px)` — image settings 4-col
- `@media (max-width: 980px)` — chat-with-logs layout

**No 480px (phone) breakpoint existed** — this is the root cause of mobile "squished" rendering.

**Issues found in CSS:**
1. **Nav overflow (P0)** — The 7-button titlebar nav (`chat / models / presets / backends / dashboard / logs / connect`) requires ~425px at minimum button size. On a 360-414px phone the buttons push each other, causing the nav to overflow into the right controls (theme toggle, account menu).
2. **Message content tables** — `.message__content table` had no overflow protection. Markdown-rendered tables would push the page horizontally.
3. **Touch targets below 44px (P1)** — `.composer__send` and `.composer__stop` were 36×36px. WCAG / iOS HIG minimum is 44×44px.
4. **Composer toolbar wrapping** — No `flex-wrap` on phone; long preset/tools labels could overflow.
5. **Slide-over width** — Already had `max-width: 90vw` but on phones 90vw still leaves an uncoverable strip; full-width is better UX on phone.
6. **Connect view padding** — `padding: 0 var(--space-6)` (32px each side) on a 360px phone = 296px content area — functional but tight.
7. **Chat-with-logs layout** — At 980px the rail collapsed but at 480px the rail was still shown in the grid template even though rail content is hidden at 768px.
8. **No breakpoint tokens** — `tokens.css` had no documented breakpoint variables.

### C — Layout components
- `App.tsx` root layout (grid-template-rows) is fine — it's two rows, not side-by-side on any axis.
- Rail hides correctly at 768px (`display: none`).
- `.chat` becomes single-column at 768px ✅
- ModelManager, PresetManager, BackendManager all get single-column at 768px ✅
- Slide-over already has `max-width: 90vw` ✅

### D — Touch targets
- Nav buttons: height is ~21px at 768px (padding 5px + font 11px). On phone this is below 44px but widening them would require a different layout pattern. Accepted as-is for nav; horizontal scroll compensates.
- Send/stop: Fixed to 44px.

### E — Typography
- Base size 14px (`--text-base`) — iOS will not auto-zoom inputs as long as inputs inherit this size (they do).
- Heading sizes reduce gracefully at 768px already.

---

## Breakpoints Established

```
--breakpoint-mobile:  480px   (phone — new)
--breakpoint-tablet:  768px   (tablet — was already in use)
--breakpoint-desktop: 1024px  (desktop narrow — documented, not yet used)
```

**Note:** CSS custom properties cannot be used inside `@media` expressions. These are documentation values in `tokens.css`. Raw pixel values are used in `@media` blocks.

---

## What Was Fixed (P0–P1)

| # | Issue | File | Lines |
|---|-------|------|-------|
| 1 | Nav horizontal scroll on 480px phone | `styles.css` | ~3670–3720 |
| 2 | Breakpoint tokens added | `tokens.css` | ~118–130 |
| 3 | Message content tables get `overflow-x: auto` | `styles.css` | ~755–763 |
| 4 | Send/stop buttons → 44px touch target at 480px | `styles.css` | ~3670–3720 |
| 5 | Composer toolbar `flex-wrap` at 480px | `styles.css` | ~3670–3720 |
| 6 | Slide-over full-width (`100vw`) at 480px | `styles.css` | ~3670–3720 |
| 7 | Connect view padding reduced at 480px | `styles.css` | ~3670–3720 |
| 8 | Chat inner tighter padding at 480px | `styles.css` | ~3670–3720 |
| 9 | Log viewer toolbar gap tightened at 480px | `styles.css` | ~4833–4838 |
| 10 | Chat-with-logs collapses rail column at 480px | `styles.css` | ~6218–6234 |

---

## Round 2 — 2026-06-13

Kyle re-tested on his 390px iPhone and filed two specific issues.

---

### Issue 1 — Nav icon-only at 480px

**Chosen approach: Option A (icons-only labels hidden)**

Rationale: Every nav item already had a suitable icon in `Icon.tsx`. Hiding text labels at 480px reduces each button from ~65px wide to ~28px wide; 7 × 28px ≈ 196px, well within the `1fr` nav column — no scrolling, no drawer, no state. Option B (hamburger/drawer) would require React state + a new drawer component and hides the navigation structure from first glance. For a POC, Option A is the correct trade-off.

**Icon assignments (chat→`chat`, models→`hard-drive`, presets→`sliders-horizontal`, backends→`box`, dashboard→`gauge`, logs→`logs`, connect→`plug`)**

Accessibility preserved via `title` + `aria-label` on every button; icon SVG is `aria-hidden="true"`. The `<span class="nav-label">` wrapper means desktop/tablet still show full text labels — only the 480px breakpoint hides them.

**Right-side cluster:** `.titlebar__right` now has `flex-shrink: 0` (global) to prevent the AccountMenu / theme-toggle / status-dot from ever compressing.

---

### Issue 2 — Model card vertical stack at 480px

`.row__content` at 480px changes from `display: grid (1fr auto)` to `display: flex; flex-direction: column; align-items: stretch`. This makes:
- Backend badge + icon + model name wrap naturally in a row at the top
- Model name gets full width and wraps (no more single-character truncation)
- Action buttons (Load, Delete, etc.) form a full-width, equally-spaced row at the bottom

Only `ModelManager.tsx` uses `row__content` / `row__main`; preset cards and backend cards use different class structures and did not need this fix.

---

### Build

`npm run build` exits **0** — only pre-existing bundle-size warnings, no new errors.

**Files changed:**
- `prototype/ui-redesign/src/App.tsx` — nav button render: `<Icon> + <span class="nav-label">` with title/aria-label
- `prototype/ui-redesign/src/styles/styles.css` — `.titlebar__nav button` inline-flex; `.titlebar__right` flex-shrink:0; 480px block: nav icon-only + model card stack

1. **Nav icon-only view at 480px** — ideally nav buttons would show just an icon glyph on very small screens, hiding text. This requires the Icon component and changes in `App.tsx`. The horizontal scroll workaround is acceptable for the demo.
2. **Nav button height < 44px** — the pill nav buttons are ~21px tall at 480px. To fix properly, convert to a bottom tab bar or a hamburger drawer. Out of scope for this pass.
3. **`icon-btn` and `titlebar__theme-toggle` at 32px** — below 44px. These are secondary controls and don't need to meet the full tap-target standard urgently.
4. **`ScriptProcessorNode` deprecation** — pre-existing, noted in history.
5. **ChatView.tsx 101KB** — decomposition effort, separate pass.
6. **Preset slide-over inner content on phone** — the `preset-assignment-panel` and `detail__grid` collapse to 1-col at 768px; confirmed working at 480px.
7. **Image settings composer at 480px** — already 2-col at 720px breakpoint; usable.

---

## Architectural Notes

- The 480px block was added in **three locations** in `styles.css` to keep it co-located with the feature sections it affects: (a) the main responsive section after the 768px block, (b) the log viewer section, (c) the end-of-file chat-with-logs section.
- `display: block; overflow-x: auto` on `.message__content table` is applied globally (not just at 480px) because table overflow is a bug at any viewport width.
- Desktop WebView2/WKWebView is unaffected — these breakpoints only fire at screen widths below 480px which no desktop window will hit unless explicitly resized very small.

---

## Build

`npm run build` exits **0** with only pre-existing bundle-size warnings (no new errors or TypeScript issues).

**Kyle's test URL:** `http://192.168.3.35:8080`
