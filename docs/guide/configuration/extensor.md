# EXTENSOR backend

The `extensor` recipe runs AMD EXTENSOR as a managed subprocess on Linux.
The executable is downloaded from AMD when first needed, using the exact GPU
architecture reported by ROCm:

| GPU target | EXTENSOR 2.3.2 archive |
|------------|------------------------|
| `gfx1151` | [Download](https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1151.zip) |
| `gfx1152` | [Download](https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1152.zip) |
| `gfx1201` | [Download](https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1201.zip) |

Other GPU targets and operating systems are rejected before downloading. The
archive's binaries and `share/extensor` assets are kept together under
`<cache_dir>/bin/extensor/rocm/`. Lemonade reuses an installed version that matches
the selected version. Updates are staged and verified before replacing the
working installation.

## Configuration

These built-in model names select the `extensor` recipe:

| Model name | Hugging Face image |
|------------|--------------------|
| `Qwen3.6-35B-A3B-EXTENSOR` | `amd/Qwen3.6-35B-A3B-EXTENSOR:Qwen3.6-35B-A3B-EXTENSOR-ROCmFP4-v1.extensor.gguf` |
| `DeepSeek-V4-Flash-EXTENSOR` | `amd/DeepSeek-V4-Flash-EXTENSOR:DeepSeek-V4-Flash-EXTENSOR-MXFP4-Tile16-v2.extensor.gguf` |
| `GLM-5.2-EXTENSOR` | `amd/GLM-5.2-EXTENSOR:GLM-5.2-EXTENSOR-Q2_K-Experts-v1.extensor.gguf` |

A load or inference request downloads the selected image if it is missing,
then starts EXTENSOR with its cached path. Completed images are reused;
interrupted downloads retain the shared downloader's resume and integrity checks.
Qwen uses the target-only v1 image because the MTP v2 image requires EXTENSOR 2.4.0.

Lemonade does not block EXTENSOR models based on model size or declared resident
memory. EXTENSOR streams weights and sizes its expert cache at runtime, so it
determines whether a model can load with the available memory. OS and GPU
compatibility checks still apply; other backends retain their memory filters.

Set these overrides in `config.json`, replacing the example path with your model directory:

```json
{
  "ctx_size": 4096,
  "models_dir": "/path/to/lemonade-models",
  "extensor": {
    "backend": "rocm",
    "rocm_bin": "builtin",
    "extensor_preset": "balanced"
  }
}
```

For example, `POST /v1/load` with
`{"model_name":"Qwen3.6-35B-A3B-EXTENSOR"}` downloads and loads Qwen.
Use the same model name in `/v1/chat/completions` to load it on the first request.
An optional `extensor.extensor_model_path` selects an existing local image instead.

`builtin` selects the version pinned by Lemonade (currently `v2.3.2`). An explicit
version such as `v2.3.2` selects that version's AMD archive. `latest` is unsupported
because these AMD downloads do not provide a latest-release discovery endpoint.
An absolute executable path selects a user-managed installation.

The presets are `exact`, `balanced`, `fast`, and `demo`. Lemonade forwards
`ctx_size` as EXTENSOR's `--context-size`.

## Download controls

EXTENSOR uses the same installer controls as llama.cpp:

- `no_fetch_executables: true` prevents executable downloads. An already installed
  binary or a local path remains usable; a missing binary produces an error.
- `offline: true` also blocks executable installation and updates. Neither flag
  is bypassed by a forced installation request.
- `LEMONADE_EXTENSOR_ROCM_BIN` overrides `extensor.rocm_bin`, following the same
  naming scheme as `LEMONADE_LLAMACPP_ROCM_BIN`. For example:

  ```bash
  export LEMONADE_EXTENSOR_ROCM_BIN=/opt/extensor/bin/extensor-server
  ```

- `LEMONADE_CACHE_DIR` selects the cache unless a cache directory was explicitly
  passed to `lemond`. Model weights use `models_dir`; when it is `auto`,
  `HF_HUB_CACHE` or `HF_HOME` controls the Hugging Face cache.
- `LEMONADE_DEFAULTS_PATH` can point to a JSON defaults file containing the shared
  `offline` or `no_fetch_executables` keys. User configuration overrides defaults.

ROCm 7 and EXTENSOR's system libraries must already be installed. This integration
downloads the executable archive and the selected model image as separate artifacts.
