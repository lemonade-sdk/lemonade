# MCP Parity Review — Session Log

**Session Timestamp:** 2026-07-30T09:17:15.715Z (UTC)
**Reviewer:** Lovell (Lead)
**Subject:** MCP Parity Layer Phase 1 — Design & Implementation
**Status:** REJECTED — Critical security defects

## Review Scope

- MCP design decision (2026-07-30): Approved in principle
- Liebergot C++ implementation: `lemonade_server_core` compiled
- Haise test suite: Python compilation passed
- Architecture: Five canonical tools, Router/ModelManager APIs, JSON-RPC 2.0

## Rejection Reason

**Three critical security defects prevent approval:**

1. **Backend status fields leak admin controls** (`action`, `release_url`)
2. **Model info fields expose filesystem paths** (`checkpoint`, `checkpoints`)
3. **Test assertions insufficient** — do not validate field redaction

## Defect Details

### Defect 1: Backend Diagnostics Leak Forbidden Fields

- **File:** `src/cpp/server/mcp_server.cpp:1472-1476`
- **Tool:** `lemonade_list_backends`
- **Issue:** Raw output from `BackendManager::get_all_backends_status()` includes `action` (admin control hint) and `release_url` (backend GitHub link)
- **Violation:** MCP contract prohibition on admin controls and backend URLs
- **Fix:** Whitelist backend fields: retain `recipe`, `backends[].name`, `backends[].state`, `backends[].message`, `backends[].version`; redact `action` and `release_url`

### Defect 2: Model Info Exposes Filesystem Paths

- **File:** `src/cpp/server/mcp_server.cpp:167-168` (model serialization), `src/cpp/server/mcp_server.cpp:128-134` (loaded model)
- **Tool:** `lemonade_get_model_info`
- **Issue:** `checkpoint` and `checkpoints` fields may contain filesystem paths from custom model registration
- **Violation:** MCP contract prohibition on filesystem paths
- **Fix:** Omit `checkpoint`/`checkpoints` or provide path-safe alternative (e.g., `has_checkpoint: boolean`)

### Defect 3: Tests Do Not Enforce Security Contract

- **File:** `test/server_mcp.py:496-524`
- **Issue:** Backend test validates shape only; model test validates known fields only; no assertions on **absence** of forbidden fields
- **Gap:** A tool can pass the test while still leaking data if implementation doesn't redact
- **Fix:** Add recursive field assertions; explicitly validate absence of `action`, `release_url`, paths, credentials, admin controls in all tool outputs

## Revision Assignments

**Lockout rules:**
- Liebergot: BLOCKED from MCP C++ code (Aaron takes ownership)
- Haise: BLOCKED from MCP test code (Liebergot takes ownership after Aaron's revision)
- Lovell: BLOCKED from C++ review (waiting for Aaron's revision)

**Timeline:**
1. Aaron: C++ implementation fix (~1-2 days)
2. Liebergot: Test assertion enhancements (after Aaron, ~1 day)
3. Kranz: Documentation update (parallel, ~1 day)
4. Lovell: Re-review when all ready (~immediate after)

## Evidence

- Compilation: PASS (`python -m py_compile`, `git diff --check`)
- Live tests: DEFERRED (server process still running; cannot rebuild)
- Code review: Identified three distinct contract violations with precise file:line citations

## Approval Gates

Revision must address:
1. ✓ Backend field redaction implementation
2. ✓ Model checkpoint field handling
3. ✓ Test assertions for forbidden field absence
4. ✓ Documentation update with redaction specification

Re-review scheduled after revision completion.
