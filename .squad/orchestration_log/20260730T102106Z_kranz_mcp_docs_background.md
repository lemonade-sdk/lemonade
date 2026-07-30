# Orchestration Log — Kranz MCP Documentation Redaction

**Agent:** Kranz (Build & Release)
**Task ID:** MCP documentation contract update
**Mode:** background
**Timestamp:** 2026-07-30T10:21:06.265-06:00
**Status:** COMPLETED

## Assignment

**Trigger:** Lovell's 2026-07-30T09:17:15.715Z documentation contract mismatch.

**Blocking defects:**
1. `docs/api/mcp.md:206–217` describes diagnostics as read-only but does not define redacted backend payload
2. No explicit prohibition on control/URL/path fields
3. Contract must match the corrected C++ public shape

**Locked scope:** Kranz ONLY — Liebergot and Haise are BLOCKED from MCP documentation.

## Remediation Completed

**Outcome:** ✓ Independently remediated rejected MCP documentation contract; diff/contract checks passed.

**Changes:**
- Named all five canonical lifecycle/diagnostic tools in contract
- Defined public payload allowlists (backend: `recipe`, `name`, `state`, `message`, `version`; model info: all except checkpoints/paths)
- Explicitly excluded backend controls and release URLs, checkpoint/path values, PIDs, backend URLs, credentials, and internal/admin process controls
- Added security principle: "Read-only does not mean unfiltered; all outputs are redacted to expose only diagnostic state, never admin/filesystem/credential information."

**Files updated:**
- `docs/api/mcp.md` — backend/model tool payload specifications, security principles, field prohibition list

**Validation:**
- ✓ Lightweight contract assertions passed
- ✓ `git diff --check` passed (no trailing whitespace, line endings valid)

## Next Step

**Dependent:** Aaron's C++ fix + Liebergot's test enhancements must merge. Documentation provides the canonical contract for external MCP clients.

---
**Session:** Scribe session 2026-07-30T10:21:06.265-06:00
