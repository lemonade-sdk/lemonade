# 2026-05-16T20:00:00Z — Kranz: PR #1914 review-feedback fixes

**Agent:** Kranz (Build & Release)
**Mode:** sync (single-agent cycle)
**Requested by:** Kyle Poineal
**Routing rationale:** PR feedback addressed CLI command syntax in `.github/ISSUE_TEMPLATE/bug-report.yml`. Owner of release / packaging / CLI surface artifacts → Kranz. No code change, just template strings; no need for Liebergot (server core) to drive — but Liebergot's domain (CLI source) was consulted as the source of truth for verification.

## Why this agent

Three inline review comments from @jeremyfowers (MEMBER, external maintainer) on PR #1914:

- Line 75: `lemond` step is redundant — server auto-starts. *"Could just remove this"* OR document platform-specific restart steps.
- Line 103: Replace `lemond --log-level debug` with the runtime config setter; no restart needed.
- Line 115: `lemonade recipes` is the wrong command — should be `lemonade backends`.

All three are CLI-surface accuracy fixes on a contributor-facing template. Kranz owns this surface.

## Files authorized to read

- `.github/ISSUE_TEMPLATE/bug-report.yml` (target)
- `src/cpp/cli/main.cpp` (CLI subcommand definitions — verification source)
- `src/cpp/server/runtime_config.cpp` (runtime config keys — verification source)
- `src/cpp/server/config_file.cpp` (config schema — verification source)
- `src/cpp/server/logging_config.cpp` (hot-reconfigure path — verification source)
- PR #1914 review thread (via `gh pr view 1914`)

## Files produced / modified

- `.github/ISSUE_TEMPLATE/bug-report.yml` — three edits matching the three review comments (commit `976a8260`, pushed to `origin/fix/1885-bug-report-template-commands`).
- `.squad/decisions/inbox/kranz-pr-1914-review-fixes.md` — decision drop (now merged into `.squad/decisions.md`).

## Outcome

✅ All three review comments addressed in a single commit. Source citations recorded in the decision entry for future doc/CLI work. YAML re-validated post-edit. PR branch pushed; CHANGES_REQUESTED awaits maintainer re-review.

**Judgment call recorded:** On comment 1, Kranz chose Jeremy's "could just remove this" lean over the alternative of adding platform-specific restart docs. Rationale documented; potential follow-up flagged (a `docs/server/` "Restarting the server" page) but explicitly NOT required to land #1914.

**Open coordinator task:** Kyle deciding whether to (a) drop a confirmation comment on PR + resolve threads, (b) leave for GitHub-side handling, (c) file follow-up for restart docs.

## Notes

- No `.squad/` files staged on the PR branch — `.git/info/exclude` honored per project working rules (decisions.md entry of 2026-05-15T18:00:00Z).
- Worked entirely from main checkout; worktree-local strategy in effect for `.squad/` state on `feat/ui-testing`.
