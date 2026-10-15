# Runtime catalog and frozen CLI qualification

This records the Linux CPU qualification of design units 4e, 4f, and the
server/CLI portion of 4j against the design of record
`.synagent/reports/2026-10-15T14-25-14-codex.md`. The implementation branch is
`feat/runtime-catalog-and-cli`, based on `248956cd796fe0028ebe4bc5f4f0496452debf6a`.

**4j server/CLI portion passed; full 4j pending maintainer-owned 4i and relevant platform evidence.**

No UI files, real models, inference backends, accelerators, benchmarks, packaging
manifests, public posts, or remote branches were changed or exercised. The fake
engine only formats deterministic replies and records argv/request JSON; it does
not open the model fixture. Its behavior was tested before freeze.

## Commits and scope

- 4e: `15af04e437a6fae4f7d0ffc2afe9d94ca16392b2`, 33 files, +1,417/-33 physical lines.
- 4f: `56646ebe68da59e67a68d52154f9286c9a05498f`, 22 files, +1,083/-66 physical lines.
- 4j server/CLI evidence, fixture, and replay driver: the commit carrying this report.

4e adds one process-lifetime catalog owner, privately validates complete
candidates, freezes all built-in arm decisions, and publishes descriptor and
registration views together before either entry point loads configuration.
Immediate lowercase `.json` regular files in `backends.d` are UTF-8 filename
sorted; unrelated files/subdirectories are ignored; selected symlinks,
nonregular/unreadable/oversize/invalid files fail startup. IDs, recipes,
configuration sections and external option ownership cannot collide with their
reserved built-in/common/global namespaces, including opt-out built-ins.

External entries have no trusted compatibility attachment. A pure preparation
step checks the real operation member-signature table before publication.
External contracts retain scalar JSON types, constraints, unset/default
semantics, global/saved/request integration, normalized managed-version defaults,
and public/install/pin identity separation. `external_only` never downloads.
There is no definition reload on config change, file change or SIGHUP.

Both `GET backends/schema` and `POST backends/validate` use the existing four
prefixes and authentication. The export is allowlisted metadata, canonical
SHA-256 IDs and private ETags; it excludes paths, argv, environment, install
records, expressions, raw definitions, current values and credentials. Candidate
validation uses original bytes, catches duplicate keys, supports a matching
external ID/recipe substitution privately, and never publishes or writes.
The generator runs separate temporary cache/config directories with
`--builtin-backends-only`; committed generated products remain byte-stable.

4f adds schema/validate commands and repeatable typed assignments on load/run/
launch. It splits the first `=`, parses a complete finite/non-overflowing JSON
value, rejects duplicate keys and explicit named/control collisions using
`CLI::Option::count()`, and merges into raw outgoing JSON after named processing.
It does not filter against local recipe descriptors. Server model/recipe/schema
preflight precedes pull/load/browser/background/agent effects. Named-only flows
have no mandatory schema request. Offline validation exits before discovery or
network access and prints the specified provisional warning. Exit codes are
0 success, 2 new option/definition validation errors, 1 transport/auth/protocol
failures. CLI link inputs contain only descriptor/contract/schema/operation data;
configured factories, entries, managers and registration code stay server-only.

## Executed gates

All builds reused the configured `build/` tree; no setup script or fresh tree was
used. The final build command was:

```sh
cmake --build build --target lemond lemonade test_backend_catalog cpp-ci-tests -j 6
```

The final 4e commit passed 102/102 CTest tests in default mode (203.56 s) and all
three opt-ins (204.82 s), plus generator checks in both modes. Its independent
catalog checks passed 62/62, late-default guard 1/1, and HTTP checks 108/108.

The final 4f code passed these commands in each mode:

```sh
ctest --test-dir build --output-on-failure -L cpp-ci
python docs/tools/gen_backend_boilerplate.py --check
python test/backend_catalog_http.py build/lemond
```

Default mode unsets `LEMONADE_THINKSOUND_CONFIGURED`, `LEMONADE_DS4_CONFIGURED` and
`LEMONADE_RYZENAI_CONFIGURED`. Enabled mode sets all three to exactly `1`.

| Final mode | CTest | Generator | Standalone lemond HTTP smoke |
|---|---:|---:|---:|
| Default | 104/104, 210.02 s | Passed | 109/109 |
| All three enabled | 104/104, 199.57 s | Passed | 109/109 |

The final focused command was:

```sh
ctest --test-dir build --output-on-failure -R 'BackendCatalog|BackendSchemaCli'
```

It passed 5/5: catalog 72/72 assertions, two independent late-bootstrap guards,
HTTP 109/109, and CLI 87/87. The existing 12-process opt-in/freeze matrix remains
inside each full run. Tests use literal wire/argv/response expectations and
independent fixtures, including a server-only recipe absent from the CLI.

