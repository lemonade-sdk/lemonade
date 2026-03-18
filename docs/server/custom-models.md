# Custom Model Configuration

This guide explains how to manually register custom models in Lemonade Server using the JSON configuration files. This is useful for adding any HuggingFace model that isn't in the built-in model list.

> **Tip:** The easiest way to add a custom model is using the [`lemonade-server pull` CLI command](./lemonade-server-cli.md#options-for-pull) or the [`/api/v1/pull` endpoint](./server_spec.md#post-apiv1pull), which automate the registration and download process. For example:
> ```bash
> lemonade-server pull user.MyModel --checkpoint "org/repo:file.gguf" --recipe llamacpp
> ```
> This guide covers the underlying JSON files for users who need manual control.

## Overview

Custom model configuration involves two files, both located in the Lemonade cache directory:

| File | Purpose |
|------|---------|
| `user_models.json` | Model registry — defines what models are available (checkpoint, recipe, etc.) |
| `recipe_options.json` | Per-model settings — configures how models run (context size, backend, etc.) |

**Cache directory location:**

| OS | Default Path |
|----|-------------|
| Windows | `%USERPROFILE%\.cache\lemonade\` |
| Linux | `~/.cache/lemonade/` |
| macOS | `~/.cache/lemonade/` |

The cache directory can be overridden by setting the `LEMONADE_CACHE_DIR` environment variable.

## `user_models.json` Reference

This file contains a JSON object where each key is a model name and each value defines the model's properties. Create this file in your cache directory if it doesn't exist.

### Template

```json
{
    "MyCustomModel": {
        "checkpoint": "org/repo-name:filename.gguf",
        "recipe": "llamacpp",
        "size": 3.5
    }
}
```

### Fields

| Field | Required | Type | Description |
|-------|----------|------|-------------|
| `checkpoint` | Yes* | String | HuggingFace checkpoint in `org/repo` or `org/repo:variant` format. Use `org/repo:filename.gguf` for GGUF models. |
| `checkpoints` | Yes* | Object | Alternative to `checkpoint` for models with multiple files. See [Multi-file models](#multi-file-models). |
| `recipe` | Yes | String | Backend engine to use. One of: `llamacpp`, `whispercpp`, `sd-cpp`, `kokoro`, `ryzenai-llm`, `flm`. |
| `size` | No | Number | Model size in GB. Informational only — displayed in the UI and used for RAM filtering. |
| `mmproj` | No | String | Filename of the multimodal projector file for llamacpp vision models (must be in the same HuggingFace repo as the checkpoint). This is a **top-level field**, not inside `checkpoints`. |
| `image_defaults` | No | Object | Default image generation parameters for `sd-cpp` models. See [Image defaults](#image-defaults). |

\* Either `checkpoint` or `checkpoints` is required, but not both.

### Checkpoint format

The `checkpoint` field uses the format `org/repo:variant`:

- **GGUF models (exact filename)**: `org/repo:filename.gguf` — e.g., `Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF:qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`
- **GGUF models (quantization shorthand)**: `org/repo:QUANT` — e.g., `unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M`. The server will search the repo for a matching `.gguf` file.
- **ONNX models**: `org/repo` — e.g., `amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx`
- **Safetensor models**: `org/repo:filename.safetensors` — e.g., `stabilityai/sd-turbo:sd_turbo.safetensors`

### Multi-file models

For models that require multiple files (e.g., Whisper models with NPU cache, or Flux image models with separate VAE/text encoder), use `checkpoints` instead of `checkpoint`:

```json
{
    "My-Whisper-Model": {
        "checkpoints": {
            "main": "ggerganov/whisper.cpp:ggml-tiny.bin",
            "npu_cache": "amd/whisper-tiny-onnx-npu:ggml-tiny-encoder-vitisai.rai"
        },
        "recipe": "whispercpp",
        "size": 0.075
    }
}
```

Supported checkpoint keys:

| Key | Used by | Description |
|-----|---------|-------------|
| `main` | All | Primary model file |
| `npu_cache` | whispercpp | NPU-accelerated encoder cache |
| `text_encoder` | sd-cpp | Text encoder for image generation models |
| `vae` | sd-cpp | VAE for image generation models |

### Image defaults

For `sd-cpp` recipe models, you can specify default image generation parameters:

```json
{
    "My-SD-Model": {
        "checkpoint": "org/repo:model.safetensors",
        "recipe": "sd-cpp",
        "size": 5.2,
        "image_defaults": {
            "steps": 20,
            "cfg_scale": 7.0,
            "width": 512,
            "height": 512
        }
    }
}
```

### Model naming

- In `user_models.json`, store model names **without** the `user.` prefix (e.g., `MyCustomModel`).
- When referencing the model in API calls, CLI commands, or `recipe_options.json`, use the **full prefixed name** (e.g., `user.MyCustomModel`).
- Labels like `custom` are added automatically. Additional labels (`reasoning`, `vision`, `embeddings`, `reranking`) can be set via the `pull` CLI/API flags, or by including a `labels` array in the JSON entry.

## `recipe_options.json` Reference

This file configures per-model runtime settings. Each key is a **full model name** (including prefix like `user.` or `extra.`) and each value contains the settings for that model.

### Template

```json
{
    "user.MyCustomModel": {
        "ctx_size": 4096,
        "llamacpp_backend": "vulkan",
        "llamacpp_args": ""
    }
}
```

### Options by recipe

#### llamacpp

| Option | Default | Env Variable | Description |
|--------|---------|-------------|-------------|
| `ctx_size` | 4096 | `LEMONADE_CTX_SIZE` | Context window size in tokens |
| `llamacpp_backend` | vulkan (Windows/Linux), metal (macOS) | `LEMONADE_LLAMACPP` | Inference backend: `vulkan`, `rocm`, `cpu`, `metal` |
| `llamacpp_args` | (empty) | `LEMONADE_LLAMACPP_ARGS` | Extra arguments passed to llama-server |

#### whispercpp

| Option | Default | Env Variable | Description |
|--------|---------|-------------|-------------|
| `whispercpp_backend` | npu | `LEMONADE_WHISPERCPP` | Backend: `npu`, `cpu`, `vulkan` |

#### sd-cpp

| Option | Default | Env Variable | Description |
|--------|---------|-------------|-------------|
| `sd-cpp_backend` | cpu | `LEMONADE_SDCPP` | Backend: `cpu`, `rocm` |
| `steps` | 20 | `LEMONADE_STEPS` | Number of inference steps |
| `cfg_scale` | 7.0 | `LEMONADE_CFG_SCALE` | Classifier-free guidance scale |
| `width` | 512 | `LEMONADE_WIDTH` | Image width in pixels |
| `height` | 512 | `LEMONADE_HEIGHT` | Image height in pixels |

#### flm

| Option | Default | Env Variable | Description |
|--------|---------|-------------|-------------|
| `ctx_size` | 4096 | `LEMONADE_CTX_SIZE` | Context window size in tokens |
| `flm_args` | (empty) | `LEMONADE_FLM_ARGS` | Extra arguments passed to `flm serve` |

#### ryzenai-llm

| Option | Default | Env Variable | Description |
|--------|---------|-------------|-------------|
| `ctx_size` | 4096 | `LEMONADE_CTX_SIZE` | Context window size in tokens |

#### kokoro

The `kokoro` recipe (text-to-speech) has no configurable options in `recipe_options.json`.

> **Note:** Per-model options can also be configured through the Lemonade desktop app's model settings, or via the `save_options` parameter in the [`/api/v1/load` endpoint](./server_spec.md#post-apiv1load).

## Complete Examples

### Example 1: Adding a GGUF LLM with large context

**`user_models.json`:**
```json
{
    "Qwen2.5-Coder-1.5B-Instruct": {
        "checkpoint": "Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF:qwen2.5-coder-1.5b-instruct-q4_k_m.gguf",
        "recipe": "llamacpp",
        "size": 1.0
    }
}
```

**`recipe_options.json`:**
```json
{
    "user.Qwen2.5-Coder-1.5B-Instruct": {
        "ctx_size": 16384,
        "llamacpp_backend": "vulkan"
    }
}
```

Then load the model:
```bash
lemonade-server run user.Qwen2.5-Coder-1.5B-Instruct
```

### Example 2: Adding a vision model with mmproj

**`user_models.json`:**
```json
{
    "My-Vision-Model": {
        "checkpoint": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
        "mmproj": "mmproj-model-f16.gguf",
        "recipe": "llamacpp",
        "size": 3.61
    }
}
```

### Example 3: Adding an embedding model

**`user_models.json`:**
```json
{
    "My-Embedding-Model": {
        "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
        "recipe": "llamacpp",
        "size": 0.08
    }
}
```

The model will automatically be available as `user.My-Embedding-Model`. To mark it as an embedding model, use the pull CLI instead:
```bash
lemonade-server pull user.My-Embedding-Model \
    --checkpoint "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S" \
    --recipe llamacpp \
    --embedding
```

## Settings Priority

When loading a model, settings are resolved in this order (highest to lowest priority):

1. Values explicitly passed in the `/api/v1/load` request
2. Per-model values from `recipe_options.json`
3. Values set via `lemonade-server` CLI arguments or environment variables
4. Default hardcoded values in `lemonade-router`

For full details, see the [load endpoint documentation](./server_spec.md#post-apiv1load).

## See Also

- [CLI pull command](./lemonade-server-cli.md#options-for-pull) — register and download models from the command line
- [`/api/v1/pull` endpoint](./server_spec.md#post-apiv1pull) — register and download models via API
- [Server Integration Guide](./server_integration.md#installing-additional-models) — overview of model management options
