# MCP Parity Test Coverage Scope Approval

**Date:** 2026-07-30T09:52:57.950-06:00
**Author:** Haise (QA)
**Verdict:** APPROVED
**Scope:** Python integration test coverage locked to `test/server_mcp.py`

---

## Test Coverage Scope

**Locked to:** `test/server_mcp.py` only

**NOT included (out of scope):**
- Production server code changes
- GUI changes to desktop or web app
- Preset configuration changes
- New test-runner additions

## Test Coverage Requirements

Validate:
1. **All ten tool schemas** — Request/response structure for lifecycle and diagnostic tools
2. **Structured lifecycle errors** — Proper error handling for:
   - Unknown models
   - Unsupported models
   - Undownloaded models
   - Virtual models (internal state, not user-facing)
3. **Safe diagnostic JSON** — Backend/model/server diagnostics do not leak:
   - Filesystem paths
   - Administrative controls
   - Backend URLs
   - Credentials or process information
4. **Explicit load/unload state transitions** — Verify model state changes correctly through load/unload operations
5. **`/mcp` API key enforcement** — API-key validation matches server configuration

## Model Discovery Strategy

**Success case:** Use the existing endpoint test model for lifecycle success.

**Not-downloaded model discovery:** Attempt to discover a supported not-downloaded model from `lemonade_list_models` without pulling it. If none is advertised by the server, retain the rest of the lifecycle coverage without the not-downloaded test case.

## Live Execution Timeline

Live execution is deferred until the C++ MCP parity implementation is built and the server is rebuilt with the five parity tools.

**Current status:** Static test structure ready; awaiting C++ implementation.

---

**Archived by:** Scribe (2026-07-30T10:27:18.631-06:00)
**Source:** `.squad/decisions.md`
