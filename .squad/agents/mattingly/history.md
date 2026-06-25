# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** UI / Frontend — Tauri desktop app + web app

## Core Context

Leading UI POC on `feat/ui-testing`. React stays (ROI analysis showed ~300 LOC savings for 47-file rewrite = inverted ROI). Four explicit framework-change conditions set by Lovell; do not re-litigate without them.

**Critical constraint:** Debian native packaging requires `src/web-app/` to use only `/usr/share/nodejs` modules. Kranz is source of truth.

**Detailed history (2026-05-15 to 2026-06-24):** See `history-archive.md` (archived June 25 at 74KB).

## Learnings

### 2026-06-25: MCP Gateway Phase A — feat/gui3-mcp-dashboard (PR #2418, tracking #2417)

**Built:** `McpPanel.tsx` (~175 LOC) read-only MCP dashboard for POST /mcp Streamable HTTP gateway (protocol 2025-06-18). Wired into ConnectView; CSS grid-area `mcp`; extended tests with A80–A88 (9 new tests, 104 total passed).

**POST /mcp shape:** `initialize` request → protocolVersion/capabilities/serverInfo response. `tools/list` request → tools array (5 tools: lemonade_list_models, lemonade_chat, lemonade_transcribe_audio, lemonade_generate_image, lemonade_omni). Sequential POSTs (not batched). Health status = tools/list success.

**Key decisions:** (1) Standalone component. (2) Direct `fetch()` with `api.baseUrl` + `api.apiKey`. (3) Two sequential POSTs. (4) Health from tools/list (no separate ping). (5) POST-only; no SSE, no OAuth.

**Phase B:** Design posted by Lovell on #2404 (GUI3 as external MCP client host; localStorage config; namespaced tools; awaiting @fl0rianr approval).

**PR status:** #2418 open, awaiting review.

### Previous phases (2026-05-15 to 2026-06-24): Summary

**a11y:** Phase 1 (skip, main, focus rings) + Phase 2 (focus traps, aria-live, contrast) = A01–A34 passing. Phase 3 deferred.

**Mobile:** 3 rounds (nav icons-only, card stacking, cache-busting headers); bottom sheet for conversations; 390px Playwright verification.

**Presets & accessibility:** 5 NVDA issues (A35–A45); 61 tests. Patterns: overlay button, live regions, aria-describedby, role=radio, aria-pressed.

**Audit fixes:** 6 failures (A01, A03, A05, A09, A29, features/15) — all fixed.

See `history-archive.md` for full detailed learnings.
