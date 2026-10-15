# Schema-driven renderer qualification (4i)

Linux CPU browser qualification passed on `feat/schema-driven-ui`, based on
`010ca16932415985aee1db5b6d70dce2f23e3776`. Both default and all-three-configured
runs passed 30/30 harness checks and 165 browser assertions in nine stages.
The complete frontend suites, typecheck, renderer/web builds, both C++ matrices,
both generator checks and both standalone HTTP smokes passed.

Full platform adoption remains pending. Debian system-module packaging could
not build on this host, and Windows/macOS/native Tauri were not exercised.
The predecessor's intermittent shutdown SIGSEGV was not reproduced against
either native baseline or candidate; its exact runtime cause remains unverified.
The shutdown implementation and its asynchronous teardown hazard predate 4i.
No shutdown fix is included.

## Arrival and commits

Arrival HEAD was `011a4359db07effb08501d543a6bb7253ddf9318`. There were eleven
modified tracked files: runtime-backends documentation, the app manifest,
ModelOptionsModal, SchemaRecipeOptions, useBackendCatalog, recipeOptionsConfig,
backendCatalog, backendInstaller, recipeNames, styles and the modal regression
test. Untracked work consisted of backendCatalog/recipeForm/schemaRecipeOptions
tests, their fixture/helper directories, catalogBrowser and catalog_ui_exercise.
Nothing was staged. All of that work was retained, reviewed and completed.

The two pre-existing stashes were preserved:

```text
stash@{0}: WIP on feat/openmoss-0.2.0-backport: cacd53976 Merge branch 'main' into feat/openmoss-0.2.0-backport
stash@{1}: On feat/mm-all: Qwen3.6 registry entry (local test build) v2
```

| Phase | Commit | `git show --stat` total |
|---|---|---|
| Selector export, predecessor | `57f5bab280bfd73215c0fe8c133b14b80bac8a6b` | 3 files, +68/-0 |
| Form/catalog foundation, predecessor | `ebe7d6e1356a52a446bc0fcb73e7afe265c1d717` | 14 files, +495/-153 |
| Controls and Save/Load, predecessor | `011a4359db07effb08501d543a6bb7253ddf9318` | 11 files, +398/-51 |
| Stabilization and behavioral tests | `c8f4194e2de8abe29dc628e2cec79634aafab306` | 18 files, +686/-32 |
| Browser driver, transcripts and report | Commit carrying this report | See final git show/stat |

The stabilization commit completes the inherited catalog refresh, strict JSON
error handling, ordinary external-model preflight, already-loaded transient
chat path, server-rendered name snapshots and checkbox styling. It also mounts
ModelOptionsModal once outside category groups. Authentication invalidation can
empty those groups; the modal must stay mounted to show the authentication error.
During a same-model connection refresh it retains only the recipe ID for branch
selection, clears execution/catalog/form data and blocks submission until fresh
authenticated responses arrive.

## Browser and freeze evidence

The checked-in machine records contain hashes, connection flags, every browser
mutation, frozen CLI commands/output and independent exact fake-engine records:

- [Default transcript](../../test/fixtures/catalog-ui-default-transcript.json)
- [Configured transcript](../../test/fixtures/catalog-ui-configured-transcript.json)

The real CMake-built web app was served at `/app` by isolated `lemond` instances.
Chrome used CDP, real mouse/keyboard input, the actual injected `window.api`, and
GPU/software-rasterizer/Vulkan/WebGPU disabling flags. HTTPS registry traffic
was blocked. No model weights, real inference, accelerator workload or benchmark
ran. HTTP listeners used ephemeral loopback ports. Only owned processes were
stopped. The pre-existing lemond on port 8000 was not stopped or reconfigured.

Each mode executed this complete sequence:

