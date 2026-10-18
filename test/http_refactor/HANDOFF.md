# PR #3771 + GUI3 — Reviewer handoff

This branch combines [HTTP server refactor PR #3771](https://github.com/lemonade-sdk/lemonade/pull/3771) (`jfowers/http-server-refactor`) with `GUI3_squashed` and adds tests under `test/http_refactor/`. **It is an integration/test branch, not a proposed replacement for either upstream branch.**

## Run it locally

From the checked-out integration branch (do not use an installed `lemond`):

```bash
cmake --build build --target lemond --parallel 4
(cd src/app && npm ci)
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

The runner starts private loopback daemons with temporary model/config directories; its test models are never downloaded or run. To execute Python contracts only, add `--skip-gui3`. The default runs the **real GUI3 TypeScript API client** against the real C++ server; additional GUI3 runtime, Playwright, CTest and docs-drift checks can be opted into separately (see `README.md`).

On 18 October 2026, the maintainer reported **2/2 suites green; 88 live methods, 1 skipped** against the alias-fixed V3 merge. The skip turned out to be a *test harness bug*: the GUI3 bridge was incorrectly skipped at import time. **V4 fixes it; a new run with zero skips is needed to claim the real GUI3 bridge is verified.** The current offline self-suite is **59 tests, green**. No independent full `lemond` run was possible in the patch-authoring environment.

To run only the previously skipped case:

```bash
python3 test/http_refactor/server_contract.py --lemond ./build/lemond \
  --gui3 --match gui3_real_api_client_over_http
```

## Known pre-existing product issue: alias listing

Aliases targeting `user.*` model definitions may be resolvable via `/v1/models/{id}` yet missing from `/v1/models?show_all=true`. Both `GUI3_squashed` and the refactor route search a public-ID-keyed map using a canonical ID. This bug **predates PR #3771**. The tests intentionally expose it.

A minimal **optional** product fix is stored separately at:

`test/http_refactor/patches/pr3771-alias-public-model-list.patch`

Apply/rebuild it on a separate trial branch to see the suite pass, but **do not silently mix it into the review/test commit**. If the current integration branch already contains the fix from a previous combined patch, split or reverse it before declaring the branch test-only. See `README.md` for exact commands.

## Keeping this shared branch current

Do merges, not rebases, so reviewers can pull the updated test branch without dealing with rewritten history. Commit or otherwise preserve any dirty working tree first; **do not force-reset**.

```bash
git switch fl0rianr/HTTP_RefactorGUI3_test
git status --short
# If origin is not lemonade-sdk/lemonade, substitute the actual upstream remote.
git fetch origin 'refs/pull/3771/head:refs/remotes/origin/pr-3771-head'
git merge --no-ff origin/pr-3771-head
git fetch origin 'refs/heads/GUI3_squashed:refs/remotes/origin/GUI3_squashed'
git merge --no-ff origin/GUI3_squashed
cmake --build build --target lemond --parallel 4
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

Resolve conflicts manually; do not blanket-select ours/theirs in production code. Test changes under `test/http_refactor/` should seldom conflict. Push only the integration/test branch, not the underlying PR head.

## Reporting findings

Supply merge commit SHA (`git rev-parse HEAD`), exact test command, skipped-test count, failure trace, and whether the alias fix was applied. The differential runner can compare separate GUI3 baseline and merged candidates:

```bash
python3 test/http_refactor/compare_baseline.py \
  --baseline-lemond /path/to/gui3-baseline/build/lemond \
  --candidate-lemond ./build/lemond
```

Live inference, GPU/LLM backends, network downloads, stream cancellation, and full visual GUI workflows are outside the scope of this offline contract suite.
