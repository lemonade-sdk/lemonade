# Orchestration Entry — MCP Parity Final Rejection Handoff

**Timestamp:** 2026-07-30T09:52:57.950-06:00
**Logger:** Project Session Scribe
**Cycle:** MCP Parity Revision Rejection & Hold
**Status:** HOLD — Merge blocked; no application code/tests/docs modifications permitted

## Review Summary

**Reviewer:** Lovell
**Verdict:** REJECTED — Three critical defects remain unresolved

### Defect 1: Unsafe Path & URL Redaction

**Issue:** `safe_public_text()` does not redact ordinary relative paths (e.g., `models/foo.gguf`) or bare URL-like values (e.g., `github.com`).

**Impact:** Diagnostic MCP payloads from `lemonade_get_model_info`, `lemonade_get_server_info`, and `lemonade_list_backends` may expose path-bearing and URL-like values despite claimed recursive redaction.

**Status:** Unresolved — Implementation does not apply uniform protection across all nested diagnostic fields and allowlisted scalar values.

### Defect 2: Backend Selection Mismatch in Load Preflight

**Issue:** `lemonade_load_model` preflight checks only `SystemInfo::get_supported_backends(info.recipe).front()`, while Router independently resolves effective backend from model and server configuration. An installed fallback can mask an unavailable selected backend that Router will actually use.

**Impact:** Load request may proceed to invoke backend installation, model download, or executable download despite selected backend being unavailable.

**Status:** Unresolved — Preflight and Router do not use shared backend-selection logic.

### Defect 3: Unrelated Runtime-Route Removal in server.cpp

**Issue:** `server.cpp` includes a runtime-route removal change unrelated to MCP parity, introducing unnecessary risk and violating scope.

**Impact:** Merge scope is contaminated; change cannot be validated as MCP-specific.

**Status:** Unrelated to MCP parity — must be split into separate review.

## Test & Build Status

**Static checks:** ✓ PASSED
- C++ compilation: successful
- Python test syntax and Black formatting: successful
- Documentation drift checks: successful

**Live parity testing:** ⚠️ BLOCKED
- Stale running `lemond` process prevented full link validation
- Process cleanup required before live MCP response redaction verification can proceed

## Reviewer Lockout — Exhausted

All three MCP parity reviewer lockouts are now **exhausted**:

| Scope | Locked-Out Agents | Assigned Owner | Status |
|-------|-------------------|-----------------|--------|
| C++ Implementation | Liebergot, Aaron, Haise | Mattingly | Awaiting revision |
| Tests | Haise, Liebergot, Kranz | Swigert | Awaiting revision |
| Documentation | Liebergot, Kranz, Aaron | Haise | Awaiting revision |

**Lovell** remains reviewer with final authority and must not modify implementation, tests, or documentation while enforcing this review.

## Merge Hold — Active

**Status:** HOLD IN PLACE

**Conditions for Merge Release:**
1. All three Lovell-specified defects resolved in Mattingly (C++), Swigert (tests), Haise (docs) revisions
2. Live MCP response verification after process cleanup
3. No concurrent regressions in phase-1 presets or GUI accessibility
4. All four acceptance criteria met (backend validation, path/URL redaction, test behavior, docs accuracy)

**Prohibited Actions While Hold is Active:**
- ✗ No modifications to MCP C++ code (except by Mattingly)
- ✗ No modifications to MCP test code (except by Swigert)
- ✗ No modifications to MCP documentation (except by Haise)
- ✗ No merge authorization without full Lovell review gate completion
- ✗ No application code or test modifications by other agents in MCP scope

## Inbox Consolidation

**Action Taken:** All 6 inbox decision records moved to archive:
- `aaron_mcp_actual_serialization_tests.md` → archive/
- `haise_mcp_exact_docs.md` → archive/
- `kranz_mcp_all_backend_preflight.md` → archive/
- `lovell_mcp_third_retrospective.md` → archive/
- `lovell_mcp_unavailable_owner_reassignment.md` → archive/
- `swigert_presets_a11y_revision.md` → archive/

**Rationale:** Inbox records represent rejected revision attempts. Moving to archive freezes the rejection decision and prepares for next-cycle assignments to Mattingly, Swigert, and Haise.

## Session Summary

**Cycle Duration:** MCP parity revision cycle (2026-07-30, 09:52:57Z to present)

**Parallel Work Completed:**
- Haise: MCP no-install guard (rejected — backend mismatch)
- Aaron: MCP redaction safeguards (rejected — incomplete redaction)
- Kranz: Test enhancements and docs (rejected — test evidence insufficient, docs overstate implementation)
- Lovell: Final review gate (verdict: 3 defects, hold merge)

**Outcome:** MCP parity remains eligible for merge only after three independent revisions (Mattingly C++, Swigert tests, Haise docs) and full re-review by Lovell. Stale running `lemond` must be terminated for live parity verification.

**No escalations beyond expected process cleanup and scope lockout enforcement.**

---

*Session logged by Project Scribe. Lovell's final acceptance criteria recorded in `/squad/decisions/archive/lovell_mcp_third_retrospective.md`.*
