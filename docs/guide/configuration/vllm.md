# vLLM Backend Options

Lemonade integrates [vLLM](https://github.com/vllm-project/vllm) as an experimental backend for AMD ROCm GPUs on Linux. vLLM brings two core benefits:

1. **Day-0 model support.** vLLM typically supports new transformer architectures within hours of their release on Hugging Face — checkpoints load directly, with no per-architecture porting.
2. **Concurrency and multi-GPU.** Paged-attention KV cache, continuous batching, and chunked prefill scale aggregate throughput with in-flight request count; tensor and pipeline parallelism are supported across multiple GPUs.

> **Status: experimental.** The backend has been validated on **gfx1151 (Strix Halo)** and **gfx1150 (Strix Point)**. Prebuilt wheels also exist for `gfx110X` (RDNA3) and `gfx120X` (RDNA4) but those targets have not been exercised end-to-end yet.

## Available Backend

### ROCm
- **Platform**: Linux only
- **Hardware**: validated on gfx1151 (Strix Halo) and gfx1150 (Strix Point); prebuilt wheels also exist for gfx110X (RDNA3) and gfx120X (RDNA4)
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

The install fetches a per-GPU-target release (e.g. `…-gfx1151`, `…-gfx1150`) from [lemonade-sdk/vllm-rocm](https://github.com/lemonade-sdk/vllm-rocm/releases). The base version is pinned in [`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json); the `-{gfx_target}` suffix is appended at runtime from `SystemInfo::get_rocm_arch()`, so a single pin covers all supported architectures.

## Use

Models registered with the `vllm` recipe in [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json) load automatically on first request. To register your own:

```bash
lemonade pull user.MyModel \
  --checkpoint main Qwen/Qwen3-4B \
  --recipe vllm
```

Standard OpenAI-compatible endpoints (`/v1/chat/completions`, `/v1/completions`) work as usual. Lemonade forwards requests to the vLLM child process, which exposes the engine's own private endpoints (e.g. `/metrics`, `/version`) on a backend-only port surfaced via `GET /v1/health` (`backend_url` field) — useful for observability but not proxied through Lemonade.

## Tuning

Lemonade builds the `vllm-server` command line in layers:

1. Lemonade-managed process and universal behavior args in code: model id, private port, host, served model name, eager execution, max model length, and prefix caching.
2. Built-in model-family args from `resources/vllm_model_config.json`.
3. User `vllm_args`.

User `vllm_args` can replace config-file args by flag or conflict group, such as `--tool-call-parser`, auto-tool-choice enable/disable flags, `--quantization`, and memory-budget flags. They cannot override Lemonade-managed args such as `--model`, `--port`, `--host`, `--served-model-name`, `--enforce-eager`, `--max-model-len`, or `--enable-prefix-caching`.

The shared-memory default `--kv-cache-memory-bytes 4G` is added unless user `vllm_args` contains `--kv-cache-memory-bytes` or `--gpu-memory-utilization`.

Free-form CLI args can be provided via `vllm_args`:

```bash
# Allow more concurrent sequences and choose a memory budget
lemonade config set vllm_args="--max-num-seqs 128 --gpu-memory-utilization 0.9"
```

`vllm_model_config.json` supports model-family entries matched by checkpoint regex and exact model entries. Exact model args apply after family args. Set `"disable_family_match": true` on a model entry to opt out of checkpoint-regex family matching and use only that model's explicit args.

## Known gotchas

- **Cold first-load JIT.** Loading a new model size triggers a Triton kernel compile. Expect 20 s – several minutes the first time you hit a given model+shape; subsequent loads of the same shape are faster as kernels cache to disk.
- **FP8 first-load is slow on gfx1151.** Cold-loading `Qwen/Qwen3-4B-FP8` took ~12 minutes in our test, exceeding Lemonade's default `wait_for_ready` timeout. The engine selects `TritonFp8BlockScaledMMKernel` and emits *"Using default W8A8 Block FP8 kernel config. Performance might be sub-optimal."* warnings — i.e. no AMD-tuned kernel configs are shipped for this GPU's exact shapes, so vLLM autotunes from defaults. FP16 is the most polished path today; FP8 should improve once AMD ships tuned configs.
- **`huggingface-hub` shadowing.** Lemonade launches `vllm-server` with `PYTHONNOUSERSITE=1` so the bundled `huggingface_hub` is used. If a module-not-found error still appears, ensure `~/.local/lib/python3.12/site-packages/huggingface_hub` isn't being injected via `PYTHONPATH`.
- **Long load times can leave orphaned processes if interrupted.** If a load times out at the Lemonade level, vLLM's child `EngineCore` may continue running in the background and hold VRAM until killed. Look for a `VLLM::EngineCor` process and `kill -9` it before retrying.
