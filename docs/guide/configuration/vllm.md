# vLLM Backend Options

Lemonade integrates [vLLM](https://github.com/vllm-project/vllm) as an experimental backend for AMD ROCm GPUs on Linux. vLLM brings two core benefits:

1. **Day-0 model support.** vLLM typically supports new transformer architectures within hours of their release on Hugging Face — checkpoints load directly, with no per-architecture porting.
2. **Concurrency and multi-GPU.** Paged-attention KV cache, continuous batching, and chunked prefill scale aggregate throughput with in-flight request count; tensor and pipeline parallelism are supported across multiple GPUs.

> **Status: experimental.** The backend has been validated on **gfx1151 (Strix Halo)** and **gfx1150 (Strix Point)**. Prebuilt wheels also exist for `gfx110X` (RDNA3) and `gfx120X` (RDNA4) but those targets have not been exercised end-to-end yet. **gfx942 (AMD Instinct MI300X, CDNA3)** installs from its own official release line and has been validated on MI300X hardware; **gfx950 (MI355X, CDNA4)** installs from the same line but has not been exercised on hardware yet (see [Deploying on MI300X](#deploying-on-mi300x-gfx942)).

## Available Backend

### ROCm
- **Platform**: Linux only
- **Hardware**: validated on gfx1151 (Strix Halo) and gfx1150 (Strix Point); prebuilt wheels also exist for gfx110X (RDNA3) and gfx120X (RDNA4); gfx942 (MI300X, CDNA3) validated on hardware; gfx950 (MI355X, CDNA4) installable, not yet hardware-validated
- **Bundle**: a self-contained tarball from [lemonade-sdk/vllm-rocm](https://github.com/lemonade-sdk/vllm-rocm) with a relocatable Python interpreter, PyTorch (ROCm), the ROCm user-space libs, Triton, and vLLM. No system Python / PyTorch / ROCm install is required on the host.

## Prerequisites

vLLM on AMD ROCm requires a kernel that exports the CWSR sysfs properties and an `amdgpu` setup that doesn't shadow the built-in driver. Both are covered with verification commands and fixes on the [Kernel Update Required](https://lemonade-server.ai/gfx1151_linux.html) page — that's the canonical reference; the same prerequisites apply to `llamacpp:rocm` and `sd-cpp:rocm-*`. Lemonade blocks install of `vllm:rocm` on systems missing the kernel fix and points users at that page.

## Install

```bash
lemonade backends install vllm:rocm
```

Or via HTTP:
```bash
curl -X POST http://localhost:13305/api/v1/install \
  -H 'Content-Type: application/json' \
  -d '{"recipe": "vllm", "backend": "rocm"}'
```

The install fetches a per-GPU-target release (e.g. `…-gfx1151`, `…-gfx1150`) from [lemonade-sdk/vllm-rocm](https://github.com/lemonade-sdk/vllm-rocm/releases). The base version is pinned in [`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json); the `-{gfx_target}` suffix is appended at runtime from `SystemInfo::get_rocm_arch()`, so the default pin covers all RDNA/APU architectures on one line.

#### Per-architecture release overrides

Some GPU targets ride a different vLLM/ROCm wheel cadence than the default pin and cannot share a single release tag — CDNA-dcgpu (gfx942 / MI300X), for example, uses its own vLLM/ROCm release line, separate from the RDNA line. For those, `backend_versions.json` carries an optional `vllm.rocm_arch_overrides` map keyed by asset family; the override base is resolved for the detected arch (falling back to the default pin otherwise) before the `-{gfx_target}` suffix is appended. An explicit `vllm.rocm_bin` pin (`latest` or a specific tag) still takes precedence over the builtin per-arch override — the override only replaces the *default* base. A pin that already carries a `-{gfx_target}` suffix must match the detected architecture: a cross-arch pin (for example a repo-wide `latest` that resolved to a suffixed RDNA tag, or an explicit tag for a different target) is **rejected** rather than installed against the wrong architecture line. Note that pinning `vllm.rocm_bin` to the exact default base tag is treated the same as leaving it unset (`builtin`) — the per-arch override still applies; set an explicit *non-default* tag to opt out of the override.

### Deploying on MI300X (gfx942)

The minimum path to a working gfx942 deployment with the FP8 + MTP recipes:

1. **Runtime asset.** `lemonade backends install vllm:rocm` installs the official gfx942 release
   (`vllm0.23.1.dev0+rocm7.15.0a20260721.g0fc695fc6.d20260723-rocm7.15.0-gfx942` from `lemonade-sdk/vllm-rocm`). CDNA uses the nightly line
   because the stable `vllm0.19.1-rocm7.13.0` CDNA asset does not start.
2. **Recipes.** The `Qwen3.6-27B-FP8-vLLM-{low,high}conc` and `Qwen3.6-35B-A3B-FP8-vLLM-{low,high}conc`
   recipes are built in; `lemonade run Qwen3.6-27B-FP8-vLLM-lowconc` pulls and serves. To register a
   pre-quantized checkpoint yourself: `lemonade pull user.MyModel --checkpoint Qwen/Qwen3.6-27B-FP8 --recipe vllm`.
3. **Verify.** The vLLM log shows the MTP head detected and the draft-token acceptance rate (~80% on
   the 27B dense recipe, gfx942). Discrete-HBM launch defaults apply automatically; force eager for a
   newly-added or misbehaving model with `lemonade config set vllm.args="--enforce-eager"`.

AITER (fused-MoE FP8 kernels) is a separate build-repo bake — the
recipes serve correctly without it.

## Use

Models registered with the `vllm` recipe in [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json) load automatically on first request. Built-in `vllm` entries serve the upstream Hugging Face weights as-is in **FP16** — there is no quantization step in the load path — so their model IDs carry an explicit `-FP16-` segment (e.g. `Qwen3.5-4B-FP16-vLLM`). This mirrors the `-Hybrid` / `-CPU` suffix convention used by `ryzenai-llm` and makes the data type obvious next to `llamacpp` (GGUF, typically Q4_K_M) and `flm` (4-bit) entries in the same list. Pointing a `user.*` `vllm` registration at a pre-quantized checkpoint (FP8, AWQ, GPTQ, etc.) is still supported.

To register your own:

```bash
lemonade pull user.MyModel \
  --checkpoint main Qwen/Qwen3-4B \
  --recipe vllm
```

Standard OpenAI-compatible endpoints (`/v1/chat/completions`, `/v1/completions`) work as usual. Lemonade forwards requests to the vLLM child process, which exposes the engine's own private endpoints (e.g. `/metrics`, `/version`) on a backend-only port surfaced via `GET /v1/health` (`backend_url` field) — useful for observability but not proxied through Lemonade.

## Model-Family Argument Config

Some vLLM model families need extra `vllm-server` arguments for correct behavior. For example, tool-calling models may need `--enable-auto-tool-choice` plus a matching `--tool-call-parser`. Lemonade keeps these built-in family defaults in [`vllm_model_config.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/vllm_model_config.json), separate from [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json).

When adding a built-in model with `recipe: "vllm"`, check whether its model family needs vLLM arguments. If it does, update `vllm_model_config.json` in the same PR as the `server_models.json` entry.

The config has two layers:

```json
{
  "schema_version": 1,
  "enable_checkpoint_regex_match": true,
  "families": {
    "qwen3.": {
      "match": [
        {
          "checkpoint_regex": "Qwen3\\."
        }
      ],
      "args": "--enable-auto-tool-choice --tool-call-parser qwen3_coder"
    }
  },
  "models": {
    "Qwen3.5-4B-vLLM": {
      "family": "qwen3."
    }
  }
}
```

- `families` defines reusable defaults for a model family. Each family can include `args` and optional `match` entries.
- `models` maps Lemonade model names to a family and can also add per-model `args`.
- `checkpoint_regex` is matched against the model checkpoint, not only the organization name. This lets one family match checkpoints from different Hugging Face organizations.
- `enable_checkpoint_regex_match` controls automatic family matching for unlisted models. Set it to `false` to require explicit entries under `models`.
- A model entry can set `disable_family_match: true` to prevent regex family matching for that model while still allowing model-specific `args`.

Argument precedence is:

1. Family `args`
2. Exact model `args`
3. User-provided `vllm_args`

Later layers override conflicting earlier flags but keep non-conflicting flags. Binary `--flag` / `--no-flag` pairs are resolved like the rest of Lemonade's `*_args` merge behavior, and repeated generic flags are preserved when the same flag appears multiple times.

Lemonade-managed process arguments cannot be set in this file or in `vllm_args`: `--model`, `--served-model-name`, `--host`, `--port`, `--max-model-len`, and `--enable-prefix-caching`.

`--enforce-eager` is a special case: it is normally managed by Lemonade (discrete-HBM GPUs such as MI300X default to CUDA-graph capture, while every other GPU — shared-memory APUs and consumer discrete dGPUs alike — defaults to eager), but you **may** pass `--enforce-eager` in `vllm_args` as a managed *intent* to force eager mode on a discrete-HBM GPU — either when bringing up a newly-added / not-yet-fully-supported model, or when an existing model's CUDA-graph capture misbehaves. Lemonade detects it and re-emits it exactly once (it is not passed through as a raw duplicate flag).

## Speculative decoding (MTP)

Some FP8 Qwen3.6 recipes enable **Multi-Token Prediction (MTP)** speculative decoding through a structured `speculative_config` object in [`vllm_model_config.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/vllm_model_config.json) (`{"method": "mtp", "num_speculative_tokens": 1}`). It is configured as an object rather than a `vllm_args` string because the space-delimited `vllm_args` tokenizer would corrupt inline JSON; Lemonade serializes it to a single `--speculative-config` argument.

**Validation scope.** MTP was verified on gfx942 / MI300X (`vllm0.19.1-rocm7.13.0`): vLLM detects the model's MTP head, and the dense `Qwen3.6-27B-FP8` recipe reached ~80% draft-token acceptance. The `Qwen3.6-35B-A3B-FP8` MoE recipes were throughput-benched on the same GPU with the MTP head active. The recipes are **not** arch-restricted, so an RDNA host with enough VRAM can load them against the default RDNA pin (`vllm0.20.1-rocm7.12.0`); that pin is newer than the gfx942 line and exposes the same method, but MTP on RDNA has not been independently serve-validated.

## Tuning

Free-form CLI args can be appended to `vllm-server` via `vllm.args`:

```bash
# Allow more concurrent sequences and turn on prefix caching
lemonade config set vllm.args="--max-num-seqs 128 --enable-prefix-caching"
```

The flat form (`vllm_args=...`) is also accepted and maps to the same setting.

## Known gotchas

- **Docker runtime needs `build-essential`.** vLLM/Triton JIT-compiles native launcher
  modules on first load. The published image includes it in the runtime stage; keep it
  if you build your own image.
- **Cold first-load JIT.** Loading a new model size triggers a Triton kernel compile. Expect 20 s – several minutes the first time you hit a given model+shape; subsequent loads of the same shape are faster as kernels cache to disk.
- **FP8 first-load is slow on gfx1151.** Cold-loading `Qwen/Qwen3-4B-FP8` took ~12 minutes in our test, exceeding Lemonade's default `wait_for_ready` timeout. The engine selects `TritonFp8BlockScaledMMKernel` and emits *"Using default W8A8 Block FP8 kernel config. Performance might be sub-optimal."* warnings — i.e. no AMD-tuned kernel configs are shipped for this GPU's exact shapes, so vLLM autotunes from defaults. FP16 is the most polished path today; FP8 should improve once AMD ships tuned configs.
- **`huggingface-hub` shadowing.** Lemonade launches `vllm-server` with `PYTHONNOUSERSITE=1` so the bundled `huggingface_hub` is used. If a module-not-found error still appears, ensure `~/.local/lib/python3.12/site-packages/huggingface_hub` isn't being injected via `PYTHONPATH`.
- **Long load times can leave orphaned processes if interrupted.** If a load times out at the Lemonade level, vLLM's child `EngineCore` may continue running in the background and hold VRAM until killed. Look for a `VLLM::EngineCor` process and `kill -9` it before retrying.
