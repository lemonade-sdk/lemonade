# Testing Guide

Lemonade is tested primarily by Python integration suites that run against a live server, plus C++ unit tests for pure logic and a small regression suite for the app. A feature isn't done until a test that could catch its regression runs in CI.

This guide covers what reviewers expect from the tests in a PR: where they go, how to run them, and what gets a PR sent back.

- [Principles](#principles)
- [Where Tests Go](#where-tests-go)
- [Running Tests Locally](#running-tests-locally)
- [Writing a Good Test](#writing-a-good-test)
- [CI Expectations](#ci-expectations)
- [What Reviewers Reject](#what-reviewers-reject)

---

## Principles

### Tests ship with the feature

New endpoints, commands, and backends need at least one test that exercises them, in the same PR as the code. Bug fixes get a regression test in the suite that owns the surface. Name the new test methods in your PR description or review replies so reviewers can find them.

### A test that isn't in CI doesn't exist

A committed test file that no workflow runs is an incomplete contribution. New Python suites must be added to a CI job; new C++ tests must be declared with `add_cpp_ci_test(<Name> CI ON COMMAND <target>)` in `CMakeLists.txt` so the `cpp-ci` CTest label picks them up. Guard the block with `if(BUILD_TESTING AND ...)` — distro packaging configures with `BUILD_TESTING=OFF` to avoid building test binaries it discards, and an unguarded declaration fails that build at configure time.

Most suites join the existing endpoint/CLI or hardware-matrix jobs in `cpp_server_build_test_release.yml`. A dedicated workflow is appropriate only when the suite has environment needs the existing jobs can't meet (real network downloads, a container, path-filtered smoke tests).

### A test must be able to fail

Before trusting a regression test, verify it fails on the pre-fix code and say so in the PR ("verified this test fails on `main`"). Assert the behavior the test name claims — a stats test that only checks counters are `>= 0` passes even when every counter is zeroed. For numeric outputs (classifiers, rerankers), structural checks (labels present, scores in range) pass on garbage; compare against a golden reference instead.

### Extend existing suites

Test suites are organized by modality and API surface, not by backend or device. A new backend or device for an existing modality — image generation on NPU, a new LLM engine — is covered by the existing modality suite through `--wrapped-server` / `--backend` flags and `test/utils/capabilities.py`, not by a new test file. Only a genuinely new surface (new endpoints, a new protocol) gets a new file.

### Test the mechanism, not the data

When a change adds rows to a data table an existing mechanism already consumes — a new GPU architecture, a model registry entry, a backend version pin — the mechanism's existing tests plus the live CI checks (artifact 404 probes, `test/server_gfx_topology.py`, the scheduled `validate_*` workflows) are the coverage. Don't add a test that asserts the new row exists. Verify the change on real hardware where you can, and state what you could and couldn't verify in the PR description.

### Deterministic over clever

No timers or sleeps as success signals — monitor logs or process IDs instead. Assert output is non-empty rather than coupling to a specific length or wording. Never add product or API surface just to make a test deterministic; configure the model (for example, llamacpp args in the test) instead.

### Respect the CI budget

Use the smallest model that exercises the code path. Suites that run on GitHub-hosted runners download models fresh every run: aim for under 1 GB, and treat 5 GB as a hard cap. The self-hosted hardware runners keep persistent model caches, so inference suites there may use larger models when the modality has no small variant. Scenarios that would meaningfully extend every PR's runtime (upgrade paths, long-idle behavior) belong in scheduled or manually-triggered workflows, not the per-PR gate.

---

## Where Tests Go

| If your PR... | Add or update tests in |
|---|---|
| Adds or changes an HTTP endpoint | `test/server_endpoints.py` |
| Adds or changes a CLI command | `test/server_cli2.py` |
| Adds a backend for an existing modality (LLM, image, audio, TTS) | The existing modality suite (`test/server_llm.py`, `test/server_sd.py`, ...): register the backend in `test/utils/capabilities.py` and `test/utils/test_models.py` so `--wrapped-server` / `--backend` cover it, and add a CI matrix row exercising it. See [adding a backend](./adding-a-backend.md). |
| Adds a backend with a new modality (new endpoints) | New `test/server_<modality>.py` modeled on `test/server_sd.py`; register it in `test/utils/capabilities.py` and `test/utils/test_models.py`; wire it into both the Windows and Linux test blocks of `cpp_server_build_test_release.yml` |
| Changes LLM inference (llamacpp, RyzenAI, FLM, vLLM) | `test/server_llm.py`, run per backend with `--wrapped-server` / `--backend` |
| Touches the Ollama-compatible API | `test/test_ollama.py` |
| Touches the Anthropic-compatible API | `test/test_ollama.py` (despite the name, this suite owns both the Ollama- and Anthropic-compatible API tests) |
| Touches the MCP gateway | `test/server_mcp.py` |
| Changes audio transcription | `test/server_whisper.py` or `test/server_moonshine.py` |
| Changes text-to-speech | `test/server_tts.py` or `test/server_tts_openmoss.py` |
| Changes image generation | `test/server_sd.py` — a new device is a flag on this suite, not a new file |
| Changes the router or routing policies | `test/cpp/test_routing_*.cpp` and `test/server_router.py`; keep `test/test_schema_lock.py` and `test/test_routing_fixtures.py` green |
| Changes the jobs engine | `test/cpp/test_job_*.cpp` and `test/server_jobs.py` |
| Changes WebSocket / Realtime behavior | `test/test_websocket_idle.py`, `test/server_websocket_auth.py` |
| Changes streaming error handling | `test/server_streaming_errors.py` |
| Changes model downloads or registry search | `test/server_downloads.py` |
| Changes API key authentication | `test/server_cli_apikey.py`, `test/server_websocket_auth.py` |
| Adds pure C++ logic (parsers, arg resolvers, utilities) | `test/cpp/test_<thing>.cpp`, declared via `add_cpp_ci_test()` in `CMakeLists.txt` |
| Changes `server_models.json` | Update `test/utils/test_models.py` and `test/utils/capabilities.py` if tests reference the affected models |
| Changes the desktop or web UI | `npm run typecheck` must pass; add a `test/app/app-regression/*.test.cjs` regression test where practical |
| Fixes a bug | A numbered regression test in whichever suite above owns the surface |
| Changes a persisted JSON format | A schema-version assertion in the owning suite, so accidental format bumps are caught |
| Docs only | No tests; `markdown-link-check` must pass |

---

## Running Tests Locally

Most Python suites expect a server already running on port `13305` (override with `LEMONADE_TEST_PORT`); they do not start one. That includes `test/server_cli2.py`, which fails fast in `setUpClass` when no server is reachable. Exceptions like `test/server_jobs.py` launch their own `lemond` from the build directory.

```bash
pip install -r test/requirements.txt
python test/server_endpoints.py
python test/server_cli2.py
python test/server_llm.py --wrapped-server llamacpp --backend vulkan
```

The `lemonade` CLI binary is auto-discovered from your CMake build directory; override with `--cli-binary`.

C++ unit tests — configure the build first (`./setup.sh` on Linux/macOS, `./setup.ps1` on Windows) if you haven't already.

Linux / macOS:

```bash
cmake --build --preset default --target cpp-ci-tests
ctest --test-dir build -L "^cpp-ci$" --output-on-failure
```

Windows (generator-independent, so it works whether `setup.ps1` configured the `windows` (VS 2022) or `vs18` (VS 2026) preset; Visual Studio builds are multi-config, so `ctest` needs `-C Release`):

```powershell
cmake --build build --config Release --target cpp-ci-tests
ctest --test-dir build -C Release -L "^cpp-ci$" --output-on-failure
```

App typecheck and regression tests:

```bash
cd src/app && npm ci && npm run typecheck
cd ../..
node test/app/run-app-regression-tests.cjs
```

Routing schema checks (pure Python, no server needed):

```bash
python test/test_routing_fixtures.py
python test/test_schema_lock.py
```

---

## Writing a Good Test

- Server integration suites (`test/server_endpoints.py`, `test/server_llm.py`, and most other `test/server_*.py` files) extend `ServerTestBase` (`test/utils/server_base.py`) and end with `run_server_tests(...)`. Suites that manage their own `lemond` processes (`test/server_jobs.py`), suites that drive the CLI against a persistent external server (`test/server_cli2.py`), and pure Python unit, fixture, and schema tests (`test/test_routing_fixtures.py`, `test/test_schema_lock.py`) are plain `unittest.TestCase` classes.
- Test methods are numbered to enforce order: `test_020_...`, with letter suffixes to insert between existing numbers (`test_021a_...`). Follow the natural sequence of the suite.
- Use the real client SDKs (`openai`, `ollama`) rather than raw HTTP where a suite is proving API compatibility.
- Gate tests on declared server capabilities with `@skip_if_unsupported` from `test/utils/capabilities.py`; it skips based on what the configured `--wrapped-server` / `--backend` reports supporting, not on detected hardware. Use `@requires_backend(...)` to gate on a specific backend. In practice, capability- and backend-gated tests execute on the self-hosted hardware runners and skip elsewhere.
- Mock external services in-process (for example, the mock cloud provider) so tests run in CI without secrets or network dependencies.
- Clean up after yourself: restore environment variables with `self.addCleanup(...)`, terminate any subprocess the test starts, and never leave a server running on a hardcoded port.

---

## CI Expectations

Every PR runs the C++ `cpp-ci` tests, the endpoint/CLI suites on Windows and Linux, routing schema checks, app typecheck and regression tests, and the docs drift and link checks. Inference suites run on self-hosted AMD hardware runners ([details](./self-hosted-runners.md)); [What defers to the merge queue](#what-defers-to-the-merge-queue) covers when they run.

- Relevant local tests should pass before requesting review. All required CI must be green before final approval and merge.
- Claiming a failure is a pre-existing flake requires evidence: link a `main` run with the identical failure signature. Fix flaky tests at the root cause; don't widen thresholds or add retries.
- The PR description states how the change was tested and which platforms you could not cover. Ask in the [Discord](https://discord.gg/5xXzkMu8Zk) for help testing on hardware you don't have.
- A silently-skipped test is a bug: if your change should be exercised by an existing CI job, confirm the job actually ran it rather than skipping.

### What defers to the merge queue

Packaging, distro, PPA, backend-validation, self-hosted inference and most macOS jobs do **not** run on PR pushes. They run in the merge queue, so a break there blocks the merge rather than every push. Each group reports through one aggregate check whose name is stable enough to be required:

| Group | Gate check | Opt in on a PR with |
|---|---|---|
| Fedora RPM, Debian 13, Arch, openSUSE, Launchpad PPA, `Build Lemonade Desktop Installer` | `Packaging builds`, `Linux distro builds`, `Launchpad PPA builds` | `ci:distros` |
| macOS `.dmg`, `Test CLI/Endpoints (macos-latest)`, `Test Embeddable (macOS)`, `Test .dmg - macOS inference` | `macOS builds` | `ci:macos` |
| llama.cpp, vLLM, stable-diffusion.cpp validation | `llama.cpp validation`, `vLLM validation`, `stable-diffusion.cpp validation` | `ci:upgrades` |
| `Test .exe - *` and `Test .deb - *` inference suites on the self-hosted rigs | `Inference backend tests` | `ci:backends` |

Every gated job is reachable from a PR by label — nothing is merge-queue-only. Apply the label when your change plausibly affects that surface (a backend version pin wants `ci:upgrades`, packaging or install-path changes want `ci:distros`, `#ifdef __APPLE__` or CMake changes want `ci:macos`, a wrapped-server or inference-path change wants `ci:backends`); the label takes effect immediately, without a push. Fork contributors can't apply labels themselves — ask a maintainer (or the [Discord](https://discord.gg/5xXzkMu8Zk)) to add one. Note that `Test .dmg - macOS inference` exercises several of the same wrapped servers (llama.cpp, whisper.cpp, moonshine, kokoro) on Metal but lives in the macOS group — a change to one of those that could break on Metal wants `ci:macos` too. `Build Embeddable Lemonade (macOS)` still runs on every PR as the AppleClang compile check.

Two consequences worth knowing:

- **A green PR does not mean macOS, packaging or hardware inference are green.** If your change touches those surfaces, label it rather than discovering the break in the queue.
- **Adding a new suite to a gated job means it only runs in the queue by default.** Say so in the PR description.

### macOS specifics

The macOS `.pkg` suites run against an installed package, whether or not Apple signing secrets are present (`Test Embeddable (macOS)` is separate — it tests the embeddable tarball) — without them the installer is simply unsigned and notarization is skipped. There is no separate fork-PR test path, so a macOS test runs the same way everywhere. Use the `disable_macos_signing` input on a `workflow_dispatch` run to reproduce the unsigned path on demand.

---

## What Reviewers Reject

| Anti-pattern | Do instead |
|---|---|
| Reimplementing C++ logic in Python and asserting against the replica | Test the real code path, or probe the real artifact (e.g. CI 404-checks on release URLs) |
| Asserting a hardcoded list won't drift | Delete the test; validate against the live source of truth |
| Timers or sleeps as success signals | Monitor logs or track process IDs |
| Assertions coupled to model output length or wording | Assert non-empty output |
| New API surface added only for testability | Configure the model or server through existing options in the test |
| A new test file for a device variant | A flag on the existing suite |
| Committing a test no CI workflow runs | Wire it into a workflow or `add_cpp_ci_test()` in the same PR |
| Touching a merge-queue-gated surface and shipping on a green unlabeled PR | Apply the matching label from the [deferred-groups table](#what-defers-to-the-merge-queue) so the gated jobs actually run |
| Large models in CI jobs that download fresh every run | Use a sub-1 GB model and note the substitution in a comment |
| Negative tests that don't reset state (env vars, loaded models) | `self.addCleanup(...)`; verify the test still tests what it claims |
| Structural-only assertions on numeric outputs | Golden-reference comparison |
| Dismissing red CI as "flaky" without evidence | Link the identical failure on a `main` run |
