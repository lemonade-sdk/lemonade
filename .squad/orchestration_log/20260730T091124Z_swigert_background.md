# Orchestration Entry: Swigert ChatView MCP Picker ARIA Revision

**Agent:** Swigert (React Accessibility Specialist)  
**Mode:** Background (autonomous)  
**Trigger:** Lovell rejection (2026-07-30T09:17:15Z) — critical `aria-required-children` violation  
**Created:** 2026-07-30T09:11:24.980-06:00  

## Task Scope

**File:** `src/app/src/components/ChatView.tsx`  
**Lines:** ~3715 (add-menu container), ~3775 (MCP picker dialog)

**Problem:** Picker rendered as nested `role="dialog"` inside `role="menu"` container → WCAG 2.1 AA violation

**Deliverable:**
1. Move MCP picker dialog outside menu hierarchy (sibling rendering)
2. Add dialog focus entry/exit lifecycle
3. Implement Escape key handling
4. Return focus to originating menu item on Back
5. Extend test coverage: axe + picker open, focus return, reopen persistence, external MCP entry
6. Verify `npm run typecheck` passes
7. Verify targeted A01/A187 accessibility tests pass

**Authority:** Narrow scope (ChatView a11y only). Coordinate with Mattingly on broader UI changes.

## Reviewer Gate

**Reviewer:** Lovell (sync)  
**Lockout Policy:** Lovell's rejection triggers escalation; original author (Mattingly) cannot revise. Swigert must implement fix independently.

**Re-Review Criteria:**
- ✓ ARIA hierarchy valid (no `aria-required-children` violation)
- ✓ Axe scans pass with picker open
- ✓ Focus management tests pass (enter, trap, exit, return)
- ✓ Typecheck passes
- ✓ No test regressions

## Standing Context

**Accessibility Directive (2026-06-14):** Every PR/feature must satisfy WCAG 2.1 AA + LLM-specific a11y (aria-live, response verbosity, high-contrast, dyslexia-friendly font, keyboard shortcuts).

**Test Non-Negotiability (2026-06-14):** Broken tests ship nothing. Regressions fixed in same commit/PR.

**Quad-Prefix Routing (Invariant #1):** n/a for this task (component-only change).

## Status

**Started:** 2026-07-30T09:11:24.980-06:00  
**Status:** In-progress  
**Expected completion:** After Lovell approval gate (TBD)  

---
**Ledger entry:** Orchestration manifest 2026-07-30 spawn session