This linkage check returned zero matches, corroborated by the CLI link inputs:

```sh
nm -C build/lemonade | rg 'ConfiguredServer|ConfiguredBackendEntry|BackendManager|backends::all_registrations|backends::create_server'
```

New Python fixtures passed Black and the commit hooks. Native Windows/macOS
builds and process behavior remain **UNCERTAIN/unrun**.

## Freeze

Freeze occurred at `2026-10-15T17:30:11.418546+00:00`, after 4e and 4f were green
and committed, before the fourth definition was authored. The frozen source
commit is `56646ebe68da59e67a68d52154f9286c9a05498f`, tree
`f184b1a8b69abf2fabba3af60154f85033d2f48b`. Host:
Linux 7.0.0-38-generic, x86_64, glibc 2.43; Ninja Release, `/usr/bin/c++`, testing
ON, web app ON, Tauri OFF. No web-app build or source change was performed.

The reused configured build embeds version label
`2026.44.0~1699.15af04e4` in both executables. It was compiled and tested with the
final 4f source before that commit was created; the configured label was retained.
The hashes below identify the actual tested artifacts.

| Frozen artifact | SHA-256 |
|---|---|
| `build/lemond` | `5e8daa6f8795f873aedbadd07f8513a42f3b228b57ea8ff3397fb57f8ff09e60` |
| `build/lemonade` | `d1c18aa78fedc98115c25ebeffe0733c315d096055c7d9856abe557e240414cd` |
| `test/fake_catalog_engine.py` | `ec6ade8fd38169e9d690ff4c9a0083e77c54af2cf9d84c45c2279b17153db0da` |
| `schemas/backend-definition-v1.schema.json` | `ddbca624e15b0b74eda862e0c315e6208f0c403aa1417232aed91c5188445169` |

The complete [freeze manifest](../../test/fixtures/catalog-fourth-freeze.json)
covers **110 files**, including source/shipped resource copies and generated
headers. Its aggregate SHA-256, before and after the exercise, is:

```text
9d5819be92044fdb40bc470fe4cc490ec4b0835727796bb057f45de8ed194127
```

No build or generator write occurred after freeze. Both frozen binaries were
checked for absence of the new recipe and option strings before placement.

## Fourth-definition transcript

The exact [external definition](../../test/fixtures/catalog-fourth.json) was
authored after freeze. It introduces `catalog-fourth` and
`catalog_fourth_batch` (integer, default 2, range 1..8), configuration section
`catalog_fourth`, a CPU subprocess with `external_only`, identity JSON/SSE chat
transport, and existing typed reference/string expressions. It contains no hook,
compiled registration, trusted compatibility attachment, generator entry or
named Lemonade flag.

The definition was placed only in the isolated server's
`<config_dir>/backends.d/90-fourth.json`. `user_models.json` registered
`CatalogFourth` with `recipe:catalog-fourth`, `source:local_path`, and an absolute
checkpoint to a **26-byte** fake fixture. The client used a distinct empty config
directory and never acquired a `backends.d` directory. The fake fixture was not
read by the fake engine and its bytes remained unchanged.

The server was first started with only its 17 compiled recipes, stopped, then
restarted after definition/model placement. The server port stayed fixed across
restarts. Every CLI invocation used the frozen binary with explicit loopback
host/port and `--no-discovery`. The core commands were:

```sh
lemonade backends schema catalog-fourth
lemonade backends validate <server-config>/backends.d/90-fourth.json
lemonade config set catalog_fourth.cpu_bin=<frozen-harness-path> catalog_fourth.catalog_fourth_batch=3
lemonade load user.CatalogFourth --save-options --option catalog_fourth_batch=4
lemonade run user.CatalogFourth --chat-cli --option catalog_fourth_batch=4
lemonade unload user.CatalogFourth
lemonade run user.CatalogFourth --chat-cli --option catalog_fourth_batch=4
# After the server-only restart, no generic override was supplied:
lemonade run user.CatalogFourth --chat-cli
```

Each run piped one literal prompt followed by `/exit`. The requests were exactly:

```json
{
  "model": "user.CatalogFourth",
  "messages": [{"role": "user", "content": "fourth before"}],
  "stream": true,
  "stream_options": {"include_usage": true}
}
```

The next two requests differed only in prompt text: `fourth while running` and
`fourth after restart`. Every fake request used `/chat`. Observed argv were:

```text
--port 8001 --batch 4 --tag initial --model-fixture <temp>/tiny model 雪.fixture
--port 8002 --batch 4 --tag initial --model-fixture <temp>/tiny model 雪.fixture
--port 8003 --batch 4 --tag revised --model-fixture <temp>/tiny model 雪.fixture
```

