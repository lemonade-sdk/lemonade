# Orchestration Log Entry — Lovell

**Timestamp:** 2026-07-30T09:17:15.715Z (UTC)
**Agent:** Lovell
**Role:** Lead Review / Architecture
**Mode:** Sync
**Outcome:** REJECTED MCP parity implementation on security grounds; issued revision assignments and lockout directives.

## Summary

Lovell completed formal review of the MCP parity design (approved in principle on 2026-07-30T09:52:57) and Liebergot/Haise implementations. Review outcome: **CRITICAL REJECTION** due to three blocking defects related to data exposure and test adequacy.

## Review Scope

- C++ MCP server implementation (`src/cpp/server/mcp_server.cpp`)
- MCP server header and wiring (`src/cpp/include/lemon/`, `src/cpp/server/server.cpp` dependency injection)
- Python integration test suite (`test/server_mcp.py`)
- MCP API documentation (`docs/api/mcp.md`)

## Blocking Defects Found

### 1. Backend Diagnostics Leak Admin Controls (CRITICAL)

**Evidence:** `src/cpp/server/mcp_server.cpp:1472-1476` returns raw `BackendManager::get_all_backends_status()` result.

**Issue:** Backend status API adds `action` (admin control hint: "install", "reinstall", etc.) and `release_url` (backend GitHub/release link) fields. These are exposed unfiltered in the `lemonade_list_backends` MCP tool output, violating the MCP contract prohibition on "exposes admin-only controls" and "backend process controls."

**Requirement:** Whitelist/redact these fields in MCP serialization while preserving read-only backend state (recipe, backend name, state, version).

### 2. Model Diagnostics Expose Filesystem Paths (CRITICAL)

**Evidence:** `model_info_to_mcp_json()` emits `checkpoint`/`checkpoints` fields; `safe_loaded_model()` retains them in loaded model state.

**Issue:** Custom model registration (`src/cpp/server/server.cpp:5762-5779`) stores checkpoint values as filesystem paths or relative cache paths. The existing `sanitize_public_json()` does not treat checkpoint fields as paths, so the MCP contract prohibition on "no filesystem paths" is not enforced. The `lemonade_get_model_info` tool output can expose local paths to external MCP clients.

**Requirement:** Omit `checkpoint`/`checkpoints` fields or replace with path-safe public representation (e.g., boolean `has_checkpoint` or normalized identifier).

### 3. Test Assertions Do Not Enforce Security Contract (CRITICAL)

**Evidence:** `test/server_mcp.py:516-524` validates backend list shape only; `test/server_mcp.py:496-514` validates known fields only.

**Issue:** Tests pass if tools execute without error and return the expected structure. Tests do NOT assert absence of forbidden fields (`action`, `release_url`, filesystem paths, credentials, admin controls). A tool can pass the test suite while still leaking data if implementation is not defensive.

**Requirement:** Add recursive field assertions covering all tool outputs, explicitly validating that `action`, `release_url`, checkpoint paths, process IDs, URLs, and credentials are NOT present in results.

### 4. Documentation Does Not Define Public Contract (HIGH)

**Evidence:** `docs/api/mcp.md:206-217` describes backend and model diagnostics as "read-only" without specifying the redacted payload or field prohibitions.

**Issue:** The MCP documentation does not make the security boundary explicit. Future maintainers may assume unfiltered fields are safe because the doc says "read-only" without mentioning redaction.

**Requirement:** Update `docs/api/mcp.md` to document the exact public shape of backend status (recipe, name, state, message, version; NO action/release_url) and model info (all fields except checkpoint; NO filesystem paths). Document the principle: "Read-only does not mean unfiltered; all outputs are redacted to expose only diagnostic state, never admin/filesystem/credential information."

## Revision Assignments (Strict Lockout)

**Aaron (C++ Core):**
- Revise MCP server implementation and header
- Fix backend field redaction in `lemonade_list_backends` tool
- Fix model checkpoint field handling in `lemonade_get_model_info` tool
- Ensure all five tools respect field prohibitions
- **Lockout rule:** Liebergot and Haise CANNOT make changes to MCP C++ code until revision is approved

**Liebergot (MCP Tests) — AFTER Aaron's revision:**
- Extend test suite with recursive field redaction assertions
- Add assertions for absence of `action`, `release_url`, checkpoint paths
- Validate all five MCP tools against the security contract
- **Lockout rule:** Haise CANNOT revise MCP tests; Liebergot takes ownership

**Kranz (Documentation):**
- Update `docs/api/mcp.md` with corrected backend and model payloads
- Document the redaction principle and field prohibitions
- Cite the specific fields that are excluded and why
- **Lockout rule:** No changes to C++ or test code; documentation only

## Verification Performed

- Python compilation: **PASS** (`python -m py_compile test/server_mcp.py`)
- Git diff check: **PASS** (`git diff --check` on staged changes)
- Live MCP tests: **DEFERRED** (server not rebuilt/restarted; unavailable for live execution)

## Next Steps

1. **Aaron** revises C++ MCP implementation (backend redaction, model checkpoint handling)
2. **Liebergot** revises test assertions after Aaron's changes are merged
3. **Kranz** updates documentation in parallel
4. **Lovell** performs re-review when all three revisions are ready
5. **Live test execution** deferred until server is rebuilt with Aaron's changes

## Timeline Impact

- Revision loop: **+2-3 days** (Aaron C++, Liebergot tests, Kranz docs)
- No blocking on other work (GUI, logs, telemetry, apps, presets)
