# Logs Pane Diagnosis — 2026-06-13

**Investigator:** Mattingly  
**Reporter:** Kyle (kpoin)  
**Symptom:** Logs pane shows "Error" status; never connects.

---

## Code Review

### LogViewer.tsx (connection mechanism)

- **Mechanism:** WebSocket (native browser `WebSocket`)
- **Target URL:** `ws://localhost:13305/logs/stream` (derived from `baseUrl` via `_buildWebSocketUrl('/logs/stream')`)
- **Auth:** API key appended as `?api_key=...` query param if set (line 476 in `api.ts`)
- **Reconnect:** On disconnect, retries after 5 seconds via `setTimeout(connect, RECONNECT_DELAY)`. Health check (`api.health()`) is attempted first; if health fails, reconnect is also delayed 5s.
- **Error handling:** `onError` callback sets `connStatus` to `'error'`; console.warn printed. Visible "Error" label + "Reconnect" button shown.
- **Mount behavior:** Connects immediately on mount via `useEffect` → `tryConnect()` (line 209–241). Component rendered at `ChatView.tsx:1894` inside the Logs nav route.

### api.ts — `connectLogStream` (line 819–846)

- Calls `_buildWebSocketUrl('/logs/stream')` — uses main API port (13305), NOT the dedicated `websocket_port` from `/health`.
- Comment on lines 823–826 claims `websocket_port` is "used by realtime audio" only and doesn't serve logs. **This is incorrect** — confirmed by direct test that port 9000 serves `/logs/stream`.
- `_openLogSocket` (line 575–634): Opens WebSocket, 5-second connect timeout, sends `{type: 'logs.subscribe', after_seq: null}` on open.

---

## Live Browser Observations (Playwright)

**Status indicator:** `Error`

**Console messages (verbatim):**
```
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
[warning] [LogViewer] Error: Could not connect to log stream on the Lemonade API port.
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
```

**Page errors:** None  
**Request failures:** None  
**WebSocket events:**
- `[created] ws://localhost:13305/logs/stream` → `[close]` (immediate close, no open event)
- Repeated twice in the 10-second window.

---

## Backend Probe

### Lemonade server status

**Running:** Yes, PID 38952 (`LemonadeServer.exe`)  
**Port:** 13305  
**Health:** `{"status":"ok","version":"10.7.0","websocket_port":9000,...}`  
**Binary path:** `C:\Users\kpoin\AppData\Local\lemonade_server\bin\LemonadeServer.exe`  
**Binary last modified:** June 10, 2026 2:45 PM (MDT)  
**Process started:** June 11, 2026 8:30 AM

### Direct WebSocket tests

| Target | Result |
|--------|--------|
| `ws://localhost:13305/logs/stream` (main port) | **FAILS** — server responds with HTTP 200 + SPA HTML instead of 101 Upgrade |
| `ws://localhost:9000/logs/stream` (dedicated port) | **SUCCESS** — connects, receives `logs.snapshot` with entries |
| Raw HTTP upgrade headers to port 13305 | Returns 200 + HTML — no upgrade detection |

---

## Diagnosis

### Root Cause

**The running `LemonadeServer.exe` binary predates the WebSocket upgrade handler.**

- The `UpgradableFrontServer` class (which intercepts WebSocket upgrade requests on the main HTTP port and routes them to libwebsockets) was introduced in commit `20126a43` on **June 10 at 5:09 PM PDT** (6:09 PM MDT).
- The installed binary was compiled at **2:45 PM MDT** on June 10 — approximately **3.5 hours before** the upgrade handler was added.
- Without `UpgradableFrontServer`, the main port treats ALL requests as normal HTTP. `/logs/stream` hits the SPA catch-all and returns `index.html` (HTTP 200).
- The UI code (`api.ts` line 827) connects to `ws://<baseUrl>/logs/stream` — using the main port. It intentionally avoids `websocket_port` (9000) due to an incorrect comment stating that port doesn't serve logs.

**Two contributing factors:**
1. **Stale binary** — main-port WebSocket upgrade not compiled in.
2. **Incorrect fallback assumption in UI** — even if #1 is fixed, the UI has no fallback to `websocket_port` if the main-port upgrade fails.

### Evidence

- HTTP 200 (HTML) returned for WebSocket upgrade attempts on port 13305
- Successful WebSocket connection on port 9000 (dedicated) confirms server-side log streaming works
- Binary timestamp (2:45 PM) < feature commit timestamp (6:09 PM) on same day
- Browser console shows immediate WebSocket close without open event

### Confidence: **95% — High**

---

## Recommended Fix

### Option A — Rebuild and reinstall (immediate fix)

Kyle should rebuild `LemonadeServer.exe` from current HEAD (which includes `UpgradableFrontServer`) and restart the server:

```powershell
cmake --build --preset windows --target LemonadeServer
# Then restart LemonadeServer.exe (kill PID 38952, start new)
```

This will enable WebSocket upgrades on the main port and the UI will connect as-is.

### Option B — UI fallback to `websocket_port` (defense in depth)

Even after Option A, the UI should be hardened to fall back to `websocket_port` from `/health` if the main-port WebSocket fails. The incorrect comment at `api.ts:823–826` should be corrected. This is a **code change** — defer to Kyle's authorization.

**Proposed change (api.ts, line 819–838):**
- On initial connection failure (`.catch()` at line 836), retry using `this._healthData?.websocket_port` if available.
- Update the comment to reflect that `websocket_port` does serve `/logs/stream`.

### Recommendation

Do **Option A now** (rebuild), then schedule **Option B** as a follow-up improvement to handle mixed-version deployments gracefully.
