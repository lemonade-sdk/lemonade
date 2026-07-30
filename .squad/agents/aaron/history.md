# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** Backend Integrator — wrapped servers, model registry, recipes

## Core Context

Owns 7 backend integrations (llama.cpp, FastFlowLM, RyzenAI, vLLM, whisper.cpp,
stable-diffusion.cpp, Kokoro) plus the recipe & version pin system.

Backends are subprocess-based — never in-process. NPU exclusivity is critical.

## Learnings

### 2026-06-25

**MCP in GUI3 — Phase A PR #2418 open; Phase B design on #2404.** Mattingly built read-only MCP dashboard (`McpPanel.tsx`, ~175 LOC). Phase B (GUI3 as external MCP client host) design posted by Lovell. May route Phase B implementation to Aaron or Haise post-approval.

### 2026-07-30T09:17:15.715Z — MCP parity C++ implementation: ASSIGNED

**Revision assignment:** Aaron is NOW RESPONSIBLE for C++ MCP implementation fix.

**Defects to address:**
1. Backend lemonade_list_backends tool: Redact ction and elease_url fields from BackendManager::get_all_backends_status() output
2. Model lemonade_get_model_info tool: Omit or sanitize checkpoint/checkpoints fields (may contain filesystem paths)
3. Ensure all five new tools respect MCP security contract (no admin controls, URLs, credentials, paths)

**Files to revise:**
- src/cpp/server/mcp_server.cpp — MCP tool implementations and serializers
- src/cpp/include/lemon/mcp_server.h — MCP server header
- src/cpp/server/server.cpp — Dependency wiring (provide BackendManager to McpServer)

**Lockout rule:** Liebergot is BLOCKED from MCP C++ code. Only Aaron can make changes.

**Timeline:** ~1-2 days. Liebergot takes over test enhancements after Aaron's revision merges.

**Approval gates:** Lovell re-review after C++ fix + test enhancements + docs update.

### 2026-07-30T10:12:56.935-06:00 — MCP parity redaction revision

Replaced the parity diagnostics' generic JSON filtering with explicit public
allowlists. Backend status now retains only `recipe`, `name`, `state`, `message`,
and optional `version`; model and loaded-state payloads omit checkpoint data,
paths, process metadata, URLs, credentials, and control fields. The existing
`lemonade_list_models` loaded section now uses the same safe loaded-model
serializer. The `lemonade-server-core` Release target compiled successfully.

### 2026-07-30T10:21:06.265-06:00 — MCP revision cycle orchestration (Scribe)

Scribe recorded Aaron's completed C++ redaction fix in the orchestration log
(20260730T102106Z_aaron_mcp_redaction_background.md). Aaron's revision addresses
all three original defects: backend allowlist redaction, model checkpoint omission,
and control/URL/path field exclusion. Liebergot now takes over test enhancements
(locked out from C++ code per Lovell's strict lockout policy). Lovell will
re-review all three parallel revisions (Aaron C++, Liebergot tests, Kranz docs).
Handoff complete; awaiting merge.

### 2026-07-30T10:33:43.808-06:00 — MCP documentation final correction

Updated `docs/api/mcp.md` to match the public model serializer exactly:
removed non-emitted model fields from the documented allowlist and retained the
loaded-state fields. Documented lifecycle preflight as requiring an already
downloaded model and already available backend, with structured failure and no
model download, backend installation, or installer side effect when either
prerequisite is missing.

### 2026-07-30T10:51:42.821-06:00 — MCP test artifact revision

Revised `test/server_mcp.py` so diagnostic assertions consume live `/mcp` JSON text blocks, recursively enforce public allowlists and path/URL/credential restrictions, and explicitly check ordinary relative paths and bare URLs are absent from returned payloads. The missing-backend lifecycle test now selects a real installable backend with an installed fallback, enables `no_fetch_executables`, verifies structured `isError`, and compares model/backend state before and after to catch download or install side effects. Syntax, Black, whitespace, AST, and static coverage checks passed; the live daemon still advertises only the legacy five tools, so the targeted new test fails externally with `Unknown tool: lemonade_get_model_info` until the rebuilt server is running.