| Stage | Assertions | Observed evidence |
|---|---:|---|
| baseline | 3 | Served bridge callback exists; fourth model absent before placement |
| fourth | 20 | Find CatalogFourth; Batch size 2; schema/server/effective/saved provenance; 9 rejected by 1..8 validation; Save integer 4 without Load; exact sparse payload; inheritance clears saved override; keyboard reaches inheritance; transient Load 4; existing chat replies exactly `fake batch=4 tag=initial` |
| probe | 22 | Fresh post-freeze recipe/option plus all five controls; text empty string, textarea newline, boolean false and typed select true survive Save; same Load/chat checks |
| legacy | 98 | All eleven literal label/order fixtures; selector presence and server choice order; selection/reset; context 4096 fallback, 6000 edit and 8192 slider maximum; string/boolean edits/reset; numeric clamp 150 and reset 20; no load/inference |
| dirty | 4 | Batch edit 5 retained in open modal |
| restart | 6 | Same URL restarted with maximum 4/default 3; first Save discovers changed catalog, emits no POST and requires review; edit 5 retained and invalid; Save/Load disabled; explicit correction to 4 saves successfully |
| incompatible | 2 | CDP supplies control_version 2 on the schema response; explicit incompatibility and disabled Save/Load |
| auth | 3 | Real bridge saves wrong key; 401 clears usable form state; Save disabled; restored key fetches fresh form; current-origin identity retained |
| switch | 7 | Navigate with per-origin credentials to second server; its authenticated catalog has 17 builtins and no fourth recipe/model; return resolves original server's new maximum 4 |

The incompatibility stage deliberately changes only the wire response through
CDP. Normal discovery, Save, Load, chat, restart and switching use real HTTP
handlers. The bridge itself is never replaced by a test mock.

Browser driver repairs preserved the assertions: legacy fixture labels are now
recipe-appropriate, FLM's installed flag comes from deterministic CPU-only fake
discovery, and a zero-tensor GGUF header supplies context metadata. The driver
waits for the requested row and requested modal identity rather than a previous
React render. Server switching seeds that origin's local credentials before
navigation. The original capitalized `Models` text wait was replaced by the real
Models button's accessibility label.

Both full freezes cover 115 artifacts: actual server/CLI, actual web assets,
shipped/source resources/schema and deterministic engine harness. The selected
server configuration and fake discovery executable have separate before/after
hash checks. The executable slots `catalog_fourth` and `ui_probe` were configured
before freeze; the external definitions/model data were placed afterward. The
probe recipe `ui-postfreeze-probe`, option `ui_probe_batch` and its additional
controls were authored after freeze using installed primitives. This proves
configuration/invocation of those installed primitives, not downloading a new
engine from an empty installation. Product binaries/bundle were checked for
absence of both new recipe and option identities before placement.

The source freeze is `c8f4194e2de8abe29dc628e2cec79634aafab306`. Default freeze
time was `2026-10-15T20:43:52.797328+00:00`; configured freeze time was
`2026-10-15T20:43:56.191694+00:00`. All 115 artifact hashes remained unchanged.
Their common canonical aggregate SHA-256 is
`3863f04b0e5661061cbb1016ccfbc010d8608da6174c9119048d53b35267b09e`.

| Artifact | SHA-256 |
|---|---|
| build/lemond | `cf65829f03e0172dd4f6cd3df23d5a8bda5cc4859029bc16c6110d84dac3655b` |
| build/lemonade | `2c34dec54012e1757043fde8211b789b92b96fe17633707192d0656c2fe3ea4a` |
| renderer.bundle.js | `684a46a61c89ecb70b222722c0766534d41dcf6e53f045a490bbce14032c0852` |

Both clients invoked the original fourth recipe and fresh probe. Every fake argv
was exactly `--port <allocated> --batch 4 --tag initial --model-fixture <tiny>`.
The four upstream requests were exactly two UI requests and two CLI requests to
`/chat`, with the literal model/prompts recorded in each transcript. UI requests
used streaming chat; CLI requests additionally used include_usage. All replies
were exactly `fake batch=4 tag=initial`. Every assertion was retained; no timing
or model-quality result is claimed. All observed owned fake children were reaped.

The existing `catalog_fourth_exercise.py` was replayed against the new full
artifact freeze and passed 45/45. Its printed old UI-pending text is historical;
it exercises the server/CLI portion only. The original 4j freeze and transcript
were preserved unchanged.

## Shutdown attribution and separate CLI limitation

`git diff 010ca1693 -- src/cpp` changes only the selector export and JavaScript
inside the HTML bridge string. The new callbacks execute in the browser; there
are no new native shutdown callbacks, C++ captures or lifetime owners. The native
handle_shutdown, stop, listener shutdown, destructor, run and main shutdown paths
are unchanged. handle_shutdown still detaches a thread, sleeps 100ms, calls stop
and std::exit while other request threads can still exist. That implementation
and its teardown hazard are pre-existing, and were left alone.

