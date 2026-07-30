# MCP Parity Phase-1 Design Approval

**Date:** 2026-07-30T09:52:57.950-06:00
**Authors:** Lovell (Lead), Haise (QA)
**Verdict:** APPROVED
**Scope:** Phase-1 lifecycle parity with explicit not-downloaded model rejection

---

## Design Rationale

The existing MCP gateway in `mcp_server.cpp` is intentionally self-contained with five canonical tools, while lifecycle and diagnostic operations already have safe read-only or explicit Router/ModelManager/BackendManager APIs.

A first parity phase should:
1. **Reject not-downloaded models** instead of reusing the auto-load callback (which can trigger downloads)
2. This keeps MCP lifecycle operations **predictable and isolated**
3. Keep pull/delete/install/media operations **outside this phase** (deferred to later work)

## Authorization Scope

Kyle's direct request represents a **narrow authorization exception** to the older frontend-only UI-POC `lemond` restriction.

**What this authorization covers:**
- MCP gateway code changes (`src/cpp/server/mcp_server.cpp`, headers)
- Dependency wiring for lifecycle APIs
- Focused MCP integration tests
- MCP API documentation updates

**What this authorization does NOT cover:**
- GUI changes to the desktop or web app
- New REST API routes
- Preset configuration changes
- General server refactors

## Phase-1 Implementation Scope

**Five canonical MCP tools (lifecycle + diagnostics):**
1. `lemonade_list_models` — List available models (diagnostic)
2. `lemonade_get_model_info` — Get model details (diagnostic)
3. `lemonade_get_server_info` — Get server health (diagnostic)
4. `lemonade_load_model` — Load a model (lifecycle)
5. `lemonade_unload_model` — Unload a model (lifecycle)

**Plus read-only backend diagnostics:**
- `lemonade_list_backends` — List backend status (diagnostic only)

**Design decisions:**
- Reject unknown, unsupported, undownloaded, and virtual models
- Do not expose download, save-option, delete, install, or admin controls
- Use only read-only BackendManager status API for backend diagnostics
- Media and destructive operations remain deferred to a later phase

## Constraints Maintained

- Existing five MCP tools remain unchanged
- JSON-RPC batching/notifications/errors remain unchanged
- CORS configuration remains unchanged
- GET-405 behavior on `/mcp` remains unchanged
- `/mcp` API-key pre-route remains unchanged

---

**Archived by:** Scribe (2026-07-30T10:27:18.631-06:00)
**Source:** `.squad/decisions.md`
