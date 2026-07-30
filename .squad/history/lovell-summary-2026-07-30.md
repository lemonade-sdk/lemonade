# History Summary: Lovell (Lead Reviewer)

**File:** `.squad/agents/lovell/history.md`
**Size:** 16.9 KB
**Summary generated:** 2026-07-30T10:21:06.265-06:00

## Key Context

Lovell is the lead and primary reviewer with authority over scope decisions, code review, and rejection lockout policy. Role spans UI POC architectural decisions, preset/recipe terminology curation, MCP parity implementation review, and MCP revision cycle orchestration.

## Major Decisions & Milestones

### UI Framework Selection & POC Scope (2026-05-15)

- **Decision:** Rejected Svelte POC; sanctioned React decomposition POC instead
- **Rationale:**
  - Framework choice is secondary to component design problem
  - `ModelManager.tsx` is 75 KB god-component (architecture problem, not framework problem)
  - Svelte POC by strongest advocate still doesn't answer 18-month ROI question
  - Externalized cost of Debian packaging support for additional framework
  - Asymmetric reversibility: easy to start, hard to abandon
- **Outcome:** React refactor POC criteria established (N sub-components <10 KB each)
- **Reviewer authority enforced:** Auto-reject list on `src/app_next/` and `src/web_app_next/` until React POC delivers

### Preset/Recipe Terminology Architecture (2026-05-16 through 2026-05-31)

- **Terminology wall:** UI = preset (capability-keyed, client-side), C++ = recipe (backend engine ID)
- **Arch review:** v1.4 presets released within guardrails
  - Stored in localStorage only (invariant #11 defended)
  - No new API routes proposed (quad-prefix invariant #1 intact)
  - Import policy: rejects legacy presets without `applies_to` field
- **Capability risk:** `labelsFor()` in app.js used fallback heuristics; fixed in production requirement to read live labels only
- **Status:** Wall clean, no terminology leaks detected

### Bug Fix #1914 Review (2026-05-16)

- **Task:** Verified CLI command syntax for bug-report template
- **Finding:** CLI names changed (`lemonade recipes` → `lemonade backends`); templates had stale references
- **Pattern:** Issue templates and docs are last to reflect CLI renames — treat as critical path in deprecation reviews

### Tauri-Only API Integration Reviews (2026-06-13)

- **PR #2224 blocking issues identified:**
  1. Merge conflict after style refactor (#2223)
  2. Web-app incompatibility (Tauri-only imports in shared renderer)
  3. CI failures cascading from web-app build break
- **Key pattern enforced:** Any Tauri-native API in shared renderer must be gated behind `window.__TAURI__` or routed through `tauriShim.ts`
- **Repo constraint:** GitHub disallows merge commits; squash merge required

### MCP Tools UI Review & Revision Cycle (2026-07-30)

**Initial rejection (2026-07-30T09:17:15.715Z):**
- **Blocking defect:** `aria-required-children` violation in add-menu container
- **Root cause:** ChatView keeps `role="menu"` while replacing every menuitem with `role="dialog"` picker
- **WCAG compliance:** Critical 2.1 AA violation
- **Test gap:** 15 MCP/chat a11y tests passed but NONE ran axe scan with picker open
- **Lockout trigger:** Original author locked out; Swigert assigned for revision

**MCP Parity Implementation Review (2026-07-30T09:52:57Z):**
- **CRITICAL rejection** on three security defects:
  1. Backend `lemonade_list_backends` exposes `action` and `release_url` (admin controls, URLs)
  2. Model `lemonade_get_model_info` exposes filesystem paths via checkpoint fields
  3. Test assertions insufficient (shape checks only, no field redaction validation)
- **Documentation gap:** MCP contract does not define redacted payload shape or field prohibitions
- **Strict lockout policy issued:**
  - Aaron (Backend Integrator): C++ MCP implementation fix ONLY
  - Liebergot (C++ Server Core): Test redaction assertions ONLY
  - Kranz (Build & Release): Documentation contract update ONLY
  - Lovell: Final review on all three independently
- **Timeline:** Parallel remediations (10:12–10:13Z), orchestration logged (10:21:06Z)

## Critical Invariants Defended

1. **Invariant #11 (per-client state):** Presets are 100% localStorage; no `/presets` API endpoints
2. **Invariant #12 (Debian packaging):** Preset implementation required zero new npm modules
3. **Invariant #1 (quad-prefix routes):** No new preset endpoints proposed
4. **Quad-prefix registration:** Every new endpoint requires `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/` registration
5. **Web-app build compatibility:** Shared renderer must not import Tauri-only APIs without gating
6. **WCAG 2.1 AA compliance:** Minimum accessibility standard enforced on all UI changes
7. **Reviewer lockout policy:** Original rejected author cannot revise own work

## Reviewer Authority & Enforcement

- **Rejection stamps:** Authority to block code from merging (3 instances: MCP UI, MCP parity, MCP revision oversight)
- **Lockout mechanism:** When Lovell rejects, different agent is assigned to revise; Lovell re-reviews the fixed code
- **Scope authorization:** Kyle's request for MCP parity was a narrow exception to the older "lemond is OFF LIMITS" UI POC rule; exception does NOT authorize GUI changes, new routes, presets, or general server refactors
- **Cross-PR coordination:** Works across PR #2223 (styles refactor) and #2224 (settings UI) to enforce consistency

## Current Status (2026-07-30 10:21Z)

- **MCP revision cycle in progress:** Aaron/Liebergot/Kranz remediations COMPLETED
- **Final review pending:** Lovell ready to review all three components
- **Prerequisites met:** All orchestration records created, agent handoffs documented
- **Next step:** Lovell reviews C++ redaction, test assertions, and documentation updates

---

**Summary prepared by:** Scribe
**Full history available in:** `.squad/agents/lovell/history.md`

## Final Review Decision — MCP Parity Rejected & Hold Placed (2026-07-30T09:52:57.950-06:00)

**Verdict:** REJECTED on three critical defects
1. **Redaction incomplete:** safe_public_text() does not redact ordinary relative paths (e.g., models/foo.gguf) or bare URLs (e.g., github.com)
2. **Backend mismatch:** lemonade_load_model preflight checks first fallback while Router uses selected backend; allows bypass of unavailable backend, triggering install/download
3. **Scope contamination:** server.cpp includes unrelated runtime-route removal, commingling concerns

**Test Status:** Static checks passed; live parity testing blocked by stale running lemond process

**Reviewer Lockout — Exhausted:** All three MCP parity lockouts now active; next revisions must be by Mattingly (C++), Swigert (tests), Haise (docs)

**Merge Hold — Active:** No modifications to MCP code/tests/docs by other agents. Mattingly, Swigert, Haise own revisions; Lovell final authority.

## Current Status (2026-07-30T09:52:57.950-06:00)

- **MCP parity rejection finalized:** Three defects documented in Lovell's acceptance criteria
- **Inbox consolidated:** All 6 decision records moved to archive
- **Hold in place:** Merge blocked until all defects resolved
- **Next cycle:** Mattingly (C++), Swigert (tests), Haise (docs) independent revisions
- **Prerequisites for release:** Process cleanup, live MCP verification, concurrent regression checks, full Lovell re-review

**Stale Process Note:** lemond daemon must be terminated before live parity testing can proceed.

**Enforcement:** Lovell reviews all three revisions independently. No application code/test modifications permitted in MCP scope by non-assigned agents.

---

**Rejection finalized:** 2026-07-30T09:52:57.950-06:00
