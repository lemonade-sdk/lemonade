# MCP Parity Phase-1 Rejection — CRITICAL

**Date:** 2026-07-30T09:17:15.715-06:00
**Author:** Lovell (Lead Reviewer)
**Verdict:** REJECT
**Severity:** CRITICAL

---

## Summary

The MCP parity Phase-1 implementation was rejected due to four blocking defects that violate the governing security contract for diagnostic tools. The defects expose internal backend data, filesystem paths, and administrative controls that must not be visible to external MCP clients.

---

## Blocking Defects

### 1. Backend Diagnostics Leak Forbidden Fields

**File:** `src/cpp/server/mcp_server.cpp:1472–1476`
**Defect:** The `lemonade_list_backends` tool returns the raw result of `BackendManager::get_all_backends_status()`. This API adds `action` and `release_url` fields (visible in `src/cpp/server/backend_manager.cpp:834–842`).
**Violation:** These fields are forbidden by the MCP governing contract — they expose administrative control hints and backend URLs which are internal infrastructure details.
**Remediation:** Use an explicit public-field allowlist for backend status. Return only diagnostic state fields: `recipe`, `name`, `state`, `message`, `version`.

### 2. Model Diagnostics Expose Filesystem Paths

**File:** `src/cpp/server/mcp_server.cpp:167–168` and `src/cpp/server/mcp_server.cpp:128–134`
**Defect:** `model_info_to_mcp_json()` emits `checkpoint` and `checkpoints` fields. `safe_loaded_model()` retains `checkpoint` data.
**Root Cause:** Custom model registration stores checkpoint paths and relative cache paths (see `src/cpp/server/server.cpp:5762–5779`). The existing `sanitize_public_json()` filters selected key names and URL-shaped strings, but does not establish that checkpoint **values** are path-safe.
**Violation:** The MCP contract prohibits filesystem paths in diagnostic tools.
**Remediation:** Omit checkpoint/checkpoints fields entirely or replace them with a path-safe public representation (e.g., a descriptive string that does not contain paths).

### 3. Tests Do Not Enforce the Security Contract

**File:** `test/server_mcp.py:516–524` and `test/server_mcp.py:496–514`
**Defect:** Existing tests check backend list shape and model `pid`/`backend_url` presence, but do not recursively assert that forbidden fields are **absent**.
**Violations:** No assertions verify absence of `action`, `release_url`, checkpoint/path leakage, or nested admin controls.
**Remediation:** Add recursive assertions that walk all objects and arrays to verify:
- Forbidden fields are not present: `action`, `release_url`, `checkpoint`, `checkpoints`, PID, backend URL, credentials, process controls
- Only whitelisted fields are present in diagnostic payloads
- This applies at all nesting levels (backend, model, server-health, loaded-model diagnostics)

### 4. Documentation Does Not Match the Corrected Public Shape

**File:** `docs/api/mcp.md:206–217`
**Defect:** The documentation describes diagnostic tools as read-only, but does not define the redacted backend payload or explicitly prohibit control/URL/path fields.
**Violation:** External clients rely on the documentation to understand the safe payload contract. Without explicit definitions, the contract is incomplete.
**Remediation:** Update `docs/api/mcp.md` to:
- Name all five canonical lifecycle/diagnostic tools
- Define the public payload allowlists for each tool (backend status, model info, server health, loaded models)
- Explicitly state the prohibited fields: backend controls, release URLs, checkpoint/path values, PIDs, backend URLs, credentials, process controls
- Add a security principle: "Read-only does not mean unfiltered; all outputs are redacted to expose only diagnostic state, never admin/filesystem/credential information."

---

## Revision Assignments (Strict Lockout)

**These assignments are strict and non-negotiable:**

1. **C++ MCP implementation/header and MCP wiring: Aaron** (NOT Liebergot)
   - Scope: `src/cpp/server/mcp_server.cpp`, `src/cpp/include/lemon/mcp_server.h`, dependency wiring in `src/cpp/server/server.cpp`
   - Requirement: Explicit public serialization/redaction for backend and model diagnostics using allowlists
   - Constraint: Do NOT delegate the implementation back to Liebergot

2. **MCP tests: Liebergot** (NOT Haise)
   - Scope: `test/server_mcp.py` only
   - Requirement: Add recursive assertions for the rejected exposure classes; preserve existing lifecycle/schema checks
   - Constraint: Do NOT revise the rejected C++ or documentation artifacts

3. **MCP documentation: Kranz** (NOT Liebergot or Haise)
   - Scope: `docs/api/mcp.md` only
   - Requirement: Update to match the corrected public payload and prohibition contract
   - Constraint: Do NOT revise the rejected C++ or test artifacts

4. **Final Review: Lovell**
   - Scope: Re-review the complete corrected set after Aaron, Liebergot, and Kranz submit
   - Role: Different reviewer perspective; Lovell will NOT modify any implementation, test, or documentation artifact

---

## Verification Gate

**Before re-review submission:**
1. Targeted C++ build: `lemonade_server_core` must compile without warnings
2. Python syntax validation: `python -m py_compile test/server_mcp.py` must pass
3. `python test/server_mcp.py` against rebuilt/restarted server (when available)
4. Scoped diff validation: `git diff --check` must pass

**Live MCP execution is required.** A compile-only or diff-only result cannot close a diagnostic security review.

---

## Minimal Corrective Acceptance Criteria

For each defect to be considered resolved:

1. **Backend allowlist redaction:**
   - `lemonade_list_backends` returns only approved public status shape
   - `action`, `release_url`, backend URLs, admin/control fields, and equivalent nested data are absent

2. **Path exposure remediation:**
   - `lemonade_get_model_info` and `lemonade_get_server_info` omit `checkpoint`, `checkpoints`, process IDs, backend URLs, credentials, controls, and filesystem paths
   - All nesting levels checked

3. **Behavior unchanged:**
   - Read-only and lifecycle behavior unchanged
   - Diagnostics do not mutate state
   - Lifecycle tools do not download, install, delete, or accept arbitrary recipe settings

4. **Test coverage:**
   - `test/server_mcp.py` recursively asserts forbidden-field and path rules for backend, model, loaded-state, and server diagnostic payloads
   - Existing schema and lifecycle coverage retained

5. **Documentation accuracy:**
   - `docs/api/mcp.md` documents approved diagnostic fields
   - Explicitly states forbidden backend-control, URL, credential, process, checkpoint, and filesystem-path data

---

## Process Decision

For future MCP parity work:
- **Every externally visible diagnostic tool must have an explicit public field allowlist**
- **Recursive forbidden-data tests must be implemented** to enforce the contract
- **Documentation must define the safe payload shape before review**
- A compile-only or diff-only result cannot close a diagnostic security review
- **Live MCP execution is required when the server is available**

---

**Archived by:** Scribe (2026-07-30T10:27:18.631-06:00)
**Source:** `.squad/decisions.md` (184 KB, archived to reduce active size)
