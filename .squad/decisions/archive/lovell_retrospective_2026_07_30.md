# MCP Parity Post-Rejection Retrospective

**Recorded:** 2026-07-30T09:52:57.950-06:00
**Facilitator:** Lovell
**Scope:** Retrospective only; no source, test, or documentation artifact was modified

---

## Facts Established

1. **Backend data exposure:** `lemonade_list_backends` serializes the raw result of `BackendManager::get_all_backends_status()`. The rejected review identified `action` and `release_url` in that result; both are forbidden in the MCP public contract.

2. **Model path exposure:** `safe_loaded_model()` and `model_info_to_mcp_json()` include `checkpoint` data. The public sanitizer filters selected key names and URL-shaped strings, but does not establish that checkpoint **values** are safe. Custom model registration can store checkpoint and cache paths.

3. **Test coverage gaps:** The MCP tests check model-info shape plus `pid` and `backend_url`, check backend-list shape, and check credential-like keys in server diagnostics. They do NOT recursively reject `action`, `release_url`, checkpoint fields, or filesystem-path leakage.

4. **Documentation incompleteness:** The MCP documentation describes the diagnostic tools as read-only and says that controls, URLs, credentials, and paths are not exposed, but does NOT define the corrected backend payload or the complete forbidden-field contract.

5. **Verification status:** `py_compile` and scoped diff validation passed. The live MCP suite was NOT run because the rebuilt and restarted server was unavailable.

6. **Lockout status (fixed):** Revision lockout is fixed:
   - Aaron owns the C++ MCP implementation/header and wiring
   - Liebergot owns the MCP tests
   - Kranz owns the MCP documentation
   - Lovell remains the reviewer

---

## Facts-Only Root Cause Analysis

### 1. Diagnostic Implementation Exposed Internal Data Directly

**Root cause:** The implementation relied on a generic sanitizer (`sanitize_public_json()`) instead of an explicit public schema for each diagnostic payload.

**Why this happened:** No explicit allowlist pattern was established; implementation pattern was to serialize the backend-manager result and filter known-bad keys after the fact.

**Prevention:** Establish an explicit public-schema pattern for every externally-visible diagnostic tool. Define the allowlist FIRST; serialize ONLY those fields.

### 2. Model Diagnostics Retained Checkpoint Fields Without Path-Safety Contract

**Root cause:** The model diagnostic serializers retained checkpoint fields without a contract-level guarantee that their values were path-safe.

**Why this happened:** Checkpoint values are legitimate for internal debugging, but the public contract does not permit filesystem paths. The distinction between internal representation and public schema was not enforced.

**Prevention:** Document the contract at the field level. Path-bearing fields must be either:
- Omitted from public payloads, or
- Transformed to a safe public representation (e.g., hashed or redacted)

### 3. Test Coverage Validated Shape But Not Security Requirements

**Root cause:** Test coverage validated request/response structure and selected credential markers, but did not encode the rejected security requirements as recursive assertions.

**Why this happened:** Tests were written to verify "happy path" structure, not to enforce the security contract. No recursive pattern was established for walking nested objects and verifying field absence.

**Prevention:** Write tests AFTER defining the security contract, not after. Encode the prohibited-field rules as recursive assertions that cover all nesting levels.

### 4. Documentation Covered Operation Semantics But Not Payload Shape

**Root cause:** Documentation covered operation semantics ("read-only", "no downloads") but did not define the exact safe public payload shape or all forbidden fields.

**Why this happened:** Documentation was written to explain what the tools do, not to define the data contract. The payload sanitization logic was treated as implementation detail, not as a contract element.

**Prevention:** Define the payload contract FIRST in documentation. Let the implementation conform to the documented contract, not the other way around.

### 5. Verification Stopped at Static Checks

**Root cause:** Verification stopped at static checks because a live server was unavailable; therefore runtime MCP behavior was not demonstrated before rejection.

**Why this happened:** The development workflow did not include live MCP validation as a pre-review gate.

**Prevention:** Live MCP execution must be a required part of the review gate for any diagnostic tool that exposes internal state.

---

## Lockout-Compliant Action Items

**Aaron:** Revise the C++ MCP implementation/header and wiring.
- Use explicit public serialization/redaction for backend and model diagnostics
- Do not delegate the implementation back to Liebergot

**Liebergot:** Revise `test/server_mcp.py` only.
- Add recursive assertions for the rejected exposure classes
- Preserve the existing lifecycle/schema checks
- Do not revise the rejected C++ or documentation artifacts

**Kranz:** Revise `docs/api/mcp.md` only.
- Match the corrected public payload and prohibition contract
- Do not revise the rejected C++ or test artifacts

**Lovell:** Re-review the complete corrected set after Aaron, Liebergot, and Kranz submit.
- Lovell will not modify any implementation, test, or documentation artifact

---

## Process Decision for Future MCP Parity Work

For future MCP parity work, every externally visible diagnostic tool must:

1. **Have an explicit public field allowlist** defined BEFORE implementation
2. **Include recursive forbidden-data tests** that walk all nested objects and arrays
3. **Include documentation of the safe payload shape** before review
4. **Require live MCP execution** when the server is available

**Verification gate:** A compile-only or diff-only result cannot close a diagnostic security review. Live MCP must demonstrate actual safe behavior.

---

**Archived by:** Scribe (2026-07-30T10:27:18.631-06:00)
**Source:** `.squad/decisions.md`
