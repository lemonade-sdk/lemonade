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

## Active definitions and the disclosed transition

The six builtin container leaves now have fixed leaf/hook contracts and the
independently reviewed execution owners. The active set is eight v2 leaves
(the two existing llama.cpp/HRX leaves plus six container leaves) with six
container execution owners. There are twenty public recipes and twenty-two
registrations; Halo Box and Nathanw remain private selectors of `llamacpp`.
The external export registry remains unchanged.

`ds4:rocm` now selects the OCI definition `ds4_rocm` and its dedicated
`backend_versions.ds4_rocm.rocm` pin. All ten OCI inventory rows already match
`server_models.json` exactly; no model rows were changed. The native DS4 source
and its `b0001` pin remain available to the independent native oracle.

The normal backend boilerplate generator regenerated the defaults and support
pages, and its `--check` pass succeeds. Fixed container backend validation now
checks supported hardware without treating the fixed backend as a public
selector. Absolute local checkpoints are accepted for container registrations.
Installation/status lookups select the actual llama container spec. Container
ports use the dynamic range, and the launch passes only the numeric render GID.
Container HOME and TMPDIR use `/var/tmp` inside Podman's writable image layer.

The new `six-container-activation` disclosure contains **167 records**, advances
runner/service policy **20 to 21**, and retains the exact **203-test** census.
All **22 prior stages** replay in independent pinned controls. No historical
fixture was changed, and every snapshot field remains in the literal comparison.
The new fixture's SHA-256 is
`4772cd25f188c039785b0e8b5368cb6a0d9aff632d0b1b076b3aa9b19a166460`.
Its complete CLI/HTTP payloads require 832,357 bytes; the pre-commit size
allowlist names only that new fixture alongside the existing vendored bundle.
The transition comparator has no exception.

The final complete CPU pass was **203/203**, in **81.30 seconds** with four
CTest workers. Evidence: `transition-author.log`, `transition/`, `ctest-final.log`,
and `generator-check.log` under this worktree's activation artifact directory.
The earlier failed expectation checks are retained beside the corrected passes.
