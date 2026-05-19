# Custom Model Configuration

This guide explains how to manually register custom models in Lemonade Server using the JSON configuration files. This is useful for adding any HuggingFace model that isn't in the built-in model list.

> **Tip:** For most Hugging Face GGUFs, the easiest way to add a custom model is just:
> ```bash
> lemonade pull org/repo
> ```
> Lemonade fetches the repo, lists the available quantizations (and any sharded folder variants), auto-detects `mmproj-*.gguf` files for vision models, infers labels (`vision`/`embeddings`/`reranking`) from the repo id, and presents an interactive variant menu. To skip the menu, append `:VARIANT`:
> ```bash
> lemonade pull org/repo:Q4_K_M
> ```
> The desktop app's "Search Hugging Face" panel calls the same [`/api/v1/pull/variants`](../../api/lemonade.md#get-v1pullvariants) endpoint under the hood.
>
> If you need full control — multiple checkpoints (`main` + `mmproj` + `vae` + ...), a non-llamacpp recipe, or custom labels — use the advanced flags on [`lemonade pull`](../cli.md#options-for-pull):
> ```bash
> lemonade pull user.MyModel --checkpoint main "org/repo:file.gguf" --recipe llamacpp
> ```
> This guide covers the underlying JSON files for users who need manual control beyond what the CLI exposes.

## Overview

Custom model configuration involves two files, both located in the Lemonade cache directory:

| File | Purpose |
|------|---------|
| `user_models.json` | Model registry — defines what models are available (checkpoint, recipe, etc.) |
| `recipe_options.json` | Per-model settings — configures how models run (context size, backend, etc.) |

See [configuration.md](./README.md) for more information about finding the cache directory.

## Model naming spec

Lemonade tracks three sources of models. Every model has a **canonical ID** of the form `<source>.<bare-name>`:

| Canonical ID    | Source                                                                   |
|-----------------|--------------------------------------------------------------------------|
| `user.NAME`     | Model registered via `lemonade pull` (entry in `user_models.json`)       |
| `extra.NAME`    | Model imported by dropping a GGUF in `--extra-models-dir`                |
| `builtin.NAME`  | Model compiled into Lemonade's built-in catalog (`server_models.json`)   |

The **bare name** `NAME` is an alias that always resolves to whichever source wins precedence for that name. Precedence is **registered > imported > built-in**.

### What the API emits

`/v1/models`, `/v1/models/{id}`, `lemonade list`, and the Ollama `/api/tags` endpoint emit each model with an `id` set to either:

- the **bare name** if the model is the precedence-winner for its bare name, or
- the **canonical-prefixed ID** if another source outranks it on the same bare name.

For each bare name with collisions, the response contains one bare row plus one canonical-prefixed row per shadowed source.

### What input forms are accepted

Anywhere a model name is accepted (request bodies, CLI args, URL path parameters), all four forms work:

- the bare name `NAME` — resolves to the winner
- `user.NAME` — always the registered model (404 if none)
- `extra.NAME` — always the imported model (404 if none)
- `builtin.NAME` — always the built-in model (404 if none)

`lemonade pull` rejects model names starting with `extra.` or `builtin.` since those prefixes are reserved.

### CLI vs. GUI display

The CLI (`lemonade list`) prints the API `id` verbatim. That means the Name column is always copy-paste-safe — every cell is a valid input to `lemonade load`, `lemonade delete`, `lemonade run`, etc.

The Tauri desktop app and the web app apply a display transformation on top of the API id: bare ids render as `NAME`, and canonical-prefixed ids render as `NAME (registered)` / `NAME (imported)` / `NAME (builtin)`. The suffix appears only for shadowed sources.

### Five reference cases

| Sources                                         | `/v1/models` ids                                      | Resolution                                                                 |
|-------------------------------------------------|--------------------------------------------------------|-----------------------------------------------------------------------------|
| built-in `Qwen2.5-Coder` only                   | `Qwen2.5-Coder`                                        | `Qwen2.5-Coder`, `builtin.Qwen2.5-Coder` → built-in                          |
| built-in `Foo` + registered `Foo`               | `Foo`, `builtin.Foo`                                   | `Foo`/`user.Foo` → user; `builtin.Foo` → built-in                            |
| built-in `Bar` + registered `Bar` + extra `Bar` | `Bar`, `extra.Bar`, `builtin.Bar`                      | `Bar`/`user.Bar` → user; `extra.Bar` → extra; `builtin.Bar` → built-in       |
| built-in `Baz` + extra `Baz`                    | `Baz`, `builtin.Baz`                                   | `Baz`/`extra.Baz` → extra; `builtin.Baz` → built-in                          |
| registered `MyModel` only                       | `MyModel`                                              | `MyModel`/`user.MyModel` → user; `builtin.MyModel` → 404                     |

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

This file configures per-model runtime settings. Each key is a **canonical model ID** — one of `user.NAME`, `extra.NAME`, or `builtin.NAME` (see the [Model naming spec](#model-naming-spec) above). Each value contains the settings for that model.

### Template

```json
{
    "user.MyCustomModel": {
        "ctx_size": 4096,
        "llamacpp_backend": "vulkan",
        "llamacpp_args": ""
    },
    "builtin.Qwen2.5-Coder-1.5B-Instruct": {
        "ctx_size": 16384
    }
}
```

> **Migration:** Older Lemonade versions stored built-in entries under their bare name (e.g. `"Qwen2.5-Coder-1.5B-Instruct"` with no prefix). On first load with the current version, any bare key matching a known built-in is rewritten to `builtin.<name>` in place. An INFO log line reports the number of migrated keys. Bare keys that don't match a built-in are preserved unchanged.

> **Note:** Per-model options can also be configured through the Lemonade desktop app's model settings, or via the `save_options` parameter in the [`/api/v1/load` endpoint](../../api/lemonade.md#post-v1load).

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

(Use `builtin.NAME` here if you're overriding a built-in model's defaults, or `extra.NAME` for an `--extra-models-dir` GGUF.)

Then load the model:
```bash
lemonade run user.Qwen2.5-Coder-1.5B-Instruct
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

The model will automatically be available as `user.My-Embedding-Model`. To mark it as an embedding model, use the manual registration flags on `pull`:
```bash
lemonade pull user.My-Embedding-Model \
    --checkpoint main "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S" \
    --recipe llamacpp \
    --label embeddings
```
Or just `lemonade pull nomic-ai/nomic-embed-text-v1-GGUF` — the `embeddings` label is auto-applied because the repo id contains `embed`.

## Settings Priority

When loading a model, settings are resolved in this order (highest to lowest priority):

1. Values explicitly passed in the `/api/v1/load` request
2. Per-model values from `recipe_options.json`
3. Global configuration values, see [Server Configuration](./README.md)

**`*_args` merge behavior:** For options ending in `_args` (e.g., `llamacpp_args`, `whispercpp_args`, `sdcpp_args`, `flm_args`, `vllm_args`), the CLI/API arguments are **merged** rather than replaced. The merge works at the flag level with higher priority settings taking priority.

For full details, see the [load endpoint documentation](../../api/lemonade.md#post-v1load).

## See Also

- [CLI pull command](../cli.md#options-for-pull) — register and download models from the command line
- [`/api/v1/pull` endpoint](../../api/lemonade.md#post-v1pull) — register and download models via API
