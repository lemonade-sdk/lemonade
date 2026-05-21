# Add a Custom Model

This guide explains every supported way to add a custom model to Lemonade Server. Start with the CLI workflows below unless you specifically need to hand-edit `user_models.json` or `recipe_options.json`.

## Choose a Workflow

### Pull a Hugging Face model

For most Hugging Face GGUFs, use the repo id directly:

```bash
lemonade pull org/repo
```

Lemonade fetches the repo, lists the available quantizations and sharded folder variants, auto-detects `mmproj-*.gguf` files for vision models, infers labels (`vision`/`embeddings`/`reranking`) from the repo id, and presents an interactive variant menu.

To skip the menu, append a variant:

```bash
lemonade pull org/repo:Q4_K_M
```

Examples:

```bash
# Interactive GGUF variant menu
lemonade pull unsloth/Qwen3-8B-GGUF

# Specific GGUF variant
lemonade pull unsloth/Qwen3-8B-GGUF:Q4_K_M

# Vision model with mmproj auto-detection
lemonade pull ggml-org/gemma-3-4b-it-GGUF:Q4_K_M

# Sharded variant
lemonade pull unsloth/Qwen3-30B-A3B-GGUF:Q4_K_M
```

### Register with explicit CLI flags

Use a `user.*` name plus `--checkpoint` and `--recipe` when you need full control: multiple checkpoints, a non-default recipe, or custom labels.

```bash
lemonade pull user.NAME --checkpoint TYPE CHECKPOINT --recipe RECIPE [--label LABEL ...]
```

Examples:

```bash
# Register and pull a custom GGUF model with a main checkpoint
lemonade pull user.Phi-4-Mini-GGUF \
    --checkpoint main unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M \
    --recipe llamacpp

# Register and pull a vision model with main + mmproj
lemonade pull user.Gemma-3-4b \
    --checkpoint main ggml-org/gemma-3-4b-it-GGUF:Q4_K_M \
    --checkpoint mmproj ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf \
    --recipe llamacpp

# Register a model with multiple labels
lemonade pull user.MyCodingModel \
    --checkpoint main org/model:Q4_0 \
    --recipe llamacpp \
    --label coding \
    --label tool-calling
```

Supported registration flags:

| Flag | Description |
|------|-------------|
| `--checkpoint TYPE CHECKPOINT` | Add a checkpoint entry. Repeat for multi-file models such as `main` + `mmproj` or `main` + `vae`. |
| `--recipe RECIPE` | Recipe to associate with the new `user.*` model. Common values: `llamacpp`, `flm`, `ryzenai-llm`, `vllm`, `whispercpp`, `sd-cpp`, `kokoro`, `collection.omni`. |
| `--label LABEL` | Add a label to the new model. Repeatable. Valid labels include `coding`, `embeddings`, `hot`, `mtp`, `reasoning`, `reranking`, `tool-calling`, `vision`. |
| `--components MODEL [MODEL ...]` | Components for an omni collection (see below). Use with `--recipe collection.omni`. |

### Register an omni collection

A collection is a meta-model made up of components. An **omni collection** is the recipe type behind [Lemonade Omni Models](../../dev/lemonade-omni.md) — registered with `recipe: "collection.omni"`.

Components must already be registered as built-in models or previously pulled `user.*` models. Components do not need to be downloaded already; missing component files are pulled by the same command.

```bash
lemonade pull user.MyKit \
    --recipe collection.omni \
    --components Qwen3-0.6B-GGUF Whisper-Tiny SD-Turbo
```

`lemonade load user.MyKit` loads every component. `lemonade delete user.MyKit` removes only the collection entry; component files stay on disk.

### Register a custom Omni Model from the desktop app

The desktop app offers a UI-driven path to register the same `recipe: "collection.omni"` entry — useful when you want to swap in a different planner LLM or a different image/ASR/TTS backbone without waiting for a new built-in [Lemonade Omni Model](../../dev/lemonade-omni.md) to ship.

1. Register or download the concrete models you want to use in **Model Manager**.
2. In the desktop app menu bar, open **File > New Omni Model > Manually** (or **From JSON** to import an exported one).
3. Pick one planner LLM and any optional models for image generation, image editing, vision analysis, speech-to-text, and text-to-speech.
4. Save the Omni Model.
5. Select the new `user.<name>` entry in the chat model picker — it appears alongside the built-in omni models under the **Lemonade** category.

