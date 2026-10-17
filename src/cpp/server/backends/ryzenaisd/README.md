# ryzenaisd backend

Lemonade backend that wraps `ryzenai-sd-server` for NPU-accelerated Stable
Diffusion inference on AMD Ryzen AI hardware.

## How it works

`RyzenAISDServer` is a `WrappedServer` subclass that follows the standard
Lemonade subprocess model: instead of running inference in-process, it spawns
`ryzenai-sd-server.exe` as a child process and proxies image generation
requests to it over HTTP on a loopback port chosen at startup.

```
Client
  → lemond (RyzenAISDServer)
    → ryzenai-sd-server.exe  --server --model-path <dir> --listen-port <N>
      → ONNX Runtime / RyzenAI EP
        → AMD NPU
```

The subprocess exposes an OpenAI-compatible Images API (`/v1/images/generations`,
`/v1/images/edits`, `/v1/images/variations`). `RyzenAISDServer` forwards
requests to those endpoints and returns the response unchanged. `steps` /
`cfg_scale` / `seed` / `strength` are embedded in the prompt on all three
endpoints as `<sd_cpp_extra_args>{...}</sd_cpp_extra_args>` (the only channel
the subprocess reads them from) -- one decode path on the subprocess side
rather than per-endpoint field conventions.

> `ryzenai-sd-server` must decode `<sd_cpp_extra_args>` on `/v1/images/variations`
> for this to take effect there; older builds that only read plain
> `num_inference_steps`/`guidance_scale`/`seed`/`strength` fields on that
> endpoint will silently fall back to their own defaults instead of honoring
> per-request overrides.

Because the subprocess is expensive to restart (ONNX Runtime init, DLL
loading, custom-op registration), the process is **kept alive** across model
switches. When a different model is requested, `load()` sends
`POST /v1/internal/load` to hot-swap the model directory in place, avoiding a
full restart.

The backend carries `SlotPolicy::ExclusiveNpu`, so the Router evicts every NPU
model of another recipe before loading a `ryzenai-sd` model. A `ryzenai-sd`
server that is already running is reused as a hot-swap target rather than
evicted.

## How models are loaded

### 1. Binary resolution

`load()` calls `backend_manager_->install_backend("ryzenai-sd", "npu")`, then
resolves the binary via
`BackendUtils::get_backend_binary_path(*ryzenaisd::spec(), "npu")`. The lookup
priority is:

1. Environment variable `LEMONADE_RYZENAI_SD_NPU_BIN`
2. `ryzenaisd.npu_bin` in `config.json` (absolute path, for pointing at a
   locally built binary)
3. The auto-installed copy, downloaded from the
   `lemonade-sdk/ryzenai-sd-server` GitHub release pinned by
   `backend_versions.json` → `ryzenai-sd.npu`

The release zip is self-contained: the executable, the ONNX Runtime and
VitisAI EP DLLs, and the GenAI-SD `lib/` tree the server points `DD_ROOT` /
`DD_PLUGINS_ROOT` at. No RyzenAI SDK installation is required on the host.

This is a different artifact from `ryzenai-llm`'s `ryzenai-server.zip`, which
carries neither the SD executable nor the GenAI-SD transaction tree. The two
version pins are independent and may be bumped separately.

### 2. Model path resolution

Models are ordinary HuggingFace repos of pre-converted ONNX files, e.g.
`amd/sd-turbo-amdnpu-onnx`. The server needs the snapshot root directory (it
contains `text_encoder/`, `unet/`, `vae_decoder/` …), not a single file, so
`RyzenAISDOps::resolve_checkpoint_path()` returns the active snapshot
directory rather than the model cache parent. `load()` then validates that the
path exists and is a directory before spawning the subprocess.

### 3. Subprocess startup

```
ryzenai-sd-server.exe
  --server
  --model-path  <resolved directory>
  --listen-port <lemond-chosen ephemeral port>
  [--verbose]                    # when lemond is in debug mode
  [<ryzenaisd_args from config>] # user-supplied extra args
```

The binary's directory is prepended to `PATH` so the ONNX Runtime and
RyzenAI DLLs bundled alongside it are found by the Windows loader.

`lemond` polls `GET /health` on the subprocess until it returns 200 (up to
120 s), then marks the backend ready.

### 4. Hot-swap (subsequent loads)

If the subprocess is already running when `load()` is called again (model
switch), `lemond` skips the restart and calls:

```
POST /v1/internal/load
{ "model_path": "<new directory>" }
```

A 200 response means the swap request was handled, not that the process
survived it, so `load()` re-runs the `/health` readiness check afterwards with
a short timeout and falls back to a cold start if the subprocess has died.

**Exception — resolution changes force a restart.** The RyzenAI NPU EP sizes a
process-wide instruction buffer from whichever model loads first. Swapping to a
model of a different resolution needs a differently sized buffer and crashes
the process, so `load()` restarts rather than hot-swaps whenever the resolution
changes. Same-resolution swaps (SD-Turbo → SD-1.5 at 512×512, SDXL-Base →
Segmind-Vega at 1024×1024) are reliable.

## Configuration

`config.json` section `ryzenaisd`:

| Key | Default | Description |
|-----|---------|-------------|
| `npu_bin` | `"builtin"` | `"builtin"` uses the auto-installed release; set an absolute path to `ryzenai-sd-server.exe` to override with a local build. |
| `args` | `""` | Extra CLI flags forwarded to the subprocess (managed flags are blocked). |
| `steps` | `20` | Default diffusion steps (overridden per model by `image_defaults`). |
| `cfg_scale` | `7.0` | Default classifier-free guidance scale (overridden per model by `image_defaults`). |
| `strength` | `0.75` | Default denoising strength for `image_edits`/`image_variations` (img2img/inpainting), embedded in the `<sd_cpp_extra_args>` prompt tag. Request-level `strength` overrides this. Not overridden by `image_defaults` (no such per-model field exists yet). |
| `width` / `height` | `512` | Default image dimensions. |

## Relevant files

| File | Purpose |
|------|---------|
| [ryzenaisd.h](../../include/lemon/backends/ryzenaisd/ryzenaisd.h) | Descriptor — recipe id, binary name, config section, option schema |
| [ryzenaisd_server.h](../../include/lemon/backends/ryzenaisd/ryzenaisd_server.h) | `RyzenAISDServer` class declaration |
| [ryzenaisd_server.cpp](ryzenaisd_server.cpp) | Implementation: `load`, `unload`, `image_generations`, `image_edits`, `image_variations` |
| [server_models.json](../../../resources/server_models.json) | Model registry entries (`recipe: "ryzenai-sd"`) |
| [backend_versions.json](../../../resources/backend_versions.json) | `ryzenai-sd.npu` release pin and its asset checksum |
| [test/server_ryzenai_sd.py](../../../../../test/server_ryzenai_sd.py) | End-to-end tests against a live `lemond` |
