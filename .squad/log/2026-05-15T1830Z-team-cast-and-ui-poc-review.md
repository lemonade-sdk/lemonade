# Session log — Team cast & UI framework POC review

**Timestamp:** 2026-05-15T18:30:00Z
**Requested by:** Kyle Poineal
**Branch:** `feat/ui-testing`

## What happened

1. **Team scaffolded (Init Phase 2).** Created agent folders, charters, and history files for Lovell (Lead), Liebergot (C++ Server Core), Aaron (?), Kranz (Build & Release), Haise (?), Mattingly (UI / Frontend). Roster written to `.squad/team.md`; routing rules to `.squad/routing.md`; Apollo 13 universe registered in `.squad/casting/policy.json` with members allocated in `.squad/casting/registry.json` and the assignment snapshot in `.squad/casting/history.json`.
2. **Git exclusions tightened.** All Squad-related ignores moved to `.git/info/exclude` (local-only). Removed Squad lines from the committed `.gitignore`. Reverted Squad `merge=union` lines from `.gitattributes`. Squad working files are invisible to upstream.
3. **Working branch.** `feat/ui-testing` created off `main` for the UI POC. No merges to `main` permitted under current directive.
4. **Directive captured.** Kyle's ground rules logged to `.squad/decisions/inbox/copilot-directive-20260515-rules.md` and merged into `decisions.md` this session.
5. **Multi-agent UI framework review.** Four agents ran in parallel (background fan-out):
   - **Mattingly** (UI/Frontend, primary): recommends **Svelte 4 + webpack + svelte-loader** on ONE panel side-by-side; POC trees `src/app-next/` + `src/web-app-next/`.
   - **Lovell** (Lead, architecture): additive topology with separate desktop binary (`com.amd.lemonade-app-next`); reviewer policy with auto-reject and Kranz-consult lists; seven-item "done enough" bar.
   - **Kranz** (Build & Release, packaging): React ✅; Svelte vanilla ⚠️ (needs Debian trixie verification); SvelteKit ❌; Flutter Web ⚠️ (pre-build only); Flutter Desktop ❌ (breaks Tauri embedding).
   - **Liebergot** (C++ Server Core, contract): canonical UI ↔ `lemond` contract — REST quad-prefix inventory, WebSocket Realtime (read `websocket_port` from `/health`), `window.api` shim contract, auth/CORS rules, MUST-NOT fence-posts.

## Key decisions merged into `decisions.md`

- Project working rules (no `main` merges; UI POC scope; `lemond` off-limits; Squad files invisible to upstream).
- UI POC framework recommendation (Svelte 4 + webpack, side-by-side, one panel).
- UI POC architecture & reviewer policy on `feat/ui-testing`.
- UI framework packaging verdict (Debian-driven framework feasibility table).
- UI ↔ `lemond` contract (immutable surface document).

## Open questions outstanding (waiting on Kyle)

1. Svelte 4 (Debian-friendly) vs Svelte 5 / runes (requires bundle vendoring).
2. Which specific panel to port first (Mattingly suggested `TranscriptionPanel` or `EmbeddingPanel`).
3. Coexistence horizon between React and Svelte renderers.
4. Whether to mirror existing hooks 1:1 as Svelte stores or redesign them during port.
5. (Per Mattingly's recommendation memo — full list pending Kyle's response.)

## Next likely actions

- On Kyle's answers: Mattingly scaffolds `src/app-next/` + `src/web-app-next/` with the chosen panel.
- Kranz consult triggered by first `package.json` / CMake additions.
- Lovell reviews first PR against the auto-reject and consult-required lists.
