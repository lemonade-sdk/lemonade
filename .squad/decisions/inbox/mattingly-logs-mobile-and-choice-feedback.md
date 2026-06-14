# Decision Summary — Mobile API/Logs Connectivity + Choice Feedback

**Date:** 2026-06-13  
**Author:** Mattingly  
**Branch:** kpoin/ui-mobile-layout  
**Commit:** 8aedb2a5

---

## Task 1 — Logs WebSocket (and all API calls) fail on mobile

### Problem

`DEFAULT_BASE_URL = 'http://localhost:13305'`. On Kyle's phone hitting the dev server at `192.168.3.35:8080`, `window.location.hostname === '192.168.3.35'` — but the API client still constructed all URLs with `localhost`, which the phone resolves to itself (not the PC). Every HTTP request and WebSocket connection silently failed.

### Fix

**`api.ts` `baseUrl` getter (lines 341–355):** After normalizing the stored/default URL, detect `hostname === 'localhost' || '127.0.0.1'` and substitute `window.location.hostname`. This is a no-op on desktop (hostname stays `localhost`) but correctly maps to the LAN IP on mobile. Because all `_fetch` calls and `_buildWebSocketUrl` calls pull from `this.baseUrl`, the single change fixes every endpoint including the logs WebSocket.

**Why not patch individual call sites:** The architecture routes everything through `baseUrl` and `_buildWebSocketUrl`. A single getter fix is the correct level of abstraction.

**User-configured non-localhost URLs:** Explicitly skipped. If the user has set `http://192.168.1.x:PORT` via the Connect view, we respect it unchanged.

---

## Task 1b — Logs WebSocket `websocket_port` fallback

### Problem

The previous diagnosis (see `mattingly-logs-pane-diagnosis.md`) confirmed that `websocket_port` (from `/health`, typically 9000) *does* serve `/logs/stream`. The stale comment in `api.ts` line 823–826 was wrong. When the main port lacks WebSocket upgrade support (stale binary), the UI had no fallback and just reported error.

### Fix

`connectLogStream` now tries the main-port WebSocket first (`suppressPreOpenErrors = true` to keep the console clean). On failure, `tryFallback()` reads `this._healthData?.websocket_port` and retries using the dedicated port. If health data hasn't been fetched yet (unlikely — health polling runs on mount), the fallback silently fails and reports error. This mirrors the exact pattern used in `connectRealtimeTranscription`.

---

## Task 2 — Choice buttons show no feedback after selection

### Problem

`ask_question` tool calls rendered as clickable pill buttons. Clicking a button fired `onOptionSelect` (which sent the choice to the AI) but gave no visual feedback — the button sat there looking unclicked.

### Fix

`ToolCallsDisplay` gained `useState<Map<number, string>>` keyed by call array index `i`. Selection is tracked per tool-call-within-message. On selection:

1. **Selected button** → `options-block__btn--selected` (filled `--accent`), `aria-pressed="true"`, `✓ ` prefix.
2. **Other buttons** → `disabled`, `opacity: 0.4` via CSS.
3. **Confirmation** → `.options-block__confirmation` renders `✓ You chose: <choice>` in `--text-muted`.
4. **Custom input** → hidden after selection.
5. **Double-click guard** → `if (selectedChoice) return` in `handleSelect`.

### Design decisions

- **Local state over message mutation**: Storing in `useState` within `ToolCallsDisplay` is sufficient for the prototype. The calls array for a completed message is immutable; re-renders preserve state via React reconciliation. Only an unmount (e.g. switching conversations) would reset it — acceptable for a POC.
- **CSS tokens used**: `--accent`, `--bg-primary`, `--text-muted`, `--border` — consistent with existing theme.
- **No new deps**: Pure CSS + React state as directed.

---

## Verification

- **Task 1:** webpack build exits 0. The `baseUrl` getter substitutes correctly: accessing the dev server from any non-localhost origin (mobile or different host) will now derive the host from `window.location.hostname`. Logs WebSocket fallback will activate automatically when main-port fails.
- **Task 2:** Build exits 0. Selection state renders correctly at 390×844 viewport.