Exact replies reaching the frozen CLI were, respectively:

```text
fake batch=4 tag=initial
fake batch=4 tag=initial
fake batch=4 tag=revised
```

The saved-options endpoint reported global/default 3, saved 4, effective 4.
After restart, an invocation with **no** `--option` still launched with batch 4.

While running, the definition's declared default was changed from 2 to 7 and its
argv tag from `initial` to `revised`. The entire filtered schema remained equal.
An unload/reload still launched the old argv. Only the next server restart
adopted default 7 and the revised tag; installed-feature and frozen-file hashes
remained equal.

| Catalog state | ID |
|---|---|
| Before placement, 17 recipes | `sha256:91e74cd40256314e02350dbd3ad1da889bd527e6a32085cc2ab0a52d561c522b` |
| External installed / unchanged during edit | `sha256:94ba4b4be6acfb6dc53ae376eb6323be3c8103e3df00b172638d24559e3930bd` |
| After server-only restart | `sha256:6347ba452c8f3399972571fb271b41abe1b3bc2beae71a7d3954da6e0a9f8ffc` |

Definition hashes changed from
`sha256:09a58b8fdca3795559c7fd060d548baa8e10bfc1c8373cd26e0bf0d95379d7a5`
to `sha256:e1c6f4965d0152be92e650693db70f358742dd1bc681402db59210f96676488d`.
All three server lifetimes exited 0. Fake-engine PIDs 898571, 898622, and 898861
were observed across HTTP worker threads and verified reaped. Temporary config,
client, model and trace directories were removed.

The [full machine transcript](../../test/fixtures/catalog-fourth-transcript.json)
records **45/45** checks, command exits, exact observations and all frozen hashes.
Replay without building:

```sh
python test/catalog_fourth_exercise.py --freeze test/fixtures/catalog-fourth-freeze.json --output /tmp/catalog-fourth-transcript.json
```

## Accounting

The comparison baseline for this assignment is `248956cd7`. The platform threshold
uses the design's baseline `b7593cd18`, including schema, integration, CMake and
generator code, excluding definitions/tests/docs. Gross additions, removals and
net are separate; no future wrapper deletion is deducted. Retained whole-file
figures include pre-existing integration code and are not a generic-platform LOC
claim. Tests/fixtures include the generated freeze/transcript evidence. The new
fourth JSON fixture is counted under definitions.

### Assignment delta and retained affected-file footprint

| Category | Files changed | Physical + / - / net | Nonblank + / - / net | Retained affected files: physical / nonblank |
|---|---:|---:|---:|---:|
| Production `.cpp` | 17 | 543 / 56 / +487 | 506 / 54 / +452 | 15,605 / 14,018 |
| Headers | 8 | 67 / 1 / +66 | 54 / 1 / +53 | 464 / 382 |
| Schema/parser | 3 | 28 / 2 / +26 | 24 / 2 / +22 | 467 / 448 |
| Definitions | 1 | 115 / 0 / +115 | 115 / 0 / +115 | 115 / 115 |
| Generator/CMake/packaging | 2 | 35 / 5 / +30 | 33 / 5 / +28 | 4,251 / 3,794 |
| CLI | 6 | 222 / 15 / +207 | 207 / 15 / +192 | 2,862 / 2,507 |
| UI | 0 | 0 / 0 / +0 | 0 / 0 / +0 | 0 / 0 |
| Tests/fixtures | 8 | 2,467 / 0 / +2,467 | 2,392 / 0 / +2,392 | 2,467 / 2,392 |
| Documentation | 2 | 452 / 0 / +452 | 372 / 0 / +372 | 452 / 372 |
| Committed generated products | 0 | 0 / 0 / +0 | 0 / 0 / +0 | 0 / 0 |
| **Total** | 47 | 3,929 / 79 / +3,850 | 3,703 / 77 / +3,626 | 26,683 / 24,028 |

Categories are disjoint: CLI includes its headers and CMake file; schema/parser
includes the definition store/model, request mapper and JSON schema; other
headers and `.cpp` files are in their respective columns. Generated evidence
JSON is charged to tests/fixtures. Retained figures mean the complete current
contents of affected files, including old code; they are not additions or the
complete repository footprint. Zero affected UI files means no implementer
changes, not completion of 4i.

### Platform threshold, measured from `b7593cd18`

