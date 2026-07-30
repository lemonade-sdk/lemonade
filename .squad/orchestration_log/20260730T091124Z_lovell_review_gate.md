# Orchestration Entry: Lovell Review Gate — Swigert Revision

**Agent:** Lovell (Lead)  
**Mode:** Sync (gated review)  
**Trigger:** Swigert ChatView MCP picker revision submission  
**Created:** 2026-07-30T09:11:24.980-06:00  

## Role & Authority

**Lead Authority:**
- Owns scope and decisions
- Holds reviewer lockout on code changes
- If rejection occurs, different agent must produce revision

**Reviewer Status on `feat/ui-testing` branch:**
- No merges to `main` from this branch without lead approval
- Auto-reject list: any `src/app-next/` or `src/web-app-next/` trees (pending React decomposition POC verdict)
- Code review: blocking authority on accessibility, architecture, test coverage

## Review Gate Parameters

**Waiting for:** Swigert's submission of ChatView ARIA revision + test coverage extension

**Review checklist:**
- [ ] `aria-required-children` violation resolved (picker outside menu hierarchy)
- [ ] Dialog focus management (enter, trap, Escape, return)
- [ ] Axe accessibility scan passes with picker open
- [ ] Focus return test passing (original menu item regains focus on Back)
- [ ] External MCP entry test passing
- [ ] Reopen persistence test passing
- [ ] `npm run typecheck` passing
- [ ] Targeted A01/A187 tests passing (no regressions)
- [ ] No other code changes introduced (scope: ChatView a11y only)

**Escalation rules:**
- Rejection → Swigert revises (not original author)
- If Swigert cannot meet criteria → escalate to Mattingly (broader UI scope discussion)
- If architectural conflict → route to Kyle (project lead) for decision

## Standing Context

**Accessibility Directive (2026-06-14):** WCAG 2.1 AA non-negotiable for all UI work.

**Test Non-Negotiability (2026-06-14):** Broken tests block merge. Regressions fixed in-tree.

**Quad-Prefix Routing (Invariant #1):** No new endpoints. Component-only change.

**Reviewer Posture (2026-05-15):** Framework POC verdicts (React decomposition POC in progress; Svelte/Flutter verdicts deferred). Lovell auto-rejects off-scope UI trees.

## Status

**Standby since:** 2026-07-30T09:11:24.980-06:00  
**Waiting for:** Swigert revision submission  
**Expected review time:** Upon submission (sync gate)  

---
**Ledger entry:** Orchestration manifest 2026-07-30 spawn session