Custom Omni Models are registered through the same `POST /v1/pull` path with `recipe: "collection.omni"` that the built-ins and the CLI flow above use. They live under the server's `user.*` namespace, so a custom Omni Model named `MyKit` is addressable as `user.MyKit`. They behave like built-in omni models for routing purposes: the selected planner LLM remains the loop driver that decides when to call tools, and optional role models are only loaded/used when their corresponding tool is called.

The Omni Model editor only offers already-registered compatible models for each role:

| Omni Model role | Tool unlocked | Required model capability |
|---------------|---------------|---------------------------|
| LLM | Chat loop and tool calls | Concrete chat model, preferably tool-calling capable |
| Vision / image analysis | `analyze_image` | `vision` label |
| Image generation | `generate_image` | `image` label |
| Image editing | `edit_image` | `edit` label |
| Speech-to-text | `transcribe_audio` | `audio` or `transcription` label |
| Text-to-speech | `text_to_speech` | `tts` or `speech` label |

If a component model is deleted later, the Omni Model entry remains registered but is hidden from the chat picker until every referenced component is available again.

### Register via API

The `/v1/pull` endpoint accepts the same model registration fields as the CLI. Use this when integrating Lemonade into another app or script:

```bash
curl -X POST http://localhost:13305/v1/pull \
    -H "Content-Type: application/json" \
    -d '{
        "model_name": "user.MyModel",
        "recipe": "llamacpp",
        "checkpoint": "org/repo:Q4_0"
    }'
```

For multi-file models, send `checkpoints`:

```bash
curl -X POST http://localhost:13305/v1/pull \
    -H "Content-Type: application/json" \
    -d '{
        "model_name": "user.Gemma-3-4b",
        "recipe": "llamacpp",
        "checkpoints": {
            "main": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
            "mmproj": "ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf"
        },
        "labels": ["vision"]
    }'
```

For an omni collection, send `components`:

```bash
curl -X POST http://localhost:13305/v1/pull \
    -H "Content-Type: application/json" \
    -d '{
        "model_name": "user.MyKit",
        "recipe": "collection.omni",
        "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
    }'
```

### Edit JSON files directly

Advanced users can edit `user_models.json` and `recipe_options.json` directly. The rest of this guide documents those files and gives complete examples.

## Overview

Custom model configuration involves two files, both located in the Lemonade cache directory:

| File | Purpose |
|------|---------|
| `user_models.json` | Model registry — defines what models are available (checkpoint, recipe, etc.) |
| `recipe_options.json` | Per-model settings — configures how models run (context size, backend, etc.) |

If you used an installer from a Lemonade release, the cache directory is typically:

| OS | Cache directory |
|----|-----------------|
| Linux systemd install | `/var/lib/lemonade/.cache/lemonade` |
| Windows | `%USERPROFILE%\.cache\lemonade` |
| macOS system install | `/Library/Application Support/lemonade/.cache` |

For a standalone `lemond` executable, the default is `~/.cache/lemonade` unless you pass an explicit `cache_dir` argument or set `LEMONADE_CACHE_DIR`.

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
| `recipe` | Yes | String | Backend engine to use. One of: `llamacpp`, `whispercpp`, `sd-cpp`, `kokoro`, `ryzenai-llm`, `flm`, `collection.omni`. |
| `components` | Yes** | Array | Components for a collection. Required when `recipe: "collection.omni"`. See [Collections](#collections). |
| `size` | No | Number | Model size in GB. Informational only — displayed in the UI and used for RAM filtering. |
| `mmproj` | No | String | Filename of the multimodal projector file for llamacpp vision models (must be in the same HuggingFace repo as the checkpoint). This is a **top-level field**, not inside `checkpoints`. |
| `image_defaults` | No | Object | Default image generation parameters for `sd-cpp` models. See [Image defaults](#image-defaults). |

\* Either `checkpoint` or `checkpoints` is required, but not both.
\*\* Required only when `recipe: "collection.omni"`. Collections do not use `checkpoint`/`checkpoints`.

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

### Collections

A collection bundles several already-registered models so they can be loaded, pulled, or deleted as a single entry. Collections do not have their own checkpoint — they reference other models by name. An **omni collection** is a collection type registered with `recipe: "collection.omni"` — this is the recipe behind [Lemonade Omni Models](../../dev/lemonade-omni.md).

```json
{
    "MyKit": {
        "recipe": "collection.omni",
        "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
    }
}
```

Components must already be registered (built-in models, or other `user.*` entries earlier in this file). Loading the collection (`lemonade load user.MyKit`) loads each component; deleting the collection removes only the collection entry, leaving components on disk.

The equivalent CLI registration is shown in [Register an omni collection](#register-an-omni-collection).

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