| Stage / measure | Files | Physical + / - / net | Nonblank + / - / net |
|---|---:|---:|---:|
| Before this assignment, generic | 30 | 2,870 / 18 / +2,852 | 2,763 / 18 / +2,745 |
| Before this assignment, including trio scaffolding | 36 | 3,043 / 81 / +2,962 | 2,927 / 77 / +2,850 |
| After 4e, generic | 48 | 3,464 / 32 / +3,432 | 3,306 / 32 / +3,274 |
| After 4e, including trio scaffolding | 54 | 3,637 / 95 / +3,542 | 3,470 / 91 / +3,379 |
| After 4f / 4j, generic | 55 | 3,745 / 77 / +3,668 | 3,567 / 75 / +3,492 |
| After 4f / 4j, including trio scaffolding | 61 | 3,918 / 140 / +3,778 | 3,731 / 134 / +3,597 |

**The 3,000-line stop-and-simplify threshold is exceeded.** The updated generic
total is **3,567 gross added / 75 removed / 3,492 net nonblank lines**, an overrun
of 567 gross added lines. Charging retained trio integration/scaffolding gives
**3,731 / 134 / 3,597**, an overrun of 731 gross added lines. Schema, comments,
CMake, generator, CLI and all new shared production code are counted. No future
deletion, formatting compression or hypothetical engine amortization is used.
The 4j commit adds evidence/data/docs only and does not increase either platform
total. These are current-diff counts, not a sum double-counting intermediate
replacements across commits.

### Retained subsets and artifacts

| Retained subset | Files | Physical lines | Nonblank lines |
|---|---:|---:|---:|
| Configured source/header directories plus schema | 22 | 2,974 | 2,864 |
| Six trio A wrappers/server headers, including compatibility/opt-in associations | 6 | 742 | 635 |
| Three built-in definitions | 3 | 884 | 884 |
| Fourth external definition fixture | 1 | 115 | 115 |
| Definition JSON schema alone (included above) | 1 | 1,266 | 1,266 |
| Unchanged committed generator destinations | 8 | 4,188 | 3,340 |
| Four frozen generated backend headers in build/include | 4 | 2,365 | 2,349 |
| Frozen fake-engine harness | 1 | 99 | 88 |
| Fourth-exercise driver | 1 | 392 | 375 |
| Freeze manifest and full transcript | 2 | 594 | 594 |
| Unchanged UI tracked UTF-8 text | 131 | 54,610 | 49,681 |

There are also seven unchanged binary UI assets. The UI inventory describes
retained files only; none was edited or rebuilt by this assignment. Configured
and A implementations coexist, with the ThinkSound trusted SSE association and
DS4 trusted launch association retained in the measured source. External
entries receive neither. The frozen harness has product-specific fixture argv
support authored before freeze; the fourth definition adds no new harness code.
The eight committed generated destinations and four build headers are
overlapping retained inventories, not extra source additions. The 110-file
freeze manifest includes binary resources, which have no meaningful LOC count.

Reproduction uses `git diff --no-ext-diff --unified=0 BASE..HEAD`: count added
and removed source lines separately, count a nonblank line when `line.strip()`
is nonempty, then count full current file contents for the retained column.
The threshold selects `src/cpp`, `schemas`, root CMake and the docs generator,
excludes definition resources/tests/docs, and separates the six trio files.
The fourth definition is categorized as definition data rather than test code.

## Tree/design adaptations and limits

- The pre-4e planner already filled arbitrary defaults at the end of preparation;
  external values were additionally resolved early for preparation expressions.
- cpp-httplib deduplicates identical query pairs, so repeated filters are counted
  from the original target. It also normalizes query escaping; CLI tests retain
  its equivalent URL representation. The existing CLI's 401 exception text is
  preserved.
- Repeatable CLI11 vector options need `allow_extra_args(false)` to avoid consuming
  model positionals. Explicitness also covers dedicated model/save/pinned controls.
- Generic launch without a model selects registered models rather than taking the
  existing recipe-import branch before preflight. Named-only launch is unchanged.
- The 4f gates tightened shared seams: scalar-only v1 sentinels, the legacy global
  `no_broadcast` reservation, normalized managed pin keys/public identities, and
  direct built-in schema snapshot consumption. These are bounded 4e/data-contract
  corrections included in the 4f commit.
- Existing `config set` normalizes hyphens; the fourth definition declares the
  explicit `catalog_fourth` section so the frozen command can configure it.
- The exercise initially stopped at the downloaded-fixture guard until the
  existing `source:local_path` field was supplied. No pull/load was attempted in
  that arm. A cleanup observer initially inspected only the main thread; it was
  corrected to inspect every server thread. No frozen artifact changed.

No UI implementation, further engine migration, watcher, runtime replacement,
plugin loader, second expression language, persistent client cache, real model
work, accelerator work, benchmark, native Windows/macOS qualification, setup,
fresh build tree, push, PR, issue or discussion action was performed. Retained A
wrappers and the three existing opt-ins remain. The measured threshold overrun
is a cost checkpoint; this qualification does not raise the ceiling or establish
full 4j adoption.
