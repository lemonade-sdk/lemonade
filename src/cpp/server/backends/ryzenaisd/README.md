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
`cfg_scale` / `seed` are embedded in the prompt as
`<sd_cpp_extra_args>{...}</sd_cpp_extra_args>` for generations/edits (the only
channel the subprocess reads them from on those endpoints); `image_variations`
forwards them as plain multipart fields instead, since that endpoint supports
both and doesn't need the tag.

Because the subprocess is expensive to restart (ONNX Runtime init, DLL
loading, custom-op registration), the process is **kept alive** across model
switches. When a different model is requested, `load()` sends
`POST /v1/internal/load` to hot-swap the model directory in place, avoiding a
full restart.

The backend carries `SlotPolicy::ExclusiveNpu`, so the Router evicts all other
NPU models before loading a `ryzenai-sd` model.

## How models are loaded

### 1. Binary resolution

`lemond` resolves the `ryzenai-sd-server` binary at `load()` time via
`BackendUtils::get_backend_binary_path(*ryzenaisd::spec(), "npu")`.  The
lookup priority is:

1. Environment variable `LEMONADE_RYZENAI_SD_NPU_BIN`
2. `ryzenaisd.npu_bin` in `config.json` (absolute path, e.g.
   `C:/…/ryzenai-sd-server/build/bin/Release/ryzenai-sd-server.exe`)
3. No auto-install — the binary is externally built; the descriptor sets
   `install_params_fn = nullptr` and `backend_versions.json` marks the
   version as `"local"`.

### 2. Model path resolution

Each model entry in `user_models.json` (or `server_models.json`) must have:

```json
{
  "recipe": "ryzenai-sd",
  "source": "local_path",
  "checkpoint": "<absolute path to ONNX model directory>"
}
```

`ModelInfo::resolved_path("main")` returns that directory verbatim (no
translation). `load()` then validates that the path exists and is a directory
before spawning the subprocess.

**`source: "local_path"` models are never downloaded by Lemonade.** The path
is treated as-is; if it doesn't exist, `load()` throws immediately with
"Model path does not exist". Lemonade's HuggingFace download engine only
runs for `source: "huggingface"` entries. There is also no auto-install for
the binary (`install_params_fn = nullptr`).

If you want Lemonade to auto-download a model from HuggingFace, omit
`source` (or set it to `"huggingface"`) and set `checkpoint` to a HF repo
id (e.g. `"amd/stable-diffusion-turbo-amdnpu-onnx"`). The model will be
downloaded to the Lemonade HF cache on the first `lemonade pull` or load
request.

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

A 200 response means the model swapped successfully in place.

## Configuration

`config.json` section `ryzenaisd`:

| Key | Default | Description |
|-----|---------|-------------|
| `npu_bin` | `"builtin"` | Absolute path to `ryzenai-sd-server.exe`. Must be set; there is no built-in download. |
| `args` | `""` | Extra CLI flags forwarded to the subprocess (managed flags are blocked). |
| `steps` | `20` | Default diffusion steps (overridden per model by `image_defaults`). |
| `cfg_scale` | `7.0` | Default classifier-free guidance scale (overridden per model by `image_defaults`). |
| `strength` | `0.75` | Default denoising strength for `image_edits`/`image_variations` (img2img/inpainting), sent as a plain multipart field. Request-level `strength` overrides this. Not overridden by `image_defaults` (no such per-model field exists yet). Note: `Server::handle_image_edits`/`handle_image_variations` (`server.cpp`, shared code) must also parse a `strength` multipart field into the request JSON for this to be reachable from an external HTTP client — added alongside this option since neither handler previously extracted it (and `handle_image_variations` didn't extract `prompt`/`steps`/`cfg_scale`/`seed` at all). |
| `width` / `height` | `512` | Default image dimensions. |

## Relevant files

| File | Purpose |
|------|---------|
| [ryzenaisd.h](../../include/lemon/backends/ryzenaisd/ryzenaisd.h) | Descriptor — recipe id, binary name, config section, option schema |
| [ryzenaisd_server.h](../../include/lemon/backends/ryzenaisd/ryzenaisd_server.h) | `RyzenAISDServer` class declaration |
| [ryzenaisd_server.cpp](ryzenaisd_server.cpp) | Implementation: `load`, `unload`, `image_generations`, `image_edits`, `image_variations` |
| `lemonade/user_models.json` | Per-user model definitions (7 local ONNX model entries) |
| `lemonade/config.json` → `ryzenaisd` | Binary path and default generation parameters |
| `lemonade/test/validate_ryzenaisd.py` | End-to-end validation script (preflight direct test + through-lemond test) |
