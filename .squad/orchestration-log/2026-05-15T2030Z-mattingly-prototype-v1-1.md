### 2026-05-15T20:30:00Z — Mattingly: UI redesign prototype v1.1 polish

**Agent routed:** Mattingly (UI / Frontend)
**Requested by:** Kyle Poineal
**Mode:** background → completion
**Why chosen:** Kyle flagged a redundancy in the v1.0 prototype ("Chat" and "Chat (active)" as separate top-nav tabs) and asked for a fresh-eyes audit pass on the rest. Mattingly owns `prototype/ui-redesign/` and authored v1.0, so the fix + audit naturally routes to the same agent.

**Files authorized to read:**
- `prototype/ui-redesign/index.html`
- `prototype/ui-redesign/styles.css`
- `prototype/ui-redesign/tokens.css`
- `prototype/ui-redesign/app.js`
- `prototype/ui-redesign/README.md`
- `.squad/decisions.md`
- `.squad/agents/mattingly/history.md`

**Files produced / modified:**
- `prototype/ui-redesign/index.html` — top nav reduced from 5 to 4 tabs; two chat sections merged; rail items wired with `data-conv-model`/`data-conv-tps` + `role="option"`/`aria-selected`/`tabindex`; "+ New chat" pill added; "Active in chat" changed from button to status span; empty-state element added to Models view; matrix empty cells cleaned up; Discover cards container got `cards--discover` modifier.
- `prototype/ui-redesign/styles.css` — `.rail__new` pill, `[data-chat-state]` show/hide, distinct rail selected vs hover state (accent gradient + 3px left-edge), global `:focus-visible` rings, `.sr-only`, `.active-card__status`, `.empty-state`, `.cards--discover` variant, broken `:hover` states fixed, hardcoded `#c89a3a` retired.
- `prototype/ui-redesign/tokens.css` — added `--accent-deep` token.
- `prototype/ui-redesign/app.js` — boot now `activateView('chat')`; rail-item click + keyboard handler; `[data-new-chat]` handler; `[data-empty-go]` handler.
- `prototype/ui-redesign/README.md` — 4-view list, new v1.1 section documenting audit fixes.
- `.squad/decisions/inbox/mattingly-ui-redesign-v1-1-polish.md` (later merged by Scribe).
- `.squad/agents/mattingly/history.md` — appended v1.1 audit Learnings section.

**Outcome:** v1.1 shipped. Chat redundancy fixed via internal `[data-chat-state="empty|thread"]` state. 10 audit items addressed (IA fix + filter chip preselection + selected/hover collision + 2 dead hovers + 2 hardcoded hexes via new token + status-vs-button conflation + empty-cell typographic noise + missing focus rings + Discover/Connect visual collision + missing empty-state component). Three judgment calls left open for Kyle: (a) default rail state (currently expanded), (b) empty-state demo location (currently Models only), (c) Discover variant heaviness (currently light accent tint). Branch `feat/ui-testing`, live at <http://localhost:8080>.
