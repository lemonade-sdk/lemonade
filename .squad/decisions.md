# Squad Decisions

## Active Decisions

### MCP Parity Post-Rejection Process Decision

**Date:** 2026-07-30T09:52:57.950-06:00
**Facilitator:** Lovell
**Type:** Process governance for future MCP parity work

## Process Rule for Future MCP Parity Work

For any externally visible diagnostic tool in future MCP phases:

1. **Explicit public field allowlist** must be defined BEFORE implementation
2. **Recursive forbidden-data tests** must validate that no prohibited fields appear at any nesting level
3. **Documentation of the safe payload shape** must be completed before review
4. **Live MCP execution is required** when the server is available; compile-only or diff-only results cannot close a diagnostic security review

## Reference

Complete MCP parity Phase-1 rejection record, design approval, test scope, and retrospective root-cause analysis are archived in:
- `.squad/decisions_archive.md` — Archive index
- `.squad/decisions/archive/lovell_rejection_2026_07_30T091715Z.md` — CRITICAL rejection with four defects
- `.squad/decisions/archive/mcp_parity_phase_1_approval_2026_07_30.md` — Approved design scope
- `.squad/decisions/archive/test_scope_approval_2026_07_30.md` — Approved test scope
- `.squad/decisions/archive/lovell_retrospective_2026_07_30.md` — Root-cause analysis

---

**Last updated:** 2026-07-30T10:27:18.631-06:00 (by Scribe)
**Archive policy:** Decisions >15 KB moved to `.squad/decisions/archive/` with index in `.squad/decisions_archive.md`
