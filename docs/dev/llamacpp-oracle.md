# Frozen llama.cpp wrapper oracle

Unit 4b freezes the wrapper at `cc73edbeb800589a62478b963e8a0cf2d920357d` before the unit 4e configured implementation replaces it. The production descriptor, factory, GGUF resolver, v1 definitions, schemas and exports retain their existing roles.

## How the seam works

`llamacpp_effects.h` provides a test-only, thread-local observation interface. Ordinary production builds expand its effect macros to their original expressions and compile no provider or observation state. The test targets compile the actual wrapper, request transformations, backend utility functions and runtime configuration validation with deterministic providers. They replace installation, path lookups, environment reads, architecture/support facts, runtime staging, process launch and HTTP transport at their effect boundaries. The system version probe reads a synthetic `FILE` through the original `fgets` code; no shell or engine runs.

The test library excludes the four ordinary object files before linking their instrumented counterparts. It retains the production implementations of argv construction, custom argument parsing, runtime defaults, device validation, binary selection precedence, release selection, CUDA visibility policy, request transformations and GGUF resolution. A missing effect provider fails closed.

The `#line` anchors preserve the production log metadata locations from the frozen source. They prevent observation scaffolding from changing the generated production machine code. `test/llamacpp_oracle_audit.py equality` compiles the four original translation units from the pinned Git revision with the current compiler flags and generated build version, substitutes those objects into a control link, and compares object and executable bytes. This avoids comparing against potentially stale binaries already present in a build directory.

## Fixtures and consumers

All fixtures live in `test/fixtures/llamacpp-oracle/`:

| File | Contract |
| --- | --- |
| `cases.json` | Named input cases with explicit options, model facts, filesystem trees and injected external facts. |
| `pins.json` | The backend-version facts used at capture. |
| `linux_x64.json` | Complete native expected results, ordered effects, launch records, descriptor and protocol transcripts. |
| `linux_arm64.json` | Wrapper and install branch expectations for Linux ARM64. |
| `windows_x64.json` | Wrapper and install branch expectations for Windows x64. |
| `macos_arm64.json` | Wrapper and install branch expectations for macOS ARM64. |
| `descriptor-{linux,windows,macos}.json` | Independently compiled full descriptor data, including support-row order, architecture/channel gates, defaults and asymmetric argument/binary variants. |
| `provenance.json` | Baseline source hashes, fixture hashes and capture constraints. |

The four wrapper views execute Linux-hosted C++ code with selected wrapper platform branches. Their support sets, architecture facts, inherited environment and install paths are supplied explicitly. They retain the host descriptor ABI in their install spec; the separate descriptor fixtures constrain the native binary spelling for each platform. These views do not qualify native Windows or macOS behavior.

Each view compares the whole actual JSON transcript to its frozen expected transcript. There are no ignored fields or comparator exceptions. Synthetic paths become `<ROOT>` and temporary cache paths become `<TEMP>`. Platform policy paths such as the Linux HIP plugin search directories remain literal because their spellings and order are part of the behavior. The fixture case table uses one complete record per line; the descriptor and protocol bodies use expanded JSON.

Unit 4e should construct the same input frames and compare its outputs to these files. It should reuse the fixtures, without deriving expected values from its new definitions or regenerating them as part of normal tests. `--capture OUTPUT` is an explicit maintainer tool, not part of CTest. A future intentional behavior correction needs its own reviewed fixture change.

## Captured surface

