# Strata engine

Lemonade runs the [Strata](https://github.com/Niko1221/Strata) native inference
engine as a subprocess. It installs only the GPU-specific native engine package.
There is no Strata app, Control runtime, Python interpreter, pip environment,
setup script, or separately managed Strata service.

Engine binaries are downloaded from
[upstream Strata releases](https://github.com/Niko1221/Strata/releases)
for officially supported GPU/OS combinations.
The draft vocabulary and expert profiles are fetched from the official Strata
repository at the backend's release tag in
[`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json).
Model weights, projectors and draft heads use their Hugging Face repositories'
default revisions; Lemonade does not maintain separate runtime commit, hash or
size pins. Available registry-provided hashes are checked during downloads.

Unlike FastFlowLM, the engine speaks a token-ID pipe protocol rather than HTTP.
Lemonade supplies the adapter entirely in C++: its existing downloader fetches
GGUF shards, a native pack builder prepares the dense weights and expert
index, and a direct C++ adapter translates requests to `GEN`/`T`/`DONE` messages.
Strata does not bind an internal HTTP port: only Lemonade's public API listens
for requests. Engine readiness comes from its `READY` message, and the backend
watchdog monitors the child process without HTTP health probes.
It sends `STOP` when a client disconnects and drains the request before reusing
the engine. Requests to one engine are serialized.

The adapter uses a pinned llama.cpp C++ library for vocabulary-only tokenization,
chat templates and output parsing. It does not start llama-server or run inference
inside Lemonade. All model inference remains in the Strata engine subprocess.

Pictures use the packaged native `strata-vision` subprocess as well. Lemonade
downloads the compatible mmproj weights and sends encoded image features to the
engine through `GENI`; neither process opens an HTTP listener.

## Install and run

With `lemond` running:

```sh
lemonade backends install strata:rocm
lemonade pull Qwen3.8-Flash-Next-Q2_0-Strata
lemonade run Qwen3.8-Flash-Next-Q2_0-Strata --strata rocm --ctx-size 8192
```

Use `strata:cuda` / `--strata cuda` for NVIDIA CUDA 12, `strata:cuda13` /
`--strata cuda13` for CUDA 13, or `strata:metal` / `--strata metal` on Apple Silicon.
The usual automatic backend selection also works. Backend installation is automatic
when a model is first pulled or loaded.

Automatic context sizing uses the selected engine release's `default_context`
(currently 8192 tokens), capped by the model's context limit. It does not apply
the fully GPU-resident model estimator: Strata sizes its expert cache and prefill
for its disk-streaming/hybrid execution path. An explicit `ctx_size` is preserved.

For downloaded models, Lemonade reads the supported context window from the
original GGUF metadata, including sharded, direct-GGUF and ready-pack selections.
The API exposes it as `max_context_window`, which sets the Web UI's context
control maximum. This model limit is separate from the automatic default and
does not guarantee that the selected context fits available memory.

### KV cache precision

On CUDA/ROCm Flash-Next models, `--strata-kv` (API recipe option `strata_kv`)
accepts `auto`, `fp16`, `int8`, `q4_0`, or `k8v4`. The default, `auto`, follows
Strata setup's recommendations: FP16 through 8192 context tokens, INT8 above
8192. The thresholds, recommended formats and backend support are release
metadata, not fixed C++ constants. An explicit format is preserved at any context.

INT8 reduces cache memory relative to FP16. Q4_0 uses rotated 4-bit K/V, with
less memory but reduced precision; K8V4 keeps INT8 keys and rotated 4-bit values.
For example:

```sh
lemonade run Swift-1.5-IQ2_XS-Strata --ctx-size 131072 --strata-kv int8
```

Use the dedicated option rather than putting `--kv` in `--strata-args`.
Metal and the separate Qwen3.6 engine path retain their native cache defaults;
explicit precision selection is rejected there rather than silently ignored.
KV precision and RAM-backed KV streaming are independent settings.

### KV cache streaming

`--strata-kv-streaming auto|on|off` (API option `strata_kv_streaming`) controls
RAM-backed KV streaming on CUDA/ROCm Flash-Next. The default `auto` follows
Strata's recommendation: from 65536 context tokens, keep the full KV cache in
RAM and a 32768-token window in VRAM. Selection follows setup's RAM-capacity
recommendation: the model's recommended
RAM, the full host KV allocation and 1 GB of additional headroom must fit total
system RAM. This recommendation applies to `auto`, `mmap`, and `resident`
expert modes; it does not measure free RAM or Windows commit. Thresholds, KV
geometry, model RAM recommendations and the VRAM window are release metadata.
KV streaming frees VRAM for GPU-cached experts but consumes pinned RAM.
Short contexts, unknown RAM capacity, custom engine arguments, unified-memory
GPUs, models without a RAM recommendation, and WSL disable automatic streaming.
WSL's driver cannot pin the required host copy.

```sh
lemonade run Swift-1.5-IQ2_XS-Strata --ctx-size 262144 --strata-kv int8 --strata-kv-streaming auto
```

Explicit `on` bypasses the capacity recommendation; it still
requires a context at or above the streaming threshold. WSL, Metal and the
separate Qwen3.6 path reject explicit streaming settings they cannot support.
Explicit `off` keeps KV in VRAM. Existing custom `--kv-resident` arguments remain
available, but cannot be combined with an explicit `--strata-kv-streaming on`
or `off`.

### CPU expert residency

`--strata-experts auto` (API recipe option `strata_experts`, the default) selects
Strata's native full resident arena for qualified GSQ-RCO, Swift and Coder
variants. It removes `--mmap-experts` rather than forcing the low-RAM
`--resident-experts` variant. Other Flash-Next variants retain mmap unless qualified
for the RAM-budget policy below. Qualification
is explicit in model metadata (`full_expert_arena`); no per-model arena-size
estimate is needed.

Unsloth Flash-Next and the two prepacked NVFP4 variants also qualify for
automatic expert residency on discrete GPUs. Lemonade measures expert bytes from
GGUF tensor headers or the downloaded expert pack, rather than maintaining
per-quant size estimates. If all experts fit beside 16 GiB of headroom and any
streamed host KV allocation, it selects the full arena. Otherwise it uses
`--resident-budget-gib`, leaving 24 GiB for the OS, engine and file cache, and
deducting streamed KV separately. Budgets below 8 GiB retain mmap instead of
forcing an allocation. These thresholds live in the Strata metadata.
For example, 64 GiB of RAM and an expert pack larger than 48 GiB select a
40 GiB budget when KV streaming is off.

Strata 0.2.7 manages PLE/ngram residency inside the engine. Lemonade does not
inject `--ple-io ram`: the engine checks available host memory, including
cgroup and engine memory limits, against the actual table size, the expert
arena still to allocate and 8 GiB of headroom. Unknown sizes or memory telemetry
keep direct reads. The engine handles mapping failures. Gains depend on the GPU
and storage.
An explicit `--ple-io ram`, `--ple-io mmap` or `--ple-io direct` in `strata_args`
takes precedence. `STRATA_PLE_AUTO_RAM=0` disables the native residency policy.
On shared-memory GPUs, Lemonade defaults to `--ple-io direct`: the engine's
host-memory admission check does not reserve the later GPU expert-cache allocation,
which competes for the same physical RAM. This default also applies with unrelated
custom arguments; an explicit `--ple-io` still wins. Discrete GPUs, Metal and
direct-GGUF models retain their native PLE behavior.

Automatic expert-budget recommendations apply only to
`--strata-experts auto` without custom `strata_args`. Explicit expert modes and
custom tuning retain control.

Lemonade logs the selected modes and total RAM/VRAM capacity, and warns when
native allocations may consume significant RAM. It does not monitor available
RAM/commit, fall back to mmap, reserve memory, or modify page-file settings.
Native allocations can cause paging, allocation failure or an unresponsive
system when memory is tight.

Force file-backed access to conserve RAM, or request resident experts explicitly:

```sh
lemonade run Swift-1.5-IQ2_XS-Strata --strata-experts mmap
lemonade run Swift-1.5-IQ2_XS-Strata --strata-experts resident
```

Explicit `resident` requests Strata's low-RAM cache-complement mode and may
consume tens of GB. The engine can fall back
to a partial copy plus file access if it cannot allocate the whole complement.
Use it only with sufficient RAM and commit capacity. Use this option instead of
`--mmap-experts`, `--resident-experts` or `--resident-cpu-experts` in `strata_args`.
Metal and the separate Qwen3.6 path retain their existing memory behavior and
reject explicit expert-mode selection.

The default VRAM reserve follows the desktop app's work-profile recommendation:
1536 MiB on display/unknown-display cards of 16 GiB or less; on larger discrete
cards, the greater of 1024 MiB and 20% of VRAM. Unified-memory cards use the
display floor rather than reserving a fifth of shared RAM. Unknown card capacity
retains the engine's reserve. Recommendations live in release metadata. An
explicit `--vram-reserve-mib` in `strata_args` takes precedence.

On a 64 GiB RAM / 16 GiB VRAM PC, the defaults select full-arena experts, INT8
streamed KV and a 1536 MiB VRAM reserve for a 262144-token context, without
custom tuning:

```sh
lemonade run Swift-1.5-IQ2_XS-Strata --ctx-size 262144
```

Use `--strata-experts mmap` and/or `--strata-kv-streaming off` when RAM or commit
is tight.

### CPU expert workers

When the OS exposes a recognized hybrid CPU topology with more physical
efficiency cores than performance cores, Lemonade recommends
`P - 1 + floor(E / 2)` expert-pool workers, capped at 32 by release metadata.
This follows upstream's deterministic recommendation; it does not benchmark
inference or pin threads. Windows uses processor efficiency classes; Linux
uses physical core IDs and core types exposed in sysfs, counting SMT siblings
only once. Unknown or balanced topologies keep engine defaults. Custom
`strata_args`, including `--pool-workers` or `--cpu-threads`, disable the
automatic recommendation. Metal keeps its existing behavior.

| Backend | OS / CPU | Engine selection |
|---------|----------|------------------|
| `cuda` | Windows / Linux x86-64 | CUDA 12, for broader driver compatibility |
| `cuda13` | Windows / Linux x86-64 | CUDA 13; NVIDIA driver 580+ |
| `rocm` | Windows / Linux x86-64 | RDNA2 (`gfx1030/1031`), RDNA3 (`gfx1100/1101/1102`), Strix Halo (`gfx1151`), or RDNA4 (`gfx1200/1201`) |
| `metal` | macOS 15+ / ARM64 | Apple Silicon Metal |

Unsupported architectures fail before downloading an asset. Intel SYCL, Intel
Macs, ARM Windows/Linux, and CPU-only execution are not exposed by this integration.
The backend is experimental; upstream's hardware qualification differs by target.

AMD detection can report family identifiers (`gfx103X`, `gfx110X`, `gfx120X`)
instead of a concrete GPU architecture, particularly on Windows. These identifiers
are accepted for discovery and resolve to the corresponding RDNA2, RDNA3, or RDNA4
engine archive, so supported GPUs remain visible and installable in the Web UI.

## Models and API

Strata keeps its downloaded weights and prepared files in a private `strata`
subdirectory of the repository cache. Those files do not mark a separate
llama.cpp catalog entry as downloaded, even when both entries use the same
Hugging Face repository. A llama.cpp entry becomes downloaded when its own
registry artifacts are present.

The registry includes 21 engine selections, all with names ending in `-Strata`:

| Family | Variants | Approximate download | Backends |
|--------|----------|----------------------|----------|
| Qwen3.8-Flash-Next GSQ-RCO | `Q2_0`, `IQ2_XS`, `IQ3_XXS`, `IQ3_S` | 66-84 GB | CUDA, ROCm, Metal |
| Swift 1.5 | `IQ2_XS`, `IQ3_XXS`, `IQ3_S` | 68-84 GB | CUDA, ROCm, Metal |
| Qwen3.8-Flash-Next Coder | `IQ1_M` (256 experts) | 58 GB | CUDA, ROCm, Metal |
| Qwen3.8-Flash-Next Unsloth | `UD-IQ4_XS`, `UD-Q4_K_XL`, `UD-Q5_K_XL`, `UD-Q6_K_XL`, `Q8_0` | 94-188 GB | CUDA, ROCm, Metal |
| Qwen3.8-Flash-Next Gyro | `Gyro-S`, `Gyro-M` | 58 / 92 GB | CUDA, ROCm, Metal |
| Qwen3.8-Flash-Next Gyro-Coder | `Gyro-Coder` (256 experts, TQ2_T) | 76 GB | CUDA, ROCm, Metal |
| Qwen3.8-Flash-Next NVFP4 | `NVFP4` (GPTQ), `NVFP4-NVIDIA` | 128 GB | CUDA only |
| Qwen3.6-35B-A3B | `UD-Q4_K_XL`, `UD-Q6_K_XL`, `Q8_0` | 23 / 33 / 38 GB | CUDA, ROCm |

For example, use `Swift-1.5-IQ3_S-Strata`,
`Qwen3.8-Flash-Next-Coder-IQ1_M-Strata`,
`Qwen3.8-Flash-Next-Unsloth-UD-Q4_K_XL-Strata`,
`Qwen3.8-Flash-Next-Gyro-S-Strata`, or
`Qwen3.6-35B-A3B-UD-Q4_K_XL-Strata`.
Both CUDA 12 and CUDA 13 variants are supported where CUDA is listed.
Automatic selection respects model-specific backend restrictions, and models
requiring an unavailable GPU family are filtered from the supported catalog.
Explicit incompatible backend choices fail before downloading or starting the engine.

Swift `Q2_0` is not offered because upstream cannot prepare its split experts.
`NVFP4-QAD` (`nvidia/Qwen3.8-Flash-Next-FP4-Local`) is deferred: it is a
safetensors checkpoint requiring a separate converter, not a ready-made Strata
pack. Lemonade does not invoke upstream's Python/PyTorch conversion pipeline.
Arbitrary llama.cpp GGUFs are not interchangeable with Strata's supported models.
Each model remains subject to its upstream repository's license.

Gyro-Coder is available as `Qwen3.8-Flash-Next-Gyro-Coder-Strata`:

```sh
lemonade pull Qwen3.8-Flash-Next-Gyro-Coder-Strata
lemonade run Qwen3.8-Flash-Next-Gyro-Coder-Strata --strata rocm
```

Its 256-expert TQ2_T weights and explicit block-128 Hadamard rotations use the
existing native Gyro preparation path. It shares the original model's picture
reader and external MTP pack. Neither the full model's nor the GSQ-RCO Coder's
static expert profile matches its pruned expert selection, so it uses the
engine's profile-free cache admission (experts are admitted as routed).
`strata_experts=auto` retains mmap; explicit `resident` requires a compatible
static profile and is rejected. Download size includes the large on-disk n-gram
table and is not a GPU-memory requirement.

Gyro-Coder-L is not offered by the pinned engine: its GGUF uses newer tensor
types 147-149, whereas this engine supports Gyro types 144-146.
The model card still describes Strata support as forthcoming; native preparation
and launch configuration are regression-tested here, but full-model inference
quality and performance have not been hardware-validated.

Allow additional disk space for preparation. For Flash-Next, upstream recommends at least
32 GB combined RAM/VRAM; smaller memory budgets use disk-backed experts and can
be substantially slower. Memory filtering uses a conservative working-set floor,
not the full download size; this is not a guarantee that every context fits.
Flash-Next selections require at least 32 GB across system RAM and the largest
dedicated GPU's VRAM. Unified memory is counted only once. The direct Qwen3.6
selections instead use their 32 / 44 / 52 GB system-RAM requirements.
Compatible selections automatically add a Strata backend filter to the Models page;
CUDA-only NVFP4 selections remain hidden on AMD-only systems.

Models, prepared packs and configuration are stored under the configured
Lemonade models directory, in a per-quant `strata` subdirectory of the model's
repository cache. Interrupted downloads can be resumed by pulling again.
GGUF experts are read directly from the original shards. The expert residency
policy chooses file-backed access or the native RAM arena without a conversion.
Unsloth and Gyro preparation dequantizes required small dense projections to the
engine's float formats, with nearest-even BF16 rounding; expert bytes stay unchanged.
Gyro's custom quant types and Hadamard activation rotations are preserved in a
v5 expert index and `hadamard.txt`.

On Metal, when non-PLE tensors span multiple shards, Lemonade additionally writes
a page-aligned `metal.gguf` for the GPU. This happens on first load and needs
space for another copy of all non-PLE tensors (potentially over 100 GB).
The PLE table remains in its original shard. Subsequent loads reuse the repack.
NVFP4 ready-made packs retain their repository directory layout and use separate
dense, PLE and embedding GGUF inputs; Lemonade does not reconvert their weights.
Qwen3.6 reads its GGUF directly without a dense pack or expert-cache profile.
Unlike Flash-Next's storage-streaming path, its hybrid mode keeps CPU experts in RAM;
upstream recommends 32 / 44 / 52 GB RAM for the three variants respectively.
Tokenizer artifacts are prepared natively from GGUF metadata in every mode.
Preparation progress is reported as an activity; detailed upstream output is in
Lemonade's server logs. Cancelling a pull stops downloading or native preparation;
an incomplete pack is never marked downloaded.
Deleting the model removes its managed quant directory.

On CUDA and ROCm, Flash-Next selections use an MTP draft head by default, alongside
the engine's existing prompt-lookup speculation (`--spec 4`). The
prepacked Q2_0 draft head is downloaded natively, with no
Python or conversion step. Its approximately 824 MB download is shared across
compatible model selections and retained when an individual model is deleted.
Lemonade fetches the multilingual vocabulary from `data/draft_vocab.bin` in the
official Strata repository at the configured backend release. The draft weights
come from their Hugging Face repository's default revision. Source repositories,
relative filenames, compatible backends and draft confidence settings live in
[`strata.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/strata.json), without duplicated hashes,
sizes or commit pins.

The draft head needs roughly another 1 GB of VRAM, reducing the room available
to the expert cache; the speed benefit depends on the hardware and request.
Disable it with `--no-strata-mtp` or `"strata_mtp": false` in load/model recipe
options to compare performance or save memory. Prompt lookup remains enabled.
If an enabled draft pack is unavailable offline, loading fails explicitly;
download it online first or disable MTP. Metal still uses prompt lookup without
the external draft head: its MTP loader requires a different artifact format.
Qwen3.6 uses its GGUF's embedded MTP block, not a separate draft GGUF or the
external pack. The same `strata_mtp` switch controls it: enabled passes the
model's configured draft count (currently `--mtp 2`), disabled passes `--mtp 0`.
The engine still loads the embedded block's weights when drafting is disabled,
so turning off drafting does not necessarily reduce VRAM usage.

Strata models must not set `checkpoints.draft`: that role is used for separate
draft GGUFs by other inference backends. Strata rejects it before downloading;
its compatible draft pack or embedded block is selected from model metadata.
The Hugging Face browser's generic GGUF import uses the `llamacpp` recipe;
use the curated `*-Strata` entry to run that model with Strata instead.

### Model-specific engine flags

The pinned engine supports the curated Qwen3.8 variants and Qwen3.6-35B-A3B.
It is not a general-purpose loader for arbitrary MoE architectures; new families
in newer Strata releases require an engine and adapter compatibility update.

Qwen3.6 has different memory and draft controls from the packed-model engine.
It keeps its native KV precision and hybrid expert placement. Explicit
`strata_kv`, `strata_experts` and `strata_kv_streaming` policies unsupported by
that path are rejected rather than translated into unrelated flags.
Its `strata_args` accepts `--prefill` / `--chunk`, `--cpu-threads` /
`--pool-workers`, `--expert-vram`, `--expert-slots`, `--hybrid-rows`,
`--draft-vocab`, `--gpu`, `--no-graph` / `--no-token-graph` and `--no-warmup`.
Use space-separated flag values, for example:

```sh
lemonade run Qwen3.6-35B-A3B-UD-Q4_K_XL-Strata --strata rocm --strata-args "--prefill 512 --cpu-threads 8"
```

Unknown Qwen3.6 flags, including packed-model `--spec`, `--expert-cache`,
`--spec-min-p`, `--kv-resident` and `--vram-reserve-mib`, fail before a download
or load instead of being silently ignored by the engine. GPU picture encoding
does not inject the packed engine's reserve flag into this path.
Lemonade serializes requests to each engine; native batch/slot flags and
`--batch-mtp` are not exposed through this adapter. Context, model paths,
vision input and `--mtp` remain managed options, including their native aliases.

Chat and Responses results expose the engine's prefill/decode `timings`, including
drafted and accepted token counts. Chat usage also includes
`prefill_duration_ttft` (seconds) and `decoding_speed_tps`. Streaming chat always
attaches timings to the terminal finish chunk, including `cache_n` and the
uncached `prompt_n`. Like the Strata server, it defaults to sending a final usage
chunk so the official benchmark can read token counts without special options.
An explicit `stream_options.include_usage: false` suppresses usage but not timings.
The CLI benchmark uses engine decode throughput rather than total request time,
which includes prefill.

The engine silence timeout during prefill is five minutes plus 100 ms per prompt
token, capped at fifteen minutes. After the first output token or a cancellation
request, the standard five-minute silence timeout applies.

Chat completions, tool calling, image inputs, and the stateless Responses API are
sent directly to Strata, including streaming. Legacy text completions and embeddings
are not enabled. Lemonade controls the context, model, engine path and process
lifetime. Text/image messages and function tools are accepted;
constrained decoding, logprobs, and forced tool selection are explicitly rejected.
Additional engine tuning flags can be supplied with
`--strata-args`; managed arguments cannot be overridden.

Serving never acquires additional executables. Lemonade's own API authentication
remains the client-facing boundary. Model preparation runs natively and can run
offline when downloaded weights and engine data have matching download receipts.
Engine upgrades preserve verified Hugging Face model weights, prepared packs,
MTP weights and vision projectors. Existing release-scoped MTP and vision cache
directories are reused in place. Strata-owned expert profiles and draft vocabulary
live beside the engine under `lemonade-data/`, preserving their repository-relative
paths (for example `lemonade-data/data/expert-profile.bin`). Every model using that
engine reads this shared copy; MTP uses `--mtp-draft-vocab` to keep its weights in
the Hugging Face cache while reading vocabulary from the engine directory.
Each official engine installation or update downloads all configured auxiliary files from
the selected release tag into the engine staging tree before promoting it.
If downloading or preparation fails, the previous engine and its data remain
untouched and the operation reports an error. No per-model cache updates, weight
downloads or projector downloads occur during this step.
For existing engines without matching auxiliary data, the first load populates a staged
engine-data directory and promotes it atomically. Offline loading requires that
data to be present already. Old per-model auxiliary copies remain untouched but
are no longer used, and do not affect whether a model is listed as downloaded.
Normal backend installation still honors offline/no-fetch-executables policy.

## Vision

All catalog selections advertise vision and have a compatible projector:
Swift uses its own fine-tuned mmproj, original Flash-Next/Coder/Unsloth/Gyro/NVFP4
share the original projector, and Qwen3.6 uses its own compatible mmproj. These
files are approximately 0.9 GB each. Pulling/loading a model downloads the
projector from its repository's default revision when vision is enabled, checking
the registry-provided hash when available. It is not folded into
the text model's native expert pack.

`--strata-vision cpu|gpu|off` (API recipe option `strata_vision`) defaults to `cpu`.
The CPU reader matches the official setup on a 16 GB card and leaves the GPU's
expert cache available for text decoding. To opt out:

```sh
lemonade run Swift-1.5-IQ2_XS-Strata --strata-vision off
```

The packaged HIP reader on ROCm and Metal reader on macOS can be selected with
`--strata-vision gpu`. An unavailable/failed requested reader is an explicit error,
not a silent placement change. The current CUDA packages provide the CPU reader
only. GPU vision uses the app's 2048 MiB reserve floor unless an explicit
`--vram-reserve-mib` override is supplied; CPU vision does not increase that floor.
The native reader starts and warms up before the text engine sizes its expert
cache. Release metadata supplies the upstream image-token budgets: 300 on CPU,
1024 on GPU.

Chat accepts OpenAI `image_url` content parts; Responses accepts `input_image`
parts with an `image_url` string. Send a base64 `data:image/...;base64,...` URL or
an HTTPS image URL. JPEG, PNG, BMP and GIF are handled by the native decoder.
Formats requiring Strata's optional Python/Pillow conversion are not converted
by Lemonade and return an explicit decoder error. Local file paths, `file:` URLs,
plaintext HTTP URLs and malformed base64 are rejected. Remote images are limited
to 20 MiB with bounded download time; offline mode requires inline base64 images.
Each request accepts at most 64 images and 256 MiB of combined encoded features.
Image tokens count toward the model's context window.

Encoded features are cached by image-content SHA256 (up to 64 pictures per loaded
model), so resending conversation images does not re-encode them. Stable image
keys are passed to the engine for its conversation/KV cache. Encoding and text
generation share one serialized request turn; they never run concurrently on the
GPU. Cancellation stops an active encoder without resetting the text engine, and
unload stops both subprocesses. Raw images and request feature files are temporary;
the per-model encoded-feature workspace is removed on unload. The processes share
a private temporary working directory so protocol filenames work even when the
user's paths contain spaces or Unicode. Use absolute paths in custom engine flags.

Verified projector downloads are shared across compatible model selections and
retained when deleting one text model, like the shared MTP pack. Offline reuse
requires the matching verified-download receipt. Changing projector provenance
invalidates that receipt without forcing text weights to be rebuilt.

## Updating and testing

### Routine engine upgrades

Update the relevant `strata` release tags and verified engine-archive checksums in
[`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json).
For example, a compatible release bump changes:

```json
"strata": {
  "cuda": "v0.2.7",
  "cuda13": "v0.2.7",
  "rocm": "v0.2.7",
  "metal": "v0.2.7"
}
```

These are [upstream Strata release tags](https://github.com/Niko1221/Strata/releases),
not the independently versioned engine number
embedded in archive names. During installation, Lemonade queries that exact
release, selects the archive matching the OS/CPU/GPU pattern, and verifies the
download against the pinned SHA-256 checksum when available, falling back to the
asset's published SHA-256 digest for unpinned releases. Verify new checksum pins
against the upstream release's `SHA256SUMS.txt` and GitHub asset digests.
There is no separate engine-version pin. Missing/ambiguous assets, missing
or invalid SHA-256 digests, API failures and hash mismatches fail explicitly.
There is no fallback to a different release or unverified download.

The current `v0.2.7` release packages Qwen engine `0.1.52`; it does not select
the separately packaged DeepSeek engine or the Strata app.

The update inherits upstream's native performance and correctness changes:
residency-biased routing on discrete NVIDIA GPUs of 13 GiB or less, gfx12
decode/prefill defaults, CUDA layer-split prefill and batching improvements,
conversation-cache isolation fixes, PLE reader recovery and Qwen3.6 sampling
penalty fixes. Some defaults change floating-point results or expert selection,
so generated text can differ. These changes do not add shared-prefix or repeated-
prompt checkpoint reuse to the separate Qwen3.6 engine.

For comparison with the previous engine defaults, set `STRATA_DEFAULTS=026`
in the server's environment. Explicit tuning still wins. The narrower
`STRATA_ROUTE_PRIOR=0` switch disables residency-biased routing, and
`STRATA_GFX12_DEFAULTS=0` disables the gfx12 architecture defaults.
Approximate long-prompt processing (`STRATA_KVA=1`) remains opt-in; Lemonade does
not enable it. Environment settings must be present in the server process
before it launches the engine.

GitHub metadata is queried only when installation or replacement is needed.
Already-installed compatible engines still load offline; ordinary loads and
automatic context sizing do not query GitHub. GitHub API rate limits apply to
installation; `GITHUB_TOKEN` or `GH_TOKEN` can raise the limit.

### Model or adapter changes

Stable metadata lives separately in
[`strata.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/strata.json), independent of release tags:

| Section | Contents |
|---------|----------|
| `engine` | Asset pattern/targets, pipe protocol, launch arguments, default context, KV geometry/capacity recommendations, VRAM reserve and vision limits |
| `models.<repository>` | Model filenames, preparation/conversion settings, supported backends, launch arguments, release-relative expert-cache profile, MTP/projector repositories or shared references, per-quant full-arena qualification and RAM recommendation |
| `quant_types` | Custom engine quant type IDs and block geometry, without inference kernels |
| `native_utilities` | Build-time llama.cpp archive URL and SHA-256 |

Asset patterns accept `{platform}`, `{variant}`, and `*` for the independently
versioned engine component. The target matrix selects variants by backend and
GPU architecture (`default` for targets without a GPU-specific variant).

To update a checkpoint's layout, change its repository, shard list or relative
profile path. There is no fixed shard count; list the primary GGUF shard
first. Add compatible model/quant entries
to `models` and the public model definitions to
[`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json).
`preparation` is `gguf-pack` (default), `native-gguf`, or `ready-pack`.
`compat_bf16` enables upstream-compatible dense conversions.
`backends` limits hardware choices; `arguments` replaces the release's tuning
arguments for a different engine path, such as Qwen3.6.
`native_file`, `ple_file`, and `embedding_file` name downloads used as engine inputs.
Ready-pack download paths are preserved; other modes keep the existing flattened
GGUF layout. `expert_profile_from` reuses another repository's profile metadata.
Optional `quant_options.<quant>` overrides preparation settings for one selection.
Only models compatible with the native pack builder and engine can be added this
way; protocol or tensor-layout changes still require C++ changes. New advertised
OS/GPU capabilities also require updating the backend descriptor and regenerating
the backend reference.

Each quant directory has a `lemonade-model.json` receipt recording the
weight-download identity and actual file sizes, independent of engine release and
expert profile. Legacy receipts containing those fields remain usable.
Changing the repository, revision,
shard list, preparation mode, conversion policy or model-weight inputs invalidates
that identity and requires an online pull. Engine release and expert-profile
changes do not invalidate model weights. The previous pack and Metal repack
receipt are invalidated before
downloading a changed identity, so same-sized replacement weights cannot reuse
stale dense data. A successful download can be prepared again offline after an
interruption. Changing `native_utilities` requires rebuilding Lemonade; CMake reads
the pin from the same metadata resource. Keep the upstream llama/llama-common targets
rather than maintaining a private source list. This dependency supplies GGUF,
tokenization/templates and subprocess utilities, not inference.

Release-only bumps replace engine-owned auxiliary data without invalidating
model/preparation, MTP-weight or projector receipts. Successful weight receipts
permit offline reuse across engine releases without querying repositories.
Delete and re-pull a model to refresh its Hugging Face default-revision weights.
This intentionally trades immutable model reproducibility for simpler maintenance;
upstream changes to filenames, layouts or compatibility still need metadata or
adapter updates. Publish updated resources with Lemonade; compatible upgrades do
not require adapter recompilation. For an engine upgrade, verify
`READY`/`GEN`/`GENI`/`STOP`/`DONE`, vision `ENC`,
sampling/timing fields, and native pack layouts against the new engine. A changed
protocol or tensor layout requires updating the adapter and fixtures; do not
merely bump the accepted protocol number. Archive naming, supported hardware or
recommended defaults changing also requires updating the corresponding stable
metadata. If native preparation output changes,
bump its receipt schema so stale packs are rebuilt. Auxiliary download receipts
are separate, so changing only a projector/drafter source does not rebuild text weights.

Run native inference tests manually on a suitable GPU with sufficient disk and
memory (these large downloads are not part of packaging CI):

```sh
python test/server_llm.py --wrapped-server strata --backend rocm
```

For a small native network smoke check without model weights or inference, build
`test_strata` and run `test_strata --release-data rocm` (use
`build\Release\test_strata.exe` on Windows). This downloads the Strata-owned data
files from the configured backend release into an isolated temporary directory,
checks the download receipts, and removes the directory afterward. The ordinary
`StrataTest` suite remains hermetic.

`StrataTest` creates synthetic GGUF shards, prepares a native pack and vocabulary,
and runs the direct C++ adapter against a C++ engine-protocol fixture subprocess.
An HTTP frontend exists only in the test harness to exercise client disconnects.
It covers all 21 catalog selections, hardware restrictions, direct GGUF and
ready-pack completeness, native pack offsets, compat-BF16 conversions, Gyro
custom types and Hadamard rotations, byte-exact Metal repacking, model presence/deletion,
chat and Responses, reasoning and tools, streaming event consistency, UTF-8,
sampling validation, concurrent requests and streaming/non-streaming disconnect cancellation.
The native vision fixture additionally checks projector provenance, multimodal
Chat/Responses and streaming, ordered image features, image-token expansion,
feature-cache reuse, literal image markers, invalid images and embedding files,
space/Unicode paths, encoder cancellation and concurrent unload.
No Python runtime, GPU or model download is needed for this test. Protocol-fixture
tests are not a native GPU inference qualification.
