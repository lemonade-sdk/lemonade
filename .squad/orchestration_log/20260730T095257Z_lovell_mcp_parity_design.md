# Orchestration Entry: MCP Parity Design Review — Lovell

**Agent:** Lovell (Lead)
**Mode:** Sync (design review)
**Trigger:** Phase-1 MCP gateway parity authorization
**Created:** 2026-07-30T09:52:57.950-06:00

## Context

Kyle authorized a narrow exception to the frontend-only UI-POC `lemond` restriction: scope MCP gateway code, dependency wiring, focused MCP integration tests, and MCP API documentation. Lovell conducted design review to establish the safe boundary.

## Design Decision

**Verdict: Approved — Phase-1 MCP server lifecycle parity design.**

### Key Points

The existing MCP gateway is intentionally self-contained in `mcp_server.cpp` with five tools. Lifecycle and diagnostics already have safe read-only or explicit Router/ModelManager/BackendManager APIs.

A first parity layer should reject not-downloaded models instead of reusing the auto-load callback. This keeps MCP lifecycle operations predictable and keeps pull/delete/install/media outside the phase.

### Authorization Scope

Kyle's direct request is a **narrow authorization exception** to the older frontend-only UI-POC restriction. The exception **does not authorize:**
- GUI changes
- New routes
- Presets or general server refactors

The exception **authorizes only:**
- MCP gateway code in `mcp_server.cpp`
- Dependency wiring and configuration
- Focused MCP integration tests in `test/server_mcp.py`
- MCP API documentation

## Cross-Agent Contract

**Haise (QA):** Test scope locked to `test/server_mcp.py`. Validate tool schemas, lifecycle errors, safe diagnostic JSON, state transitions, and API-key enforcement. Live execution deferred until C++ implementation.

**Mattingly (Frontend):** No new UI routes, presets, or GUI3 panels as part of MCP parity. MCP panel enhancements deferred to Phase 2.

## Status

**Approved:** 2026-07-30T09:52:57.950-06:00
**Next:** Lovell awaiting Mattingly/Haise implementation and test coverage in parallel branches

---
**Ledger entry:** Orchestration manifest 2026-07-30 MCP parity design gate
