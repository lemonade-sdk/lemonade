# PR Review Session — 2026-06-13

**Reviewer:** Lovell (Lead/Architect)
**Author:** boclifton-MSFT
**Target branch:** kpoin/ui-testing

## PR #2223 — Refactor app styles into component partials

**Verdict:** ✅ Approved and merged (squash)

- Split monolithic `styles.css` (7594 lines) into 22 focused partials under `src/app/styles/partials/`
- Import order in `index.css` is well-documented: foundation (variables, base) → components (alphabetical, order-independent) → feature sheet (last, for overrides)
- Net +72 lines (comments in index.css + .hintrc config)
- No selectors renamed or dropped
- No CI configured for this branch (expected)
- `.hintrc` addition is reasonable (CSS compat linting config)

## PR #2224 — Add model folders UI (Server Settings)

**Verdict:** ⚠️ Changes requested (not merged)

### Positives
- Well-structured `ServerSettings.tsx` and `serverRuntimeConfig.ts`
- Correct architectural decision: uses `/internal/config` and `/internal/set` (server-wide config via HTTP, not localStorage) — aligns with invariant #11
- Clean auth/error handling with distinct unauthorized state
- No src/cpp changes

### Blocking Issues
1. **Merge conflict:** PR #2223 deleted `styles.css`; this PR adds to it. Needs rebase.
2. **Web-app incompatibility:** Direct import of `@tauri-apps/plugin-dialog` in shared renderer code will break the web-app build. Needs a shim/guard.
3. **CI failures:** .deb, .rpm, macOS .dmg, Windows embeddable all failed — likely related to #2.

### Non-blocking
- PR title says "model folders" but content is "server settings for model directories" — suggest rename for clarity.
