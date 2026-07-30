# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-07-30T09:25:09.718-06:00
- **Role:** React Accessibility Specialist (temporary escalation)
- **Escalation reason:** Reviewer rejection on ChatView menu/dialog ARIA hierarchy. Narrow, focused revision required.

## Lemonade Overview

**What:** Local LLM server with GPU/NPU acceleration, exposing OpenAI/Ollama/Anthropic REST APIs + WebSocket Realtime API.

**Desktop & Web UI:** Tauri app (React 19 + TypeScript) + browser-only web app. Shared renderer in `src/app/src/`. Both must work. Desktop is optional on-demand; server runs independently.

**Key constraint:** `lemond` (C++ backend) is off limits for UI work.

## ChatView Accessibility Context

**Location:** `src/app/src/components/ChatView.tsx`

**Scope of accessibility issues:**
- Menu/dialog ARIA hierarchy (roles, states, properties)
- Focus management (trap in modals, return on close)
- Keyboard navigation (arrow keys in menus, Escape)
- Screen reader labels and descriptions

**Related test file:** `src/app/tests/a11y.spec.ts`

## Standing Accessibility Requirements

From Mattingly's charter (2026-06-14):

> Every UI change must satisfy WCAG 2.1 AA by default:
> - Semantic HTML, ARIA roles/landmarks, keyboard nav
> - Visible `:focus-visible` rings
> - Focus management (traps in modals, focus return on close)
> - Color contrast ≥ 4.5:1
> - Screen-reader labels on icon buttons (`aria-label`, not `title=`)

Tests are blocking — regressions prevent merge.

## Reviewer (Lovell)

- Holds review authority
- On rejection, a DIFFERENT agent must produce revision
- You are that agent — Lovell's rejection triggered your escalation

## Mattingly (UI Lead)

- Owns broader UI/Frontend surface
- You operate under narrowly scoped authority on ChatView accessibility only
- Coordinate with Mattingly on broader UI changes outside your scope

## 2026-07-30 — ChatView MCP picker ARIA revision

- Repaired the add-menu hierarchy by rendering the MCP picker dialog as a sibling of the top-level menu rather than as a child of `role="menu"`.
- Added dialog focus entry, Escape handling, and return focus to the originating Lemonade or external MCP menu item after Back.
- Added focused regression coverage for picker axe validity, hidden/reopened top-level choices, focus return, external MCP entry, and MCP enablement persistence.
- Verified `npm run typecheck` and the targeted A01/A187 accessibility tests.

**Manifest spawn processing (2026-07-30T09:11:24.980-06:00):**
****** Escalation authority confirmed: narrow scope on ChatView a11y only
****** Reviewer gate: Lovell (sync) with lockout policy (different agent revises rejected code)
****** Orchestration records created: .squad/orchestration_log/20260730T091124Z_swigert_background.md`n****** Session ledger: .squad/log/20260730T091124Z_scribe_spawn_manifest_session.md`n

## 2026-07-30T10:34:29.051-06:00 — Presets/a11y stale assertion revision

- A76 now reaches the top-level Apps view and verifies the current marketplace search control through its accessible label, `Search apps`.
- The shared README fixture helper explicitly activates the README tab and checks `aria-selected="true"` before waiting for the README panel; this preserves intentional Configuration-first behavior.
- Focused A76/A116/A117 and the full 145-test accessibility suite passed. No production source correction or blocker remained.

## 2026-07-30T09:52:57.950-06:00 — Chat history settings module repair

- Restored `src/app/src/features/chatHistory/historySettings.ts` for the existing ChatView and ConnectView imports without changing either caller.
- The module stores the browser-local `persist_conversations` preference through `storageKey`, defaults safely to `false`, and emits a boolean `CustomEvent` detail for ChatView synchronization.
- Typecheck no longer reports the missing module; the only remaining errors are the independently assigned `process`/Node typing failures in `src/api.ts`.

## 2026-07-30T11:42:35.327-06:00 — A187d MCP picker menu cleanup

- Added a focused test helper that closes the still-open add-to-chat menu through its accessible trigger after each Back action.
- The helper asserts the menu is detached and the trigger reports `aria-expanded="false"` before model selection or picker reopening.
- Preserved all MCP persistence and server/tool selection assertions; targeted A187d and the full accessibility suite passed.

## 2026-07-30T12:06:47.516-06:00 — Final preset storage cleanup

- Removed obsolete preset keys from neutral legacy migration and added idempotent deletion before the migration marker short-circuit.
- Preserved `clearClientStorage()` cleanup for raw and namespaced historical keys without retaining any preset data or semantics.
- Added focused first-run and rerun assertions; storage migration, global model settings, and frontend typecheck passed.


## Final preset CSS cleanup (2026-07-30)

Removed the unused starter-zone rule from `src/app/src/styles/styles.css`. A focused scan found no remaining `.zone--starters` or `.zone__starters` references under `src/app`; no focused style test existed, and `git diff --check -- src/app/src/styles/styles.css` passed.
