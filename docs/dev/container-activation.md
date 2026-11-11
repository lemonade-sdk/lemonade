# Container activation on the rebased branch

The activation starts from `a2d90783bb635e7f1fda2ae46997ea7bb91e204e`
on the isolated `rebase-squash-tmp` worktree. The original
`integration/configured-backends` reference remains at `9af81abfc`.

## Fresh baseline

On 2026-11-11 the registered worktree directory was absent, while its saved
index matched `a2d90783b` exactly. Its tracked files were restored with
`checkout-index`; no branch was moved. There was no existing `build/` to reuse.

`cmake --preset default` and
`cmake --build --preset default --target lemond lemonade cpp-ci-tests -j 8`
completed, including all independent historical control builds.
`ctest --test-dir build -L '^cpp-ci$' --output-on-failure` passed **203/203**
in 405.70 seconds. The tracked source remained unchanged during these checks.
The immutable C9 stages and the upstream absorption stage replayed as part of
that baseline.

Evidence is under `.synagent/artifacts/container-activation-20261111/` in this
worktree: `configure-baseline-host.log`, `build-baseline.log`, and
`ctest-baseline.log`. Temporary files and independent controls are under this
worktree's `.synagent/scratch/`; no task artifacts were written to `/tmp`.
The first sandboxed configuration could not resolve GitHub; the host retry
fetched the pinned build dependencies successfully.

The pre-existing `lemond` PID 2100, started on November 9 and listening on
port 8000, was observed without making requests to it or changing it.
