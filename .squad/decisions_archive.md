# Squad Decisions Archive

**Archive Policy:** Decisions dated before 2026-07-30T09:00:00Z are moved to this archive to maintain active decisions.md below the 15 KB summarization threshold. Archive is append-only.

**Archive created:** 2026-07-30T10:27:18.631-06:00 by Scribe
**Archival threshold:** 15 KB per session policy

---

## Archived Records

### 1. MCP Parity Phase-1 Rejection — CRITICAL

**Source:** `.squad/decisions.md` (archived from active file 2026-07-30T09:17:15.715Z)

**Full decision record:** Moved to `.squad/decisions/archive/lovell_rejection_2026_07_30T091715Z.md`

**Summary:**
- Verdict: REJECT (four blocking defects identified)
- Defects: Backend allowlist exposure, checkpoint path leakage, test coverage gap, documentation contract mismatch
- Lockout assignments: Aaron (C++), Liebergot (tests), Kranz (docs), Lovell (review)
- Minimal corrective acceptance criteria defined for each defect
- Status: Remediation cycle in progress (all agents completed 2026-07-30T10:12:56-57Z)

**Defects archived:**
1. Backend diagnostics leak forbidden fields (`action`, `release_url`)
2. Model diagnostics expose filesystem paths via `checkpoint` fields
3. Tests do not enforce security contract
4. Documentation does not match corrected public shape

---

### 2. MCP Parity Phase-1 Design Approval

**Source:** `.squad/decisions.md` (archived from active file 2026-07-30T09:52:57.950Z)

**Full decision record:** Moved to `.squad/decisions/archive/mcp_parity_phase_1_approval_2026_07_30.md`

**Summary:**
- Verdict: APPROVED (Phase-1 lifecycle parity design)
- Scope: MCP gateway code only (five canonical tools); no pull/delete/install/media phase
- Design decision: Reject not-downloaded models instead of auto-load callback
- Authorization: Limited exception for MCP gateway, tests, docs; no GUI/routes/presets
- Status: Governing approval for current revision cycle under strict lockout

---

### 3. MCP Parity Test Coverage Scope Approval

**Source:** `.squad/decisions.md` (archived from active file 2026-07-30T09:52:57.950Z)

**Full decision record:** Moved to `.squad/decisions/archive/test_scope_approval_2026_07_30.md`

**Summary:**
- Author: Haise (QA)
- Verdict: APPROVED (Python integration test scope)
- Scope: Locked to `test/server_mcp.py`; no production/GUI/preset changes
- Coverage: All ten tool schemas, structured lifecycle errors, safe diagnostic JSON, API key enforcement
- Discovery: Not-downloaded model discovery deferred until C++ implementation complete
- Status: Ready for C++ integration

---

### 4. MCP Parity Post-Rejection Retrospective

**Source:** `.squad/decisions.md` (archived from active file 2026-07-30T09:52:57.950Z)

**Full decision record:** Moved to `.squad/decisions/archive/lovell_retrospective_2026_07_30.md`

**Summary:**
- Facilitator: Lovell
- Scope: Root-cause analysis only; no artifact modifications
- Root causes identified:
  1. Diagnostic implementation exposed internal backend data directly (no explicit public schema)
  2. Model diagnostic serializers retained checkpoint fields without path-safety contract
  3. Test coverage validated shape but not security requirements
  4. Documentation covered operation semantics but not exact safe payload shape
  5. Verification stopped at static checks (live server unavailable)
- Process decision: Future diagnostic tools must have explicit field allowlists, recursive forbidden-data tests, and payload documentation
- Live MCP execution required when server available
- Status: Retrospective archived; governs future MCP work

---

## Archived Decisions Index

| Decision | Author | Date | Status | Archive File |
|----------|--------|------|--------|--------------|
| MCP Parity Phase-1 Rejection (CRITICAL) | Lovell | 2026-07-30T09:17:15.715Z | Archived with lockout assignments | `lovell_rejection_2026_07_30T091715Z.md` |
| MCP Parity Phase-1 Design Approval | Lovell | 2026-07-30T09:52:57.950Z | Archived (governing approval) | `mcp_parity_phase_1_approval_2026_07_30.md` |
| Test Coverage Scope Approval | Haise | 2026-07-30T09:52:57.950Z | Archived (test governance) | `test_scope_approval_2026_07_30.md` |
| Post-Rejection Retrospective | Lovell | 2026-07-30T09:52:57.950Z | Archived (process decision) | `lovell_retrospective_2026_07_30.md` |

---

## Active Decisions (Retained in decisions.md)

The following decisions remain active in `.squad/decisions.md`:

1. **MCP Parity Retrospective Process Decision** — Governs future diagnostic tool development (live MCP execution required, explicit field allowlists required)

---

**Archive maintained by:** Scribe
**Last updated:** 2026-07-30T10:27:18.631-06:00
**Archive integrity:** Append-only; external links to individual decision records in `.squad/decisions/archive/`
