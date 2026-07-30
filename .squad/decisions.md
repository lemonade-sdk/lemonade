# Squad Decisions

## Active Decisions

### Phase-1 Presets Decoupling Review — Rejection & Lockout Assignment

**Date:** 2026-07-30T10:18:36.054-06:00
**Reviewer:** Lovell
**Verdict:** REJECTED — Phase behavior implementation passed all production tests, but three legacy accessibility tests fail due to pre-existing unrelated concurrent changes and stale helper assumptions.

#### Phase-1 Assessment (Passed)
- Direct `modelName` tuning is active source; legacy defaults migrate losslessly and remain idempotent
- Active Model Configuration, Effective Settings, Chat, API composition, and tool runtime do NOT consume preset/default-sentinel state
- MCP enablement, server selection, and tool selection are client-local independent; built-in Lemonade used only when no MCP state exists
- `change_preset` is absent; `load_model` retains explicit load options
- Chat tools picker and MCP interaction remain intact; targeted picker and modal accessibility coverage passed
- Validation: `npm run typecheck`, `npm run test:preset-intent`, `npm run test:mcp-runtime`, `npm run test:storage` — all passed

#### Failed Accessibility Tests (3 of 145)
| Test | Failure | Root Cause | Phase-1 Status | Locked-Out Artifact |
|------|---------|-----------|----------------|-------------------|
| A76 | Settings rail has no "App directory" entry; marketplace now at top-level Apps (concurrent) | Pre-existing concurrent Apps extraction (4f5c3d84) not caused by phase-1 | Conditional: phase-1 eligible after concurrent fix | `src/app/tests/a11y.spec.ts` (Haise locked out) |
| A116 | README panel hidden; phase-1 changed model details from README-first to Configuration-first (8f5a9688, 595c98a9, current) | Mattingly's intentional Configuration-first default phase-1 behavior; test helper assumes README default | Conditional: intended behavior, not a defect; must update stale helper | `src/app/tests/a11y.spec.ts` (Haise locked out) |
| A117 | Same hidden README panel and timeout as A116 | Same intentional Configuration-first change; Haise's new A187 coverage passed | Conditional: same as A116 | `src/app/tests/a11y.spec.ts` (Haise locked out) |

#### Lockout Assignments — Independent Revisions Required
1. **`src/app/tests/a11y.spec.ts`** — A76, A116, A117 revisions by **Swigert** (accessibility-test focused only)
   - A76: Navigate via top-level `Apps` button → `[data-view="apps"]` → `.connect__marketplace-search`; do not look for Settings entry
   - A116/A117: Explicitly click `README` tab, assert `aria-selected="true"` before waiting for `.detail__readme`; do not assume README default

2. **`src/app/src/components/AppsView.tsx`** — A76 compatibility by **Swigert** (one-file accessibility-only escalation)
   - Restore canonical accessible name: `aria-label="Search marketplace apps"` to preserve accessibility contract
   - Do NOT re-add duplicate Settings rail entry

3. **`src/app/src/components/ModelDetailPanel.tsx`** — NO REVISION (causal component for A116/A117 but Configuration-first default is intended phase-1 behavior)

#### Conditional Approval Boundary
Once Swigert completes independent a11y test/component revisions and concurrent Apps accessibility compatibility is verified, phase-1 preset implementation remains eligible for conditional approval. Full `npm run test:a11y` command must pass before merge approval.

---

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

### MCP Parity Final Rejection — Three Critical Defects (2026-07-30T09:52:57.950-06:00)

**Date:** 2026-07-30T09:52:57.950-06:00
**Reviewer:** Lovell
**Verdict:** REJECTED — Three critical defects documented with formal acceptance criteria

#### Defect 1: Unsafe Path & URL Redaction in Diagnostic Tools

**Issue:** safe_public_text() does not redact ordinary relative paths (e.g., models/foo.gguf) or bare URL-like values (e.g., github.com).

**Impact:** Diagnostic MCP payloads may expose path-bearing and URL-like values despite claimed recursive redaction.

