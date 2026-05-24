# Lemonade UI Redesign Prototype

A static HTML/CSS/JS visualization of the redesigned lemonade UI. **Not production
code** — this is a design prototype for evaluating the direction before committing
to a real React port.

## Run it

From this directory:

```bash
python -m http.server 8080
```

Then open <http://localhost:8080>.

No build step. No dependencies. Plain HTML, CSS custom properties, and a small JS
file for view switching and slide-over open/close.

## What's in here

| File | What it is |
|------|-----------|
| `index.html`   | Single-page app, all 4 views toggleable from the top nav |
| `tokens.css`   | Design tokens — color, type scale, space, radii, motion |
| `styles.css`   | Layout + component styles, all expressed in tokens |
| `app.js`       | Tiny imperative DOM glue (no framework) |

## What to look at

Open it and click through the top nav in this order:

1. **Chat** — the hero view. Single view with two internal states:
   - *Empty state* (default): greeting card, capability chips, loaded-models row, conversation rail expanded on the left, composer at the bottom, status pill in the bottom-left.
   - *Thread state*: click any conversation in the left rail to swap into a populated thread with code block, per-message inline metrics, and a simulated streaming cursor on the last reply. The "+ New chat" pill at the top of the rail returns to the empty state.
2. **Models** — one list, three zones (Loaded / Installed / Available), filter chips at the top (none preselected — click to filter), capability badges replacing colored dots. Click any row to open the slide-over detail panel. There's a hidden empty-state sketch inside `.manager__body` (`<div class="empty-state" hidden>`) — drop the `hidden` attribute in DevTools to preview the fresh-install state.
3. **Backends** — device-first matrix (rows = devices, columns = capabilities). Update banner at top. Toggle "Show technical details" to reveal git SHAs. Empty matrix cells are now a clean em-dash with screen-reader text rather than filler copy.
4. **Connect** — tabs for Connect (3rd-party app integration) and Discover (curated model + persona feed). Discover cards get an accent left-edge strip and a softer warm tint to differentiate from Connect.

## What's new in v1.1 (audit pass)

- **Chat is now one view, not two.** Top nav dropped the redundant "Conversation" tab. The chat view carries its own empty ↔ thread state internally, driven by left-rail selection and the "+ New chat" pill.
- **Filter chips no longer pre-select.** The "Chat" capability filter used to be on by default, which made the filter row read like nav tabs. All chips now start off.
- **Selected vs hover are now visually distinct.** Selected rail items get an accent left-edge marker plus a tinted gradient background; hover is just `surface-2`.
- **Focus rings everywhere.** `:focus-visible` accent ring on rail items, chips, buttons, tabs, and the new-chat pill for keyboard navigation.
- **Fixed broken hovers.** `.cell__swap` and `.row__action--ghost` previously had identical default and hover backgrounds — they now actually respond.
- **Hardcoded `#c89a3a` retired.** Replaced with a new `--accent-deep` token; the lemon dot and assistant avatar gradients now flow entirely through tokens.
- **"Active in chat" is no longer a button.** It's a non-interactive `success`-colored status label, distinct from the "Switch to ▸" pill in the active-models cards.
- **Empty state component sketched** for Models view (hidden by default — see above).
- **Discover cards differentiated** from Connect via accent left strip and a faint warm gradient.

## What's mocked

Everything. There is no backend, no `lemond` connection, no real model loading,
no real chat streaming. The composer accepts text but doesn't send anywhere. The
slide-over panel always shows the same content shape with the row's data swapped
in. Filter chips toggle visual state but don't actually filter the rows.

## Design rationale

Lives in [`.squad/decisions.md`](../../.squad/decisions.md) — search for the
2026-05-15 entry titled *"UI/UX redesign study — competitive review + screenshot
critique"*. The proposals there are the spec this prototype is rendering.

## Browser support

Tested mental model: latest Chromium / WebKit / Firefox. Uses `backdrop-filter`
(WebKit-prefixed where needed), CSS custom properties, CSS grid. Honors
`prefers-reduced-motion`.
