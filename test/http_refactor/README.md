# PR #3771 + GUI3: HTTP refactor compatibility and regression tests

This is a **test-only integration suite** for Lemonade [PR #3771](https://github.com/lemonade-sdk/lemonade/pull/3771) (`jfowers/http-server-refactor`) merged with `GUI3_squashed`. It operates on the C++ `lemond` produced **from the checked-out merge**, not on an installed Lemonade instance. It does not modify server, GUI3, CLI, or CI product code.

The implementation contains **59 independent offline/self-tests and 88 real-daemon unittest methods**. Several live methods iterate over every applicable route reported by `/internal/routes` and perform hundreds of individual subchecks. The actual count grows with the manifest. The recorded-example checks cover the **105** response fixtures checked in with the refactor. The optional GUI3 bridge invokes the real `src/app/src/api.ts`, not a mocked frontend. As of this handoff update, the Python self-tests have been verified locally, but the corrected real GUI3 bridge still requires a new run against your built `lemond`.

## Important validation update (18 October 2026)

The maintainer's V3 run reported **2/2 suites green**, with **88 live test methods, 1 skipped**. The skipped method was `test_gui3_real_api_client_over_http`, even though the runner said `+ GUI3`: a `@unittest.skipUnless(WITH_GUI3, ...)` decorator was evaluated during module import, **before** the `--gui3` flag could set `WITH_GUI3=True`.

V4 fixes this **test-harness defect** by checking the flag inside the test method. Two additional offline tests prevent a return of this bug. **Do not report GUI3's live TypeScript bridge as verified based on the older 88/1-skipped result.** After applying V4, run:

```bash
python3 test/http_refactor/server_contract.py --lemond ./build/lemond \
  --gui3 --match gui3_real_api_client_over_http
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

With Node/TypeScript installed and `--gui3` active, **88 methods should run without this skip**. A new failure in the previously skipped bridge is a real compatibility/harness finding, not a reason to reinstate the skip. Under `--skip-gui3`, the one intentional skip is expected.

The **public-model alias listing bug predates PR #3771** (it is also present in `GUI3_squashed`). The separately supplied patch in `test/http_refactor/patches/` is *not* automatically applied. If the source fix is absent, some strict alias assertions will fail as expected; keep those failures visible and test the fix separately.

## Quick start

From the **repository root**, with this test patch applied:

```bash
# Configure and build lemond using your normal local toolchain/preset.
cmake --build build --target lemond --parallel 4
(cd src/app && npm ci)
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

If the binary is under `build/bin/lemond` or another directory, pass its full path. The compiled server and the tests must come from the same merged checkout. Python 3.10+ with the standard library suffices for the new Python tests. The GUI3 bridge needs Node 20+ and the TypeScript dependency installed by `npm ci`.

**Without Node/GUI3 dependencies**, you can still run every real-server Python contract:

```bash
python3 test/http_refactor/run_suite.py --lemond ./build/lemond --skip-gui3
```

**Without a compiled server**, run the self-tests, recorded fixture checks, and structural/docs guards:

```bash
python3 test/http_refactor/run_suite.py --self-test-only
```

The self-tests do **not** claim that the C++ refactor or GUI3 integration works: the real-server suite must also pass.

## Coverage

| Area | What the new suite checks |
| --- | --- |
| Route registry and documentation | At least 80 route definitions, IDs, unique methods/paths, path parameter binding, schema types, aliases, response formats, referenced example setups, filtered `/internal/routes`, docs markers, and runtime health schema. |
| Quad routing | All four `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/` prefixes, and **GET 405** for every POST-only Quad path (not only a hard-coded subset). |
| Request parser | Malformed JSON and huge numeric literals, non-object JSON scalars, root arrays, missing/invalid nested fields, four-prefix POST validation, method/path collisions, numeric query errors, registry search before offline enforcement, and multipart validation with real file attachments. |
| Security | Separate API/admin bearer keys, missing/incorrect/malformed credentials, admin/regular scope, unauthorized writes, authentication before HTTP 404/405, Ollama and MCP protection, `/metrics`, OPTIONS preflight, disallowed origins, same-origin access. |
| Core HTTP compatibility | Health, model inventory, stats, system stats, downloads, jobs, aliases, Ollama version/tags, metrics, HEAD response, unknown route, MCP 405. |
| Mutating contracts | Isolated alias create/list/update/delete, missing/self/reserved names and cycles, legacy argument spellings, model registration and validation without a binary backend, inventory/aliases across four prefixes, canonical/public user model names, alias chains and retargeting, downloaded-only filtering, privacy of file paths, atomic invalid option batches, dry-run options, config update/restore and atomic rejection. |
| Concurrency | Parallel route-manifest reads, parallel invalid JSON, 16 concurrent independent alias creations with no lost entries. Repeatable with `--repeat-race`. |
| GUI3 integration | Real transpiled TypeScript `api.ts` calling an isolated `lemond`: health, models, stats, system info, config, log-level GET/POST, jobs, downloads, registration, model detail/files, options and delete. |
| Recordings | 105 API examples, route coverage, payload/response shapes, stream JSON syntax, binary payload sizes, basic credential-leak guards, OpenAI/Ollama/MCP/Anthropic fixtures, balanced generated Markdown regions, matching route source IDs. |

Not covered by this offline/CPU suite: actual LLM/GPU inference, real model downloads, server-stream cancellation with an inference backend, upload decoding against a real transcription backend, and full visual GUI behavior. Use the existing project suites for these.

## Why `llama-server` is not needed for registration contracts

A minimal devcontainer can run `lemond` without having any inference backend installed. Previously, `test_model_register_list_detail_options_delete` failed with HTTP 500 (`llama-server not found in PATH`) because **registration itself** calls the system's recipe-availability check even though the test never invokes an inference backend.

`ManifestContract` (which includes the real GUI3 API bridge) and `StateAndConcurrencyContract` now start their own private daemons with **`disable_model_filtering=true`** in addition to the existing `offline=true` and `no_fetch_executables=true` settings. These are documented `lemond` configuration keys; the override bypasses only local recipe-availability filtering for synthetic registration. This is **not** a product fix and is deliberately **not** applied to security, baseline-comparison, or production environments. The new harness and runtime tests verify the flag and isolation. No fake executable is placed on `PATH`, no GPU is needed, and no network traffic is allowed.

**These tests do not verify whether a real llama.cpp backend can load or run a model.** Use the normal inference/backend test suites for that contract.

## Optional strict query-parser probes (may intentionally fail)

Source inspection of this merged snapshot uncovered two **suspected product parsing defects**, unrelated to the missing `llama-server`:

- `GET /api/v1/models?show_all=invalid-boolean` appears to be accepted as `false` rather than rejected. `typed_param()` converts any query string other than `true` into `false` for a declared boolean; the GET path does not call `check_args()`.
- `GET /internal/models/sync/status?sync_id=12.5` or `sync_id=-1` appears to be parsed by `std::stoull()` without checking the full string was consumed, despite the route declaring an integer.

These are code-level findings, **not verified against a running binary here**. An **opt-in** reproducer checks their intended strict-parser behavior, while the default suite continues to test already-established contracts:

```bash
python3 test/http_refactor/query_parser_probe.py --lemond ./build/lemond
```

A failed probe is a finding to verify against the reference branch and discuss with the PR author. Do not silently weaken it to accept malformed values. The probe is intentionally separate from the default test runner, so known/unconfirmed parser issues do not obscure unrelated integration results.

## Alias-model-list regression (pre-existing in GUI3_squashed)

`GET /v1/models?show_all=true` promises to include aliases of listed models.
Both the old `GUI3_squashed` `Server::handle_models()` and the refactored
`routes/openai/models/models.cpp` look up a **canonical** user model ID in
`get_supported_models()`, which is keyed by **public** model IDs. As a result,
`GET /v1/models/{alias}` can resolve a registered user model while
`GET /v1/models?show_all=true` leaves its alias out. This is a **product bug
inherited from the baseline**, not a newly introduced PR #3771 regression.

The regression checks are deliberately strict (not skipped or xfailed):

```bash
python3 test/http_refactor/server_contract.py \
  --lemond ./build/lemond --match alias --quiet
```

A separate, optional **product fix** for the refactored route resolves the
canonical target to its public ID before searching the returned inventory.
Apply it only on a dedicated fix branch, then rebuild; do not mix the fix with
the test-only integration commit. The tests also check aliases targeting public
IDs, alias chains, target changes/deletion, missing targets, catalog targets,
and all four Quad prefixes. A register-only model's alias must not appear in
the downloaded-only inventory.

The independently applicable product patch is stored in the test repository at
`test/http_refactor/patches/pr3771-alias-public-model-list.patch`. It changes
**only** `src/cpp/server/routes/openai/models/models.cpp`. It is intentionally
not applied by the tests, and it should be reviewed/cherry-picked independently.

To try the fix without modifying the shared test branch (start with a clean
working tree):

```bash
git switch fl0rianr/HTTP_RefactorGUI3_test
git switch -c review/pr3771-alias-fix
git apply --check test/http_refactor/patches/pr3771-alias-public-model-list.patch
git apply test/http_refactor/patches/pr3771-alias-public-model-list.patch
cmake --build build --target lemond --parallel 4
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

If `git apply --check` fails, check whether the fix is *already present*
with `git apply --reverse --check test/http_refactor/patches/pr3771-alias-public-model-list.patch`.
Never reapply blindly, and do not change the assertions to hide the failure.

## One-command runner options

```bash
# Full Python + real GUI3 TypeScript client bridge:
python3 test/http_refactor/run_suite.py --lemond ./build/lemond

# All of the above, and run the alias concurrency test 20 extra times:
python3 test/http_refactor/run_suite.py --lemond ./build/lemond --repeat-race 20

# Also run docs drift, existing C++ tests, GUI3 runtime tests and Playwright:
python3 test/http_refactor/run_suite.py --lemond ./build/lemond \
  --docs-check --ctest --with-ui --playwright

# Print exactly what will run, without modifying anything:
python3 test/http_refactor/run_suite.py --dry-run --with-ui --docs-check
```

The runner executes GUI3 runtime and Playwright tests **last**, so C++/HTTP failures can be diagnosed before lengthy browser tests. It keeps running independent suites after a failure and exits nonzero if any failed. `--ctest` assumes a configured `build/` with compiled tests. `--playwright` also needs installed browsers and the normal repo Playwright prerequisites. Optional existing CI/UI suites use their own setup and are not part of the isolated HTTP harness.

To run a specific live test without the runner:

```bash
python3 test/http_refactor/server_contract.py --lemond ./build/lemond \
  --match post_only_quad_routes
python3 test/http_refactor/server_contract.py --lemond ./build/lemond \
  --match concurrent_alias_updates
python3 test/http_refactor/server_contract.py --lemond ./build/lemond \
  --match form_temperature
```

`--match` accepts any case-insensitive substring of the method name. The wrapper uses verbose unittest output by default; `-v` is not necessary. Use `--gui3` to include the real TypeScript bridge when directly invoking `server_contract.py`.

## Compare the refactor against a separately built GUI3 baseline

A separate differential runner compares the **unchanged common HTTP surface** of two isolated binaries, deliberately ignoring volatile health/version/pid fields and unrelated model inventory order. It reports any remaining differences; it does **not** suppress unexpected regressions.

```bash
python3 test/http_refactor/compare_baseline.py \
  --baseline-lemond /path/to/GUI3_squashed/build/lemond \
  --candidate-lemond ./build/lemond

# Or include the same comparison in the full runner:
python3 test/http_refactor/run_suite.py --lemond ./build/lemond \
  --baseline-lemond /path/to/GUI3_squashed/build/lemond
```

This comparison intentionally excludes PR #3771's documented 404-to-405 changes, standardized error messages and new `/internal/routes` endpoint. Both executables use separate temporary directories and do not send traffic to an existing server. A difference in bundled model catalogs between build commits may also be legitimate; inspect the reported IDs rather than assuming every difference is a refactor bug.

## Isolation and cleanup

The real-server suites start a fresh `lemond` on loopback with an OS-assigned port, a temporary `HOME`, temporary config and model directories, disabled telemetry, `offline=true`, `no_fetch_executables=true`, and sanitized `LEMONADE_*` variables. Before running tests, they verify that the new process reports the expected private models directory. Only synthetic user model **definitions** are registered; nothing is downloaded or inferred. Files and processes are cleaned up after each group, including error exits. Auth tests explicitly create their own isolated API/admin secrets.

The Python HTTP client never follows redirects or connects to non-loopback addresses. The GUI3 bridge refuses to run unless the Python harness supplies an explicit local URL and isolation flag. The harness tests never use the running system-wide `lemond` or port 13305.

## Publishing and maintaining the shared test branch

The branch `fl0rianr/HTTP_RefactorGUI3_test` already contains the HTTP refactor
and GUI3 merge. The tests are under `test/http_refactor/`. A reviewer should
**fetch this shared branch**, not apply the old V1/V2/V3 patch sequence.

For the maintainer preparing the shared branch, preserve unrelated work and
commit only the test changes:

```bash
git switch fl0rianr/HTTP_RefactorGUI3_test
git status --short
git add test/http_refactor/
git diff --cached --name-only  # check every staged path before committing
git commit -m "test: add HTTP refactor and GUI3 contract coverage"
# Push to the remote where you have write access:
git push <your-remote> HEAD:fl0rianr/HTTP_RefactorGUI3_test
```

**Important when using the earlier combined V3 patch:** that patch also changed
`src/cpp/server/routes/openai/models/models.cpp`. The test-suite changes in
V4 do not change production C++, but any earlier applied source fix is still
in your working tree or history. Keep the fix separate from the test commit.
If the C++ file is only an uncommitted change and matches the supplied patch,
you can reverse **that one product patch** before publishing the test-only branch:

```bash
git apply --reverse --check \
  test/http_refactor/patches/pr3771-alias-public-model-list.patch
git apply --reverse \
  test/http_refactor/patches/pr3771-alias-public-model-list.patch
```

Reverse it only after confirming that no other intended edits overlap the file.
This intentionally restores the pre-existing alias-list failure on an unpatched
integration branch. If the fix is already committed, inspect history first;
do not reset or rewrite a published branch blindly. An alternative is to keep
a **clearly separate product-fix commit** in a *trial branch*, where all 88
contracts can be retested together.

Use **merges, not rebases** on the published shared test branch, so the tester can update it independently as PR #3771 and GUI3 change:

```bash
git switch fl0rianr/HTTP_RefactorGUI3_test
git status --short

# Adapt "origin" if upstream is named differently.
git fetch origin 'refs/pull/3771/head:refs/remotes/origin/pr-3771-head'
git merge --no-ff origin/pr-3771-head

git fetch origin 'refs/heads/GUI3_squashed:refs/remotes/origin/GUI3_squashed'
git merge --no-ff origin/GUI3_squashed

cmake --build build --target lemond --parallel 4
python3 test/http_refactor/run_suite.py --lemond ./build/lemond
```

When new upstream changes produce a conflict, resolve each file deliberately, commit the merge, then rebuild. Do not blindly select `--ours` or `--theirs` for server or GUI code. If a merge must be cancelled, `git merge --abort` is safer than resetting the branch. Once green, push **this test branch**, not the original PR head, so the PR author can fetch and run it without your help.

## What to send the reviewer

After pushing the shared test branch, send the short reviewer note in
[`HANDOFF.md`](HANDOFF.md). Include the actual branch URL, the merge HEAD SHA,
full suite result (especially whether `--gui3` was run after the skip fix),
and whether the optional alias product patch was applied. **Do not claim all
88 live tests passed unless the output has zero skips with `--gui3`.**

A follow-up to the initial clean-branch run should cover `--docs-check`, the
optional CTest suite and the existing GUI3 runtime/Playwright tests where local
prerequisites permit. These are independent gates, not part of the 88 HTTP
contracts.

## Failure reports

```bash
set -o pipefail
git rev-parse HEAD
git status -sb
python3 test/http_refactor/run_suite.py --lemond ./build/lemond \
  2>&1 | tee http-refactor-results.log
```

Report the test name, failed URL and response, merge commit SHA, and whether the TypeScript bridge and optional suites were enabled. For a suspected regression, compare the same behavior against an independently built `GUI3_squashed` binary using `compare_baseline.py`.