- Backend validation receives the original option. Bare ROCm is resolved through the configured channel, including the no-config stable default. Installation receives the effective backend. System mode keeps the wrapper's GPU assignment; literal CPU changes the device to CPU.
- Launches preserve exact argv order, local `-m` versus HF `-hf`, context, device selection, port, Jinja, metrics, log-level verbosity, optional artifacts and custom arguments appended last. All reachable backend/channel rows are represented, alongside invalid selections.
- Embedding contexts below, at and above 8192 retain the requested option and use the effective floor only in argv. LLM and reranking contexts do not receive that floor.
- HF, vision/mmproj, CPU mmproj offload, raw versus resolved draft paths, mixed-case DFlash filenames, case-sensitive labels, DFlash/MTP precedence and explicit speculative overrides are independent input dimensions.
- Runtime argument defaults cover `merge_args`, wrong-typed input fallbacks, `--parallel` and `-np`, equals syntax, explicit speculative values, quotes, empty tokens, unmatched quotes, whitespace and negative values. Scope-resolution fixtures retain backend, architecture, model, tombstone and request precedence.
- Every reserved spelling is attempted with both separated and equals values, in both HF and local branches. Inactive device, mmproj, draft, embedding and reranking spellings remain reserved. Model and HF reservations remain conditional. The raw debug `-lv` and conditional `--no-mmproj-offload` behavior are retained.
- Environment records retain assignment order, absent versus empty values, TheRock runtime ordering, inherited loader paths, CUDA device visibility, explicit device and `LLAMA_ARG_DEVICE` precedence, PRIME offload, Windows PATH/staging/SVM decisions, Metal residency and HOME fallback. Working directory, program, output inheritance, health-log filtering and readiness endpoint are exact fields.
- Binary lookup preserves system PATH selection, external-over-managed precedence, install-directory lookup and missing-binary errors. Release fixtures exercise repository versus asset phases with different pinned/resolved versions, every backend/OS/architecture branch, TheRock trimming and errors, ROCm family mapping, CUDA fact failure and unknown-helper-backend Vulkan fallback.
- System fixtures preserve command spelling, version parsing, first-line and buffer behavior, failed pipe creation, ignored exit status, HIP override validation, executable PATH precedence, adjacent/sibling plugin discovery, fixed system paths and KFD-dependent checks.
- Tiny generated GGUF files and cache trees exercise magic checks, registration variants, metadata/labels, non-LLM label isolation, wildcard/exact/suffix/quant/folder/shard resolution, ambiguity, active/older snapshots, deduplication, partial downloads, live manifests, import filtering and flat-cache fallback.

## Protocol and ordering

The eleven bound operation cells are eight buffered operations and three streaming operations. The binding table and full supported-operation/model/delivery matrix are frozen. The HTTP methods, paths, bodies, returned model IDs and raw stream bytes are checked through both the observation seam and an independent loopback transport test against the production wrapper.

| Operation | Exact hook order |
| --- | --- |
| Buffered chat | alias, sanitizer, HTTP forwarding, model normalization |
| Buffered completion | alias, HTTP forwarding, model normalization |
| Buffered responses | sanitizer, HTTP forwarding, model normalization |
| Streaming chat | alias, sanitizer, streaming forwarding |
| Streaming responses | sanitizer, streaming forwarding |
| Streaming completion | streaming forwarding with the original body bytes |
| Embeddings, reranking, slots, slot action, tokenize | Direct forwarding with no alias, sanitizer or model normalization |

Hook observations run inside the actual transformation functions, so changing an order fails even where the transformed JSON happens to commute. The fixtures include existing versus absent aliases, client versus loaded model names, non-object responses, nested sanitizer boundaries, malformed streaming JSON, primitive JSON and unknown streaming endpoints. Streaming cases cover both SSE and byte forwarding, default and explicit timeout, and telemetry callback presence.

Failure fixtures preserve the first error and the entire effect prefix. In particular, backend validation precedes installation; installation precedes a missing main artifact; missing main precedes port and program resolution; binary resolution precedes device validation; device validation precedes reserved-argument validation; reserved arguments fail before environment construction or launch. Within custom arguments, the first offending token wins and the sorted reserved list is exact. Release-fact failures also retain their order relative to unsupported-platform errors.

## Running the gates

```bash
cmake --preset default
cmake --build --preset default --target lemonade lemond cpp-ci-tests
ctest --test-dir build -L '^cpp-ci$' --output-on-failure
python3 test/llamacpp_oracle_audit.py equality --out /tmp/llamacpp-equality
python3 test/llamacpp_oracle_audit.py mutations --out /tmp/llamacpp-mutations
```

