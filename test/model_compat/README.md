# PR #3828 + GUI3: Model Manager compatibility and regression tests

This is a **test-only integration branch** for Lemonade PR
[#3828](https://github.com/lemonade-sdk/lemonade/pull/3828)
(`bitgamma/model-manager-refactoring`) merged with `GUI3_squashed`.
It checks the actual C++ `lemond` HTTP API, GUI3's real TypeScript API client,
registration and option persistence, cache safety, and concurrent operations.

**Maintainer quick start:** build `lemond`, install the GUI3 Node dependencies, and
run this command from the **repository root**:

```bash
cmake --build build --target lemond --parallel 4
(cd src/app && npm ci)
python3 test/model_compat/run_suite.py --lemond ./build/lemond
```

Use the binary built **from this checkout**, not an installed system `lemond`.
If your CMake build directory differs, replace `build` and the binary path. A
previously configured Lemonade CMake build is required. There is no automatic
`cmake -S/-B` step, because local toolchains and backend presets differ.

## 1. What is tested

| Suite | Tests / role | Run directly |
| --- | --- | --- |
| Harness | 7 standard-library fixture tests | `python3 -m unittest discover -s test/model_compat -p 'test_*.py' -v` |
| GUI3 app regressions | Existing app-level tests | `node test/app/run-app-regression-tests.cjs` |
| GUI3 runtime | Existing runtime tests + 29 mocked-HTTP API tests + 15 route guards | `(cd src/app && npm run test:runtime)` |
| Core server | 36 black-box HTTP contracts | `python3 test/model_compat/server_contract.py --lemond ./build/lemond --gui3 --verbose` |
| Advanced server | 20 advanced cache, safety, restart, and concurrency tests | `python3 test/model_compat/advanced_contract.py --lemond ./build/lemond --verbose` |
| Edge server | **17 additional** snapshot, filesystem, restart, and concurrent-access tests | `python3 test/model_compat/edge_contract.py --lemond ./build/lemond --verbose` |

The `--gui3` flag on `server_contract.py` adds tests using **GUI3's actual
`src/app/src/api.ts` against the real server**. The other GUI3 runtime tests
mock HTTP; green mocked tests alone do *not* prove compatibility with `lemond`.
In a GUI3 merge, `node test/app/run-app-regression-tests.cjs` intentionally
selects `gui3.test.cjs` only; the 15 additional route guards are picked up
instead by `src/app/tests/model-manager-routes.runtime.cjs`. Do not mistake
that runner's smaller count for missing tests.

The optional C++ project tests are separate from this harness. Once CMake is
configured, discover available tests with `ctest --test-dir build -N`, then run
`ctest --test-dir build --output-on-failure` as appropriate for the build.

### One-command runner

```bash
# Full suite, including GUI3 TypeScript against the real server:
python3 test/model_compat/run_suite.py --lemond ./build/lemond

# Without Node/UI dependencies, still executes the real-server Python tests:
python3 test/model_compat/run_suite.py --lemond ./build/lemond --skip-ui

# Reproduce the generation/parallel-registration race 20 extra times:
python3 test/model_compat/run_suite.py --lemond ./build/lemond --repeat-race 20

# Show commands without executing them:
python3 test/model_compat/run_suite.py --lemond ./build/lemond --dry-run
```

The runner keeps going after a failed suite, prints a summary, and returns a
**nonzero exit status** if anything failed. To run just a failing test family:

```bash
python3 test/model_compat/advanced_contract.py \
  --lemond ./build/lemond --match parallel_registrations --verbose
python3 test/model_compat/edge_contract.py \
  --lemond ./build/lemond --match snapshot --verbose
```

### Environment and safety

- Python 3.10+ (stdlib only), Git, CMake and a compiled `lemond`; Node/npm and
  the installed GUI3 packages are needed only for GUI3 tests.
- Each server suite launches a **new process** on loopback with a temporary
  `HOME`, separate config, separate model directory, and `offline=true`. It
  never sends model API calls to your already-running Lemonade instance.
- Server tests use synthetic GGUF headers. No actual weights, GPU, registry
  downloads, API keys, network access or inference backend are required.
- The suite cleans up its temporary server process and files even on failure.
  The only durable outputs are terminal logs and source files you commit.
- All tests remain opt-in. They do not alter CI, application build scripts,
  `src/app/src/api.ts`, or the production C++ sources.

## 2. Getting the test branch: for another contributor

The branch is named **`fl0rianr/modelRefactorGUI3_test`**. It must be pushed
to a Git remote the contributor can read; local-only branches are not visible
to someone else. If the test branch is on `origin`:

```bash
git fetch origin fl0rianr/modelRefactorGUI3_test
git switch --track origin/fl0rianr/modelRefactorGUI3_test
```

If it is published on a fork instead, add that fork as a remote and replace
`origin` with its remote name. For example (use the *actual* fork URL):

```bash
git remote add test-owner <URL-of-the-test-branch-repository>
git fetch test-owner fl0rianr/modelRefactorGUI3_test
git switch -c fl0rianr/modelRefactorGUI3_test \
  --track test-owner/fl0rianr/modelRefactorGUI3_test
```

A contributor with an existing local branch can simply switch to it. Verify
what is being tested with `git status -sb` and `git log -1 --oneline`.

## 3. Updating the branch with PR #3828 or GUI3 changes

**Use merges, not rebases, on this shared integration branch.** That preserves
its test commits and makes the result reproducible. Start from a clean working
tree: review `git status --short` and commit intentional work first. Do not
run `reset --hard`, force-push, or blindly discard unrelated local changes.

```bash
git switch fl0rianr/modelRefactorGUI3_test
git status --short

# The GitHub PR ref works even if #3828 comes from a fork:
git fetch origin \
  'refs/pull/3828/head:refs/remotes/origin/pr-3828-head'
git merge --no-ff origin/pr-3828-head

# If GUI3_squashed is hosted on origin:
git fetch origin \
  'refs/heads/GUI3_squashed:refs/remotes/origin/GUI3_squashed'
git merge --no-ff origin/GUI3_squashed

# Build and test the new merged HEAD:
cmake --build build --target lemond --parallel 4
python3 test/model_compat/run_suite.py --lemond ./build/lemond
```

If `GUI3_squashed` lives on a different remote, fetch/merge
`<gui3-remote>/GUI3_squashed` instead. If your repository's `origin` is not
`lemonade-sdk/lemonade`, fetch PR #3828 from the actual upstream GitHub remote.
Git will report `Already up to date` when that head is already merged.

**When a merge conflicts:** inspect `git status`, resolve the conflicted files
and `git add <resolved-files>; git commit`. Do not use blanket `--ours`/`--theirs`
for product code or tests. If you need to back out of the *current merge*,
`git merge --abort` preserves the pre-merge committed state. Since the new
tests live in `test/model_compat/`, they should rarely conflict with C++/GUI3
updates. Existing GUI3 source-inspection tests may need small path adjustments
when the refactoring moves code: retain the behavior being tested.

After successful tests, push the **test branch** to its intended writable
remote (`git push <test-remote> HEAD:fl0rianr/modelRefactorGUI3_test`). The
other contributor can fetch that updated head without needing your local
checkout. Do not push these test merges to `main` or to PR #3828 itself.

## 4. Race-condition fix: **separate patch, not part of the test branch**

The `advanced_contract.py` test
`test_parallel_registrations_do_not_lose_updates` exposed HTTP 500
`Model not found` after concurrent registrations. On PR #3828,
`ModelManager::build_cache()` can return when a concurrent write causes the
snapshot-generation check to reject the build. A stale/invalid snapshot can
then be read immediately afterward. This does not by itself show that model
registration was lost on disk.

The **proposed, unmerged** one-function fix is stored separately at:

`test/model_compat/patches/pr3828-cache-build-generation-retry.patch`

For an A/B comparison, first test the *unpatched* merge branch and capture the
failure. Then create a **separate local branch** for the trial fix:

```bash
git switch fl0rianr/modelRefactorGUI3_test
git switch -c review/pr3828-cache-retry

git apply --check \
  test/model_compat/patches/pr3828-cache-build-generation-retry.patch
git apply \
  test/model_compat/patches/pr3828-cache-build-generation-retry.patch
cmake --build build --target lemond --parallel 4
python3 test/model_compat/advanced_contract.py \
  --lemond ./build/lemond --match parallel_registrations --verbose
python3 test/model_compat/run_suite.py --lemond ./build/lemond --repeat-race 20
```

If `git apply --check` fails, first test whether the change is **already
present** using `git apply --reverse --check <patch-file>`. Otherwise inspect
upstream changes; do not force-apply an obsolete diff. Keep the product fix out
of the integration branch until the maintainer accepts it. This proposed fix
must be compiled and tested locally; it has not been validated against your
newest upstream merges.

## 5. Reporting a failure

From the repository root:

```bash
set -o pipefail
git status -sb
git rev-parse HEAD
python3 test/model_compat/run_suite.py --lemond ./build/lemond \
  2>&1 | tee model-compat-results.log
```

Provide the failing test name, complete traceback, the `git rev-parse HEAD`
SHA, and whether the optional C++ fix was applied. For a questionable failure,
repeat only that test and compare against an independently built pre-refactor
`main` binary. A failed contract is not necessarily a newly introduced bug.

## 6. Coverage gaps

The synthetic offline suite **does not yet validate** actual network
transfers, partially resumed HTTP range downloads, live backend inference,
GPU-specific behavior, or the full GUI3 visual workflows. Those need
separately scoped tests, preferably an HTTP fixture server and Playwright.
See [ADVANCED.md](ADVANCED.md) for background on the 20 advanced tests.
