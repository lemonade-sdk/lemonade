# vLLM Backend Options

Lemonade integrates [vLLM](https://github.com/vllm-project/vllm) as an experimental backend for AMD ROCm GPUs on Linux. vLLM brings three things to Lemonade that the existing backends can't:

1. **Day-0 model support.** vLLM is typically the first inference engine to support new transformer architectures, often within hours of a model's release on Hugging Face. Because vLLM loads models directly from Hugging Face checkpoints (no per-architecture porting, no quantized GGUF conversion), most newly released models work without any code changes in Lemonade.
2. **True omni-modal support.** vLLM has first-class support for vision-language models, audio inputs, embeddings, and reranking in addition to text generation — all from a single engine, sharing the same model registry and serving stack.
3. **Improved concurrency and multi-GPU support.** vLLM's paged-attention KV cache, continuous batching, and chunked prefill deliver significantly higher aggregate throughput under concurrent load than llama.cpp. vLLM also natively supports tensor and pipeline parallelism across multiple GPUs.

> **Status: experimental.** All vLLM models in the registry carry the `experimental` label and are surfaced only when the experimental opt-in is enabled. The backend has been validated on **gfx1151 (Strix Halo)** and **gfx1150 (Strix Point)**. Prebuilt wheels also exist for `gfx110X` (RDNA3) and `gfx120X` (RDNA4) but those targets have not been exercised end-to-end yet.

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

Free-form CLI args can be appended to `vllm-server` via `vllm_args`:

```bash
# Allow more concurrent sequences and turn on prefix caching
lemonade config set vllm_args="--max-num-seqs 128 --enable-prefix-caching"
```

## Known gotchas

- **Cold first-load JIT.** Loading a new model size triggers a Triton kernel compile. Expect 20 s – several minutes the first time you hit a given model+shape; subsequent loads of the same shape are faster as kernels cache to disk.
- **FP8 first-load is slow on gfx1151.** Cold-loading `Qwen/Qwen3-4B-FP8` took ~12 minutes in our test, exceeding Lemonade's default `wait_for_ready` timeout. The engine selects `TritonFp8BlockScaledMMKernel` and emits *"Using default W8A8 Block FP8 kernel config. Performance might be sub-optimal."* warnings — i.e. no AMD-tuned kernel configs are shipped for this GPU's exact shapes, so vLLM autotunes from defaults. FP16 is the most polished path today; FP8 should improve once AMD ships tuned configs.
- **`huggingface-hub` shadowing.** Lemonade launches `vllm-server` with `PYTHONNOUSERSITE=1` so the bundled `huggingface_hub` is used. If a module-not-found error still appears, ensure `~/.local/lib/python3.12/site-packages/huggingface_hub` isn't being injected via `PYTHONPATH`.
- **Long load times can leave orphaned processes if interrupted.** If a load times out at the Lemonade level, vLLM's child `EngineCore` may continue running in the background and hold VRAM until killed. Look for a `VLLM::EngineCor` process and `kill -9` it before retrying.
