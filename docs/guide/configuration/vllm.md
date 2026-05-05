# vLLM Backend Options

Lemonade integrates [vLLM](https://github.com/vllm-project/vllm) as an experimental backend for AMD ROCm GPUs on Linux. vLLM's paged-attention KV-cache and continuous batching deliver much higher aggregate throughput than llama.cpp under concurrent load, at the cost of a heavier runtime footprint.

> **Status: experimental.** All vLLM models in the registry carry the `experimental` label and are surfaced only when the experimental opt-in is enabled. The backend is currently validated only on **gfx1151 (Strix Halo / Radeon 8060S)**. Other AMD targets (`gfx1150`, `gfx110X`, `gfx120X`) have prebuilt wheels but have not been exercised end-to-end.

## Available Backend

### ROCm
- **Platform**: Linux only
- **Hardware**: AMD Ryzen AI MAX+ (Strix Halo, gfx1151) — validated. Prebuilt wheels also exist for `gfx1150` (Strix Point), `gfx110X` (RDNA3), and `gfx120X` (RDNA4) but are untested.
- **Wheel**: `vllm-0.20.1+rocm721` from [wheels.vllm.ai/rocm/](https://wheels.vllm.ai/rocm/), packaged as a self-contained tarball by [lemonade-sdk/vllm-rocm](https://github.com/lemonade-sdk/vllm-rocm).
- **Bundle contents**: relocatable CPython 3.12, PyTorch 2.10.0+rocm7.12.0, ROCm 7.12.0 user-space libs, Triton, vLLM. No system Python / PyTorch / ROCm install required on the host.

## Prerequisites

### Kernel
The kernel must export the CWSR (Context Wave Save/Restore) sysfs properties. Without them, ROCm dispatches on gfx1151 trigger `GCVM_L2_PROTECTION_FAULT` and the backend hangs. Mainline 6.18.4+ has the fix; some vendor kernels backport it. Lemonade blocks install of `vllm:rocm` on systems missing the fix and points users at [Kernel Update Required](https://lemonade-server.ai/gfx1151_linux.html).

Quick check:
```bash
uname -r
grep -E "cwsr_size|ctl_stack_size" /sys/class/kfd/kfd/topology/nodes/*/properties
```

### `amdgpu-dkms` collision
The default Radeon repo (`amdgpu/30.30`) ships `amdgpu-dkms 6.16.13`, which overrides the kernel's built-in driver with a broken version. Either switch to `amdgpu/31.20` or uninstall `amdgpu-dkms` entirely — vLLM bundles its own ROCm user-space, so the DKMS package is not needed for inference. See [Kernel Update Required](https://lemonade-server.ai/gfx1151_linux.html) for the exact commands.

## Install

```bash
# Install the backend (downloads ~2.5 GB split into two release assets).
lemonade backends install vllm:rocm
```

Or via HTTP:
```bash
curl -X POST http://localhost:13305/api/v1/install \
  -H 'Content-Type: application/json' \
  -d '{"recipe": "vllm", "backend": "rocm"}'
```

The install fetches `vllm{version}-rocm{version}-{gfx_target}` (e.g. `vllm0.20.1-rocm7.12.0-gfx1151`) from [lemonade-sdk/vllm-rocm](https://github.com/lemonade-sdk/vllm-rocm/releases). The base version is pinned in [`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json); the `-{gfx_target}` suffix is appended at runtime from `SystemInfo::get_rocm_arch()` so a single pin covers all supported architectures.

## Use

Models registered with the `vllm` recipe in [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json) load automatically on first request. To register your own:

```bash
lemonade pull user.MyModel \
  --checkpoint main Qwen/Qwen3-4B \
  --recipe vllm
```

Standard OpenAI-compatible endpoints (`/v1/chat/completions`, `/v1/completions`) work as usual. Lemonade forwards requests to vLLM's child process, which exposes the engine's own private endpoints (e.g. `/metrics`, `/version`) on a backend-only port surfaced via `GET /v1/health` (`backend_url` field) — useful for observability but not proxied through Lemonade.

## Tuning

The vLLM child process is launched with sensible defaults for single-GPU APUs. To override, set `vllm_args` (free-form CLI args appended to `vllm-server`) and / or `vllm_backend`:

```bash
# Allow more concurrent sequences (default is what the bundled launcher sets).
lemonade config set vllm_args="--max-num-seqs 128 --enable-prefix-caching"
```

## Known gotchas

- **Cold first-load JIT.** Loading a new model size compiles HIP/Triton kernels for your GPU, taking 20 s – several minutes. Subsequent loads of the same shape hit the on-disk Triton cache.
- **FP8 quantization on gfx1151.** vLLM 0.20.1 selects `TritonFp8BlockScaledMMKernel` for FP8 models on the 8060S, but no AMD-tuned kernel config exists for this GPU/shape — vLLM falls back to default configs and warns *"Performance might be sub-optimal."* Cold first-load can take 12+ minutes (longer than Lemonade's `wait_for_ready` timeout will tolerate). FP16 is the recommended precision today; revisit FP8 once AMD ships tuned configs.
- **`huggingface-hub` shadowing.** Lemonade launches `vllm-server` with `PYTHONNOUSERSITE=1` so the bundled `huggingface_hub` is used. If a module-not-found error still appears, ensure `~/.local/lib/python3.12/site-packages/huggingface_hub` isn't being injected via `PYTHONPATH`.
- **Long load times leave orphaned processes if interrupted.** If a load times out at the Lemonade level, vLLM's child `EngineCore` may continue running in the background and hold VRAM until killed. Look for a `VLLM::EngineCor` process and `kill -9` it before retrying.