An isolated native 010ca169 baseline was built without checking out/resetting
this branch: recompile baseline server.cpp/backend_catalog.cpp with the current
Release flags, unchanged headers/dependencies and all other unchanged objects,
then relink into `/tmp/lemonade-4i-takeover/lemond-baseline`. Browser assets were
copied beside it; no production artifact was replaced. Three attempts per arm
under GDB, three plain attempts per arm and three per arm with an active browser
log WebSocket/concurrent polling all exited 0 after Cleanup complete. Thus the
exact reported SIGSEGV was not reproduced in 9 baseline and 9 candidate attempts.

**Attribution limit:** source establishes a pre-existing native shutdown path
and excludes the suspected browser callbacks as native callbacks. It does not
establish the exact SIGSEGV's runtime root cause, and a baseline SIGSEGV remains
unverified. Do not turn the source comparison into a claimed runtime reproduction.
The KB's original observation is retained with this distinction. Further crash
attribution/fixing needs a separate investigation/review.

The browser gate uses exit-checked SIGTERM for its owned-server restarts; that
was already the inherited driver's stop method. POST shutdown was investigated
separately above, and is also used by the HTTP smokes. Completed schema/Save/
Load/chat evidence is valid; it does not qualify the intermittent POST-shutdown
teardown. No exit failure was ignored or converted into success.

A separate unchanged CLI defect was discovered: `run --chat-cli --option KEY=4`
loads once with the override, then handle_chat_command starts the REPL, whose
load uses empty_recipe_options. An unsaved default 2 therefore produces batch 2.
This was reproduced with the CPU fake; CLI source is unchanged from 010ca169.
The full freeze uses the original 4j explicit workflow: `load --save-options
--option KEY=4`, assert saved/effective integer 4, then run chat. Exact argv and
request/reply assertions remain enabled. Transient generic-option propagation
through the CLI REPL remains unqualified and unfixed. UI transient Load/chat is
independently qualified by both browser runs.

## Executed commands and results

From `src/app`, all commands exited 0:

| Command | Result |
|---|---:|
| npm run test:custom-collections | 11/11 |
| npm run test:downloaded-only-settings | 3/3 |
| npm run test:app-settings | 3/3 |
| npm run test:router-collections | 83/83 |
| npm run test:app-regression | 136/136, eight suites, no skips |
| npm run test:backend-catalog | 24/24 |
| npm run test:recipe-form | 83/83 |
| npm run test:schema-recipe-options | 22/22 |
| npm run typecheck | Passed, tsc --noEmit |
| npm run build:renderer | Passed; existing model-viewer dynamic-import warning |

The total is 236 existing plus 129 new frontend cases, 365/365.
No frontend runtime or test-framework dependency was added.

```sh
cmake --build build --target lemond lemonade test_backend_catalog cpp-ci-tests web-app -j 6
cmake --build build --target web-app -j 6
ctest --test-dir build --output-on-failure -L cpp-ci
python docs/tools/gen_backend_boilerplate.py --check
LEMONADE_FROZEN_CLI=/tmp/lemonade-4i-frozen-cli python test/backend_catalog_http.py build/lemond
python test/catalog_ui_exercise.py --output /tmp/lemonade-4i-takeover-qualified-default
python test/catalog_fourth_exercise.py --freeze /tmp/lemonade-4i-takeover-final-default/freeze.json --output /tmp/lemonade-4i-takeover/fourth-replay.json
python -m black --check test/catalog_ui_exercise.py
git diff --check
```

Default commands explicitly unset LEMONADE_THINKSOUND_CONFIGURED,
LEMONADE_DS4_CONFIGURED and LEMONADE_RYZENAI_CONFIGURED. Configured commands set
all three to `1`; the configured browser output directory ends in
`qualified-configured`. CTest includes the catalog/CLI CPU tests and existing
twelve-process opt-in/freeze matrix. No native factory/manager symbols were
found by `nm -C build/lemonade` for ConfiguredServer, ConfiguredBackendEntry,
BackendManager, backends::all_registrations or backends::create_server.

| Gate | Default | All three configured |
|---|---:|---:|
| cpp-ci CTest | 104/104, 213.02s | 104/104, 217.84s |
| generator --check | Passed | Passed |
| standalone lemond HTTP + frozen old CLI | 113/113 | 113/113 |
| real browser / full freeze | 30/30 harness + 165 browser assertions | 30/30 + 165 |

