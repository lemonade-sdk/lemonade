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
