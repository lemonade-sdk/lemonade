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
