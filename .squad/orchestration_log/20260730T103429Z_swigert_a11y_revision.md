# Swigert — Phase-1 Presets A11y Test & Component Revision (Independent Scope)

**Session:** 2026-07-30T10:34:29.068-06:00
**Mode:** Background
**Role:** ⚛️ Accessibility

## Assignment Summary

Swigert is independently revising two rejected artifacts under focused accessibility scope only. Revisions address stale test helper assumptions and concurrent a11y compatibility issues, NOT phase-1 behavior defects.

## Revision Scope

### 1. `src/app/tests/a11y.spec.ts` — A76, A116, A117 Test Helper Updates

**Original artifact owner:** Kyle (legacy sections authored in 5f00291c and 2ce8fccf)
**Phase-1 artifact owner:** Haise (submitted with phase-1 test additions; locked out from revision)
**Revision authority:** Swigert (accessibility-test focused)

**A76 correction:**
- Current: Looks for Settings rail entry "App directory" (concurrent Apps extraction broke this)
- Corrected: Navigate via top-level `Apps` button → wait for `[data-view="apps"]` → inspect `.connect__marketplace-search` for label "Search marketplace apps"

**A116/A117 correction:**
- Current: Assumes README is default tab; README panel hidden due to Configuration-first phase-1 behavior
- Corrected: Explicitly click `README` tab before inspection; assert `aria-selected="true"` before waiting for `.detail__readme` element

### 2. `src/app/src/components/AppsView.tsx` — One-File A11y Escalation

**Original artifact owner:** Kyle (concurrent UI extraction in 4f5c3d84)
**Phase-1 disposition:** Out-of-scope production concurrent work
**Revision authority:** Swigert (one-file accessibility-only escalation; Mattingly locked out)

**Exact correction:**
- Restore canonical accessible name: `aria-label="Search marketplace apps"` on marketplace search input
- Do NOT re-add duplicate Settings rail entry
- Preserve top-level `Apps` route routing and marketplace placement

## Constraints

- Apps source (`src/app/src/components/AppsView.tsx`) revision authority limited to one-file accessibility compliance only
- No expanding scope beyond stated a11y corrections
- No expanding scope to other concurrent artifacts

## Status

IN PROGRESS — Swigert independently revising per assignment. Approval blocked pending completion and full `npm run test:a11y` pass.
