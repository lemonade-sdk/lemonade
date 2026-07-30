# Phase 1 Presets Decoupling — Final Consolidated Decision Record

**Date:** 2026-07-30T11:50:42.664-06:00
**Consolidated by:** Scribe
**Final Verdict:** APPROVED

## Summary

Phase 1 Presets decoupling implementation is **approved and complete**. The preset-storage behavior, MCP independence, and model-configuration-first interactions are production-ready. All validation gates passed after three independent parallel repairs to unrelated pre-existing infrastructure defects.

## Phase 1 Scope (Approved)

### Product Behavior
- Per-model browser-local tuning is authoritative; active runtime uses stored preferences.
- Legacy default-sentinel records migrate losslessly and idempotently without driving active behavior.
- MCP enablement, server selection, and tool selection remain independent client-local state.
- Built-in Lemonade server is the default when no local MCP state exists.
- `change_preset` is absent from production runtime and rejected safely when invoked.
- Chat Tools picker, MCP panel contract, Model Configuration, and Effective Settings flows intact.
- Configuration-first model-detail presentation is intentional.

### Validation Results
| Command | Result |
|---------|--------|
| `npm run typecheck` | **PASS** |
| `npm run test:preset-intent` | **PASS** |
| `npm run test:mcp-runtime` | **PASS** |
| `npm run test:storage` | **PASS** |
| `npm run test:a11y` | **PASS — 145/145** |

## Out-of-Scope Infrastructure Repairs (Parallel, Independent)

### 1. Node Type Configuration — Kranz

**Artifact:** `src/app/src/api.ts` (`process.env.LEMONADE_BASE_URL` references)

**Issue:** Pre-existing `process` typings missing; introduced by commit `80a3b05b` (before Phase 1).

**Resolution:** Restored `node_modules/@types/node` via `npm install --ignore-scripts --no-audit --no-fund`. No `tsconfig.json`, `package.json`, or API behavior changes.

**Causality:** Unrelated to Phase 1; was a pre-existing build defect blocking typecheck completion.

### 2. Missing Chat History Module — Swigert

**Artifact:** `src/app/src/features/chatHistory/historySettings.ts`

**Issue:** `ChatView.tsx` and `ConnectView.tsx` import a module that did not exist; caused browser compile-overlay timeout and accessibility-test blockage.

**Resolution:** Restored module with three required exports:
- `CHAT_HISTORY_PREFERENCE_EVENT`
- `loadChatHistoryPreference`
- `saveChatHistoryPreference`

Uses existing `lemonade:persist_conversations` storage key. No Phase 1 code altered; no commits created.

**Causality:** Pre-existing concurrent dirty work; imports not in Phase 1 submission.

### 3. A187d Test Flow Cleanup — Swigert

**Artifact:** `src/app/tests/a11y.spec.ts`, test A187d (lines 2995–3036)

**Issue:** After "Back to add to chat options," the add-to-chat menu remains open by design. A187d immediately clicked the model button without dismissing the menu; open menu intercepted the pointer event and timed out.

**Resolution:** Added accessible menu-dismissal step via existing trigger. Asserts menu is detached and `aria-expanded="false"` before model selection and after second Back. Preserves all localStorage persistence and MCP server/tool selection assertions.

**Scope Boundary:** Test-flow only; no `ChatView.tsx`, `McpPanel.tsx`, or MCP behavior changes.

**Result:** A187d and full `npm run test:a11y` suite pass (145/145).

## Lockout Compliance

- **Haise:** Locked out from artifact revision; not involved in this final approval.
- **Mattingly:** Locked out from rejected/reassigned artifacts; Phase 1 product implementation approved as submitted.
- **Lovell:** Final review completed; no product code or tests modified.

## Out-of-Phase Constraints

The following pre-existing work remains **intentionally untouched**:
- `src/app/src/components/McpPanel.tsx` — MCP Phase A contract (read-only under Phase 1 boundary).
- Server `/mcp` gateway, logs, telemetry, and backend changes — explicitly out of Phase 1 scope.
- Apps feature implementation — concurrent work not included in this review.

Any MCP-panel or server-side MCP changes require separate authorized MCP review.

## Final Status

**Phase 1 Presets Decoupling: COMPLETE AND APPROVED for merge.**

All validation commands pass. Three independent parallel repairs to unrelated pre-existing infrastructure are complete and do not alter Phase 1 product behavior. Ready for staging and commit.
