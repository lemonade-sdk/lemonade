# Lovell — Phase-1 Presets Decoupling Review — Rejection & Lockout Assignment

**Session:** 2026-07-30T10:34:29.068-06:00
**Mode:** Sync (Review Gate)
**Role:** 🏗️ Lead Reviewer

## Review Verdict

**REJECTED** — Phase-1 behavior implementation is correct and passes all production tests. Accessibility gate failed due to three pre-existing legacy test failures caused by concurrent unrelated changes and stale helper assumptions, not phase-1 code. Phase-1 remains conditionally eligible for approval once a11y artifacts are corrected and full command passes.

## Phase-1 Assessment (Passed)

✓ Direct `modelName` tuning is active source; legacy defaults migrate losslessly and remain idempotent
✓ Active Model Configuration, Effective Settings, Chat, API composition, and tool runtime do NOT consume preset/default-sentinel state
✓ MCP enablement, server selection, and tool selection are client-local independent
✓ `change_preset` is absent; `load_model` retains explicit load options
✓ Chat tools picker and MCP interaction remain intact

## Production Validation (All Passed)

✓ `npm run typecheck`
✓ `npm run test:preset-intent`
✓ `npm run test:mcp-runtime`
✓ `npm run test:storage`
✓ Targeted picker and modal accessibility coverage (A187)

## Accessibility Failures (3 of 145 tests)

| Test | Failure | Root Cause | Phase-1 Status |
|------|---------|-----------|----------------|
| A76 | Settings rail missing "App directory"; marketplace moved to top-level (concurrent) | Pre-existing concurrent Apps extraction (4f5c3d84) | Not phase-1 defect; conditional eligibility |
| A116 | README panel hidden; phase-1 changed model details to Configuration-first default (8f5a9688, 595c98a9) | Intentional phase-1 behavior; stale test helper | Not a defect; conditional eligibility; intended behavior |
| A117 | Same hidden README panel as A116 | Same intentional Configuration-first change | Same conditional status as A116 |

## Lockout Assignments — Independent Revisions Required

**1. `src/app/tests/a11y.spec.ts` — Swigert (accessibility-test focused)**
   - A76 revision: Navigate via top-level `Apps` button → `[data-view="apps"]` → `.connect__marketplace-search`
   - A116/A117 revision: Explicitly click `README` tab, assert `aria-selected="true"` before waiting for `.detail__readme`
   - Haise locked out from this artifact

**2. `src/app/src/components/AppsView.tsx` — Swigert (one-file accessibility-only escalation)**
   - Restore canonical accessible name: `aria-label="Search marketplace apps"`
   - Do NOT re-add duplicate Settings rail entry
   - Mattingly locked out from revising this concurrent artifact

**3. `src/app/src/components/ModelDetailPanel.tsx` — NO REVISION NEEDED**
   - Causal component for A116/A117 but Configuration-first default is intended phase-1 behavior
   - Do not revert merely to satisfy stale helper

## Conditional Approval Boundary

Phase-1 Presets decoupling remains eligible for conditional approval once:
1. Swigert completes independent a11y test and component revisions
2. Apps-source exception decision is made
3. Full `npm run test:a11y` command passes
4. No concurrent regressions introduced

## Status

APPROVAL BLOCKED — Awaiting Swigert's independent revision completion and Apps-source exception decision.
