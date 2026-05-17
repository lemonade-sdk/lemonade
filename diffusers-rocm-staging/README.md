# diffusers-rocm

Portable builds of **Hugging Face Diffusers** with **AMD ROCm** acceleration, packaged as self-contained relocatable archives. Each release bundles a relocatable CPython interpreter, PyTorch-ROCm, the ROCm user-space libraries, and a small FastAPI server exposing OpenAI-compatible image-generation endpoints. No system Python, PyTorch, or ROCm install required.

Designed as a backend for [**Lemonade**](https://github.com/lemonade-sdk/lemonade), and built with the same pipeline as our sister project [`vllm-rocm`](https://github.com/lemonade-sdk/vllm-rocm).

> [!IMPORTANT]
> **Early development.** ROCm support for consumer AMD GPUs (RDNA) in Diffusers / PyTorch is still maturing. Issue reports welcome.

## Supported devices

| GPU Target  | Architecture  | Devices                                                                |
|-------------|---------------|------------------------------------------------------------------------|
| **gfx1151** | Strix Halo APU| Ryzen AI MAX+ Pro 395                                                  |
| **gfx1150** | Strix Point APU | Ryzen AI 300                                                         |
| **gfx120X** | RDNA4 GPUs    | RX 9070 XT, RX 9070, RX 9060 XT, RX 9060                               |
| **gfx110X** | RDNA3 GPUs    | RX 7900 XTX/XT/GRE, RX 7800 XT, RX 7700 XT, RX 7600 XT/7600            |

Builds include ROCm user-space wheels — no separate ROCm installation required. You still need a Linux kernel with a working `amdgpu` driver; for gfx1151 specifically that means kernel 6.18.4+ (see [Lemonade's gfx1151 notes](https://lemonade-server.ai/gfx1151_linux.html)).

## Quick start

1. **Download** the build for your GPU from the [latest release](https://github.com/lemonade-sdk/diffusers-rocm/releases/latest). Large builds may be split into `.partNN-of-MM.tar.gz` parts.
2. **Extract** the archive:
   ```bash
   mkdir -p ~/diffusers-rocm
   # single-archive case:
   tar xzf diffusers0.32.1-rocm7.12.0-gfx1151-x64.tar.gz -C ~/diffusers-rocm
   # split case (concatenate parts, then untar):
   cat diffusers*.part*-of-*.tar.gz | tar xz -C ~/diffusers-rocm
   ```
3. **Run** the server with any Diffusers-compatible model:
   ```bash
   ~/diffusers-rocm/bin/diffusers-server \
     --model Efficient-Large-Model/Sana_1600M_1024px_diffusers \
     --port 8000 \
     --pipeline-class SanaPipeline
   ```
4. **Test** with curl (OpenAI-compatible Images API):
   ```bash
   curl http://localhost:8000/v1/images/generations \
     -H "Content-Type: application/json" \
     -d '{
       "model": "Efficient-Large-Model/Sana_1600M_1024px_diffusers",
       "prompt": "a cinematic photo of a cat astronaut on Mars",
       "size": "1024x1024",
       "num_inference_steps": 20
     }' | jq -r '.data[0].b64_json' | base64 -d > out.png
   ```

> **Lemonade integration**: these builds are designed to drop into [Lemonade](https://github.com/lemonade-sdk/lemonade) as the `diffusers` recipe. Lemonade handles downloading, launching, and routing requests; you don't need to invoke `diffusers-server` directly.

## What's included

Each release extracts to a relocatable CPython 3.12 distribution with all deps pre-installed:

```
bin/
  diffusers-server            # Launcher shim — sets LD_LIBRARY_PATH, execs diffusers_server
  python3.12                  # Bundled CPython (python-build-standalone)
lib/
  libpython3.12.so
  python3.12/site-packages/
    diffusers/                # HF Diffusers
    transformers/             # HF Transformers (text encoders, tokenizers)
    accelerate/, safetensors/, sentencepiece/, ...
    torch/                    # PyTorch ROCm (libs under torch/lib/)
    _rocm_sdk_core/lib/       # ROCm core (hip, hsa, comgr, clang, llvm) — when wheel present
    _rocm_sdk_libraries_gfx<arch>/lib/
                              # Per-arch ROCm math libs (rocblas, hipblas, MIOpen, ...)
    diffusers_server/         # FastAPI server module (provided by this repo)
    fastapi/, uvicorn/, ...
```

The `bin/diffusers-server` shim puts ROCm/torch lib paths on `LD_LIBRARY_PATH`, then execs:

```
python3 -m diffusers_server --model ... --port ...
```

## OpenAI-compatible endpoints

| Endpoint                       | Status     | Notes                                                  |
|--------------------------------|------------|--------------------------------------------------------|
| `POST /v1/images/generations`  | ✅         | Maps OpenAI request → diffusers pipeline `__call__`     |
| `POST /v1/images/edits`        | planned    | For img2img / inpainting pipelines                      |
| `POST /v1/images/variations`   | planned    | For variation pipelines                                 |
| `GET  /health`                 | ✅         | Used by Lemonade's startup probe                        |

The request schema accepts `n`, `size`, plus diffusers-specific passthrough fields (`num_inference_steps`, `guidance_scale`, `negative_prompt`, `seed`).

## CLI

```
diffusers-server --model HF_ID
                 [--port N] [--host H]
                 [--served-model-name NAME]
                 [--dtype bf16|fp16|fp32]
                 [--pipeline-class CLASS]    # e.g. SanaPipeline
                 [--variant VARIANT]
                 [--log-level info|debug|warning|error]
```

If `--pipeline-class` is omitted, `DiffusionPipeline.from_pretrained` is used and auto-dispatches based on the model's `model_index.json`. For SANA models, pass `--pipeline-class SanaPipeline` (or one of `SanaSprintPipeline`, `SanaPAGPipeline`).

## Automated builds

The GitHub Actions workflow (`.github/workflows/build-diffusers-rocm.yml`):

1. Downloads relocatable **CPython 3.12** from [`astral-sh/python-build-standalone`](https://github.com/astral-sh/python-build-standalone)
2. Installs **PyTorch ROCm** from AMD's pip index (`https://repo.amd.com/rocm/whl/<target>/`)
3. Best-effort installs **`rocm-sdk-core` + `rocm-sdk-libraries-gfx<target>`** so the Triton HIP backend has its libraries
4. Installs **Diffusers + Transformers + FastAPI** from PyPI
5. Copies `src/diffusers_server/` into bundled `site-packages`
6. Pre-compiles Triton's HIP utils to eliminate the runtime gcc dependency
7. Generates `bin/diffusers-server` shim
8. Strips ~1 GB of unneeded files (pip wheels, opencv, pandas, LLVM tools beyond clang)
9. Tars the result, splits if >1.9 GB, and creates a GitHub release per GPU target

Release tag format: `diffusers{version}-{rocm_version}-{gfx_target}` (e.g. `diffusers0.32.1-rocm7.12.0-gfx1151`).

## Dependencies

### Runtime (bundled)
- **[Diffusers](https://github.com/huggingface/diffusers)** — HF pipelines for image/video diffusion
- **[Transformers](https://github.com/huggingface/transformers)** — text encoders, tokenizers
- **[PyTorch](https://pytorch.org/)** — tensor compute (ROCm wheel from `repo.amd.com/rocm/whl/<target>/`)
- **[ROCm SDK wheels](https://github.com/ROCm/TheRock)** — when available for the target arch
- **[FastAPI](https://github.com/tiangolo/fastapi)** + **[Uvicorn](https://github.com/encode/uvicorn)** — HTTP server
- **[python-build-standalone](https://github.com/astral-sh/python-build-standalone)** — relocatable CPython 3.12

### Build (CI only)
- Ubuntu 22.04 GitHub Actions runner
- `pip`, `gcc` (for the Triton HIP utils prebuild)

## License

MIT — see [LICENSE](LICENSE).