The audit uses temporary compiled sources and replacement objects; it does not edit the working wrapper or weaken assertions. Its eight mutants change a managed flag, the parallel default, hook order, an error message, a reserved alias, the context floor, readiness endpoint and installation/error ordering. Each must compile successfully and then exit with a fixture mismatch.

The existing request test is now included in `cpp-ci` and links the server core so its out-of-line token-alias dependency is satisfied. Its sanitizer assertions remain unchanged. Existing HRX and other configured-backend oracle assertions remain unchanged.

## Separate unit 4h qualification

CPU fixtures establish decisions and transport contracts. They do not establish native MSVC/AppleClang compilation; Windows filesystem, DLL search/staging and SVM behavior; Metal execution, residency and native HOME lookup; real ROCm stable/nightly, CUDA or Vulkan loading/offload and driver compatibility; real model acceptance or quality for vision/HF/draft/DFlash/MTP; actual system HIP plugin loading; HRX inference, cancellation and unload/reload on supported AMD devices; or the real external-fork executable/model product proof. No new NPU claim is made.

The design's bounded replacement for the existing POSIX shell version probe is deliberately deferred. This unit freezes its current command, parser and read behavior; adding a five-second/64-KiB bound would change production behavior and needs a separate correction. Fake probe success does not prove probe liveness or real system installation health.

## Unit 4b qualification record

The final Linux CPU run passed the following gates against the same source tree. Detailed local logs are under `.synagent/artifacts/backend-v2-phase4b/`.

| Gate | Result |
| --- | --- |
| Configure and `lemonade lemond cpp-ci-tests` build | Pass |
| `ctest -L '^cpp-ci$'` | 132/132; baseline 123; no assertion removed or weakened |
| Four wrapper views | 476 cases each, 11 binding cells and the complete operation matrix, 32 buffered and 40 streaming transcripts each |
| Three independent descriptor views | Complete ordered field equality |
| Production-wrapper loopback transport | 150/150 assertions |
| Existing llama.cpp request assertions and gfx950 gate | Pass; request test now participates in CI |
| Existing configured/HRX and all prior C++ suites | Pass in the full CI run |
| Prior execution/compiler/bounds/boundary/export/authorization/resolver/descriptor/v1/ABI/admission suites | All 15 direct invocations pass |
| Catalog HTTP / frozen CLI / CLI REPL / negotiation | 115/115, 104/104, 58/58, 88/88 |
| Data/ABI parity | Native, Windows and macOS views: 17 descriptors, eight production and three synthetic mapper cases each; exports remain empty |
| Renderer catalog / recipe form / schema options | 24/24, 83/83, 22/22 |
| Additional app regressions / TypeScript | 136 passed, zero skipped/failed; typecheck passes |
| Generator `--check` | Pass |
| `lemond` smoke | Health/schema 200, 17 recipes, eight definitions, clean exit |
| Mutants | 8/8 compile and are caught by fixture mismatches |
| Syscall audit, each wrapper view | One initial executable, zero child executable launches, zero network calls, zero accelerator-path accesses |
| Legacy snapshot / schema / eight digests / ETags | Separate differences are all zero bytes; whole snapshot also compares byte for byte |
| Original-source code generation control | All four objects identical; no production symbols change |

The controlled `lemond` SHA-256 is `56016a29dbb411039ebf7c0c5887afb53c920daf965cf396da2632e0addaf156`. The controlled CLI SHA-256 is `88c067ad4f2ced7648c5a9c38e16dc15d20151831897e5065377cefd9d09872b`. Both match the original-source control links with identical generated build-version data. The binaries present when the task began had different hashes; they are not used as the code-generation control.

No real engine, model, inference, benchmark, GPU or NPU ran. Loopback servers used isolated ephemeral ports. The existing port-8000 service was not changed. Native-platform and real-engine qualification remains the separate unit 4h gate described above.