CMake web-app staging built successfully with the separate manifests and shared
renderer. Its four webpack warnings concern the existing vendor dynamic import
and bundle/asset size. Debian system-module mode was attempted through
BuildWebApp.cmake using a separate `/tmp` staging/output tree and the existing
webpack launcher, with USE_SYSTEM_NODEJS_MODULES=ON. It failed resolving
`html-webpack-plugin` from that system-only environment. The host also lacks a
system webpack executable. This distribution build remains unverified; no
packages or production manifests were changed to manufacture a pass.

## Accounting and design differences

The accounting base is 010ca169; moves count as removals/additions. Physical and
nonblank lines are counted separately. Retained means the full final contents
of files affected by 4i, not the whole repository. Machine transcripts are
generated test evidence, separated from authored tests and production-generated
products. Counts below are filled from the final git diff.

| Category | Files | Physical + / - / net | Nonblank + / - / net | Retained physical / nonblank |
|---|---:|---:|---:|---:|
| Production .cpp | 2 | 23 / 1 / +22 | 23 / 1 / +22 | 8771 / 7872 |
| Headers | 0 | 0 / 0 / +0 | 0 / 0 / +0 | 0 / 0 |
| UI | 21 | 916 / 214 / +702 | 876 / 196 / +680 | 15861 / 14018 |
| Authored tests/fixtures | 13 | 1500 / 2 / +1498 | 1481 / 2 / +1479 | 2397 / 2342 |
| Docs | 3 | 362 / 0 / +362 | 307 / 0 / +307 | 814 / 679 |
| Generated test evidence | 2 | 966 / 0 / +966 | 966 / 0 / +966 | 966 / 966 |
| Generated production artifacts | 0 | 0 / 0 / +0 | 0 / 0 / +0 | 0 / 0 |

Handwritten production additions total 939 physical / 899 nonblank lines
(916/876 UI plus 23/23 .cpp). The ~1,900 gross-added production budget is met.
The retained compatibility table remains 503 physical / 439 nonblank lines,
including all eleven recipe lists, twenty definitions and sixteen aliases.
The earlier generic-infrastructure 3,000-line checkpoint remains exceeded as
documented in the prior qualification; this UI result does not resolve that
architecture cost decision.

The concrete deviations from the proposed file/order/budget are:

- backendInstaller adds 119 physical lines against its per-file estimate of 90:
  ordinary Load/chat also needs catalog preflight and the already-loaded transient
  fast path. Overall production remains within budget.
- The browser exercise is split between Python orchestration and dependency-free
  Node/CDP interaction. It exceeds the proposed 200–300-line browser-driver
  estimate to cover full freeze, both clients, all controls and all eleven forms.
- The existing modal was mounted per category; stable single ownership was
  required for auth switching and also prevents multiple live modals.
- RecipeName/RecipeOptions remain compatibility aliases to their explicit legacy
  types; external identities use strings and discriminated submissions.
- The fresh-probe executable slots are provisioned before freeze; definitions,
  option/control records and models are authored/placed after it. No runtime
  install/download capability is inferred.
- CLI persistence is explicit before chat because the unchanged REPL drops
  transient generic overrides. Its separate defect is disclosed above.
- The larger qualification report and generated transcripts exceed the small
  documentation/evidence estimate; none is counted as production or as savings.
- Debian system-module and native-platform acceptance are unverified. The exact
  shutdown SIGSEGV's baseline runtime attribution is also unverified despite
  the unchanged pre-existing native teardown path.

## Maintainer work and final boundary

No push or public PR/issue/review/comment was made. No model weights, benchmark,
GPU/NPU workload, backend migration, watcher/reload system, new runtime
dependency, manifest consolidation, native Tauri/Rust bridge change, shutdown
patch or CLI REPL patch was added. Generated production regions were not edited.
Cloud/collection protocols and specialized audio/TTS panels were retained and
covered by existing regression suites rather than redesigned.

The maintainer retains presentation review, Windows/macOS/native bridge
qualification, Debian packaging qualification, the broader architecture cost
decision, the CLI REPL fix and the unresolved shutdown crash investigation.
Linux browser success must not be represented as complete cross-platform 4j
adoption. The branch remains checked out; final state is clean after the commit
carrying this report, with the two original stashes unchanged.

Detailed run logs, baseline reproduction scripts and screenshots are retained
under `/tmp/lemonade-4i-takeover` and the two `qualified-*` output directories.
Reproducible baseline objects and failed-attempt artifacts were pruned. The
reusable baseline executable/resources and small evidence logs were retained.
