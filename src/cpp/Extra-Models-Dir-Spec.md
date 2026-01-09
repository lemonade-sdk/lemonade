# Extra Models Directory Specification

Lemonade Server supports discovering GGUF models from a secondary directory in addition to the HuggingFace cache. This enables compatibility with llama.cpp's model cache and user-managed model directories.

## CLI Argument

The `--extra-models-dir PATH` argument specifies a secondary directory to scan for GGUF models.

**Default value:** None (feature is disabled unless explicitly enabled)

**Suggested paths:**
- **Windows:** `%LOCALAPPDATA%\llama.cpp`
- **Linux/macOS:** `~/.cache/llama.cpp`

## Model Discovery

### Scanning Rules

1. The directory is scanned recursively for `.gguf` files.
2. Discovered models are added to the model list alongside registered models from `server_models.json` and `user_models.json`.
3. HuggingFace cache remains the primary source for registered models.

### Naming Convention

All discovered models are prefixed with `extra.` to prevent naming conflicts with registered models (similar to how user-added models are prefixed with `user.`):

| Directory Structure | Model Name |
|---------------------|------------|
| `Qwen3-8B-Q4_K_M.gguf` | `extra.Qwen3-8B-Q4_K_M.gguf` |
| `gemma-3-4b-it-Q8_0/*.gguf` | `extra.gemma-3-4b-it-Q8_0` |

This allows users to have both a registered model (e.g., `Qwen3-Coder-30B-A3B-Instruct-GGUF`) and a custom GGUF variant (e.g., `extra.Qwen3-Coder-30B-A3B-Instruct-GGUF`) without conflict.

### Directory-Based Models

Models in subdirectories are treated as a single model. This supports:

- **Multimodal models:** Directory contains a main `.gguf` file and an `mmproj*.gguf` file.
- **Multi-shard models:** Directory contains multiple numbered shard files (e.g., `*-00001-of-00006.gguf`).

### Multimodal Detection

If a directory contains a file with `mmproj` anywhere in the filename, it is automatically set as the model's `mmproj` field and the `vision` label is applied.

## Model Properties

Discovered models receive the following default properties:

| Property | Value |
|----------|-------|
| `recipe` | `llamacpp` |
| `suggested` | `true` |
| `downloaded` | `true` |
| `labels` | `["custom"]` (plus `"vision"` if multimodal) |
| `size` | Sum of all `.gguf` file sizes in GB |
| `source` | `extra_models_dir` |

## Conflict Resolution

The `extra.` prefix ensures discovered models never conflict with registered models. Both can coexist:

- Registered: `Qwen3-Coder-30B-A3B-Instruct-GGUF` (from `server_models.json`)
- Discovered: `extra.Qwen3-Coder-30B-A3B-Instruct-GGUF` (from `--extra-models-dir`)

## Model Deletion

Models discovered from `--extra-models-dir` cannot be deleted via the API. They are user-managed external files. Attempting to delete an `extra.*` model will return an error with the file path for manual deletion.

Example error:
```
Cannot delete extra models via API. Models in --extra-models-dir are user-managed. Delete the file directly from: C:\Users\Jeremy\.lmstudio\models\Qwen3-8B-Q4_K_M.gguf
```