**Acceptance Criteria:**
- One complete safety rule must apply to every string value emitted by lemonade_get_model_info, lemonade_get_server_info, lemonade_list_backends, recursively through every object and array
- Rule must reject or replace scheme URLs, bare URLs, host names, absolute paths, Windows paths, dot-relative paths, and ordinary relative paths (e.g., models/foo.gguf)
- No original path or URL value may remain in any nested MCP text block or serialized diagnostic object
- All allowlisted string fields must be protected, not only backend messages

#### Defect 2: Backend Selection Mismatch in Load Preflight

**Issue:** lemonade_load_model preflight checks SystemInfo::get_supported_backends(info.recipe).front(), while Router independently resolves effective backend from configuration and model options. An installed fallback can mask an unavailable selected backend.

**Impact:** Load request may proceed to invoke backend installation, executable download, or model download despite selected backend being unavailable.

**Acceptance Criteria:**
- Preflight must resolve backend selection using the same effective model and server configuration that Router::load_model uses
- Every configured or model-selected backend must be validated before Router::load_model is called
- Missing or unavailable selected backend must return MCP tool error before loading begins
- Rejected request must not invoke installation, download, fallback substitution, or configuration mutation
- Test must configure unavailable selected backend while otherwise supported fallback is available; verify error, unchanged status, unchanged download state
- Test must cover model-level selection path as well as server-level configured selection path

#### Defect 3: Unrelated Runtime-Route Removal in server.cpp

**Issue:** server.cpp includes unrelated runtime-route removal change, contaminating MCP parity scope.

**Impact:** Merge scope is contaminated; change cannot be validated as MCP-specific.

**Acceptance Criteria:**
- Split unrelated change into separate review
- MCP scope limited to diagnostic tool redaction and backend selection validation only

#### Test & Build Status

- **Static checks:** ✓ PASSED (C++ compilation, Python syntax, Black formatting, drift checks)
- **Live parity testing:** ⚠️ BLOCKED (stale running lemond process prevented full verification)

#### Reviewer Lockout — Exhausted

| Scope | Locked-Out Agents | Assigned Owner | Status |
|-------|-------------------|-----------------|--------|
| C++ Implementation | Liebergot, Aaron, Haise | Mattingly | Awaiting revision |
| Tests | Haise, Liebergot, Kranz | Swigert | Awaiting revision |
| Documentation | Liebergot, Kranz, Aaron | Haise | Awaiting revision |

**Lovell** remains reviewer with final authority and must not modify code while enforcing this review.

#### Merge Hold — Active

**Status:** HOLD IN PLACE

**Prohibited Actions:**
- ✗ No modifications to MCP C++ code (except by Mattingly)
- ✗ No modifications to MCP test code (except by Swigert)
- ✗ No modifications to MCP documentation (except by Haise)
- ✗ No merge authorization without full Lovell review gate
- ✗ No application code or test modifications by other agents in MCP scope

**Prerequisites for Merge Release:**
1. All three defects resolved in independent revisions (Mattingly C++, Swigert tests, Haise docs)
2. Live MCP response verification after process cleanup (stale lemond process must be terminated)
3. No concurrent regressions in phase-1 presets or GUI accessibility
4. Full Lovell re-review and approval of all three revisions

#### Inbox Consolidation

**Action:** All 6 inbox decision records moved to archive:
- aron_mcp_actual_serialization_tests.md → archive/
- haise_mcp_exact_docs.md → archive/
- kranz_mcp_all_backend_preflight.md → archive/
- lovell_mcp_third_retrospective.md → archive/
- lovell_mcp_unavailable_owner_reassignment.md → archive/
- swigert_presets_a11y_revision.md → archive/

**Rationale:** Records represent rejected revision attempts; moving to archive freezes decision and prepares for next-cycle assignments.

#### Next Cycle

Mattingly, Swigert, and Haise own independent revisions addressing all acceptance criteria. Lovell will conduct final consolidated re-review. Process cleanup required before live parity verification can proceed.
