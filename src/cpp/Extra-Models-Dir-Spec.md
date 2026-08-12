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

### Access and Failure Behavior

When `extra_models_dir` is updated at runtime, an existing path must be a directory that the `lemond` process can enumerate. Permission and I/O failures reject the config update. A path that does not exist yet is accepted so the directory watcher can observe it if it is created later.

During discovery, inaccessible nested directories are skipped. Extra-model discovery is optional: a filesystem failure must not remove or hide models from `server_models.json` or `user_models.json`.

### Naming Convention

All discovered models are prefixed with `extra.` to prevent naming conflicts with registered models (similar to how user-added models are prefixed with `user.`):

| Directory Structure | Model Name |
|---------------------|------------|
| `Qwen3-8B-Q4_K_M.gguf` | `extra.Qwen3-8B-Q4_K_M.gguf` |
| `gemma-3-4b-it-Q8_0/*.gguf` | `extra.gemma-3-4b-it-Q8_0` |

This allows users to have both a registered model (e.g., `Qwen3-Coder-30B-A3B-Instruct-GGUF`) and a custom GGUF variant (e.g., `extra.Qwen3-Coder-30B-A3B-Instruct-GGUF`) without conflict.

### Directory-Based Models

A subdirectory holding several distinct model variants is listed as one model per variant, so every version in the folder can be selected:

| Directory contents | Models listed |
|--------------------|---------------|
| `Qwen3-8B-Q4_K_M.gguf`, `Qwen3-8B-Q8_0.gguf` | `extra.Qwen3-8B-Q4_K_M`, `extra.Qwen3-8B-Q8_0` |
| `model-00001-of-00002.gguf`, `model-00002-of-00002.gguf` | `extra.<folder name>` (one model) |

The folder stays a single model when splitting would be ambiguous: a single shard set, or files that do not all belong to a named variant. This still supports:

- **Multimodal models:** Directory contains a main `.gguf` file and an `mmproj*.gguf` file.
- **Multi-shard models:** Directory contains multiple numbered shard files (e.g., `*-00001-of-00006.gguf`).

### Shard Grouping

Files are merged into one model only when their names declare the same shard series, such as `model-Q4_K_M-00001-of-00003.gguf`. The `-`, `.` and `_` separators are all accepted before the shard index.

Sharing a quantization token is not sufficient. `Model-Q4_K_M.gguf` and `Model-Q4_K_M-imatrix.gguf` are two independent models and are listed separately.

### Legacy Folder Names

When a folder is split into variants, its folder name is still accepted in requests as a hidden input alias, resolving to the first variant alphabetically. It is not listed as an extra model, so existing scripts keep working without a duplicate entry appearing in `/api/v1/models`.

### Multimodal Detection

If a directory contains a file with `mmproj` anywhere in the filename, it is automatically set as the model's `mmproj` field and the `vision` label is applied. When several `mmproj` files are present, the first by filename is chosen, so the selection is stable across restarts.

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

Two scanned directories can contain identically named GGUF files. The first model found keeps the plain name; the second is qualified with its directory name, so neither is lost:

| File | Model Name |
|------|------------|
| `Llama-Local-GGUF/model-Q4_K_M.gguf` | `extra.model-Q4_K_M` |
| `Mistral-Local-GGUF/model-Q4_K_M.gguf` | `extra.Mistral-Local-GGUF-model-Q4_K_M` |

Directories are scanned in sorted path order, so the assignment is stable across restarts.

## Model Deletion

Models discovered from `--extra-models-dir` cannot be deleted via the API. They are user-managed external files. Attempting to delete an `extra.*` model will return an error with the file path for manual deletion.

Example error:
```
Cannot delete extra models via API. Models in --extra-models-dir are user-managed. Delete the file directly from: C:\Users\Jeremy\.lmstudio\models\Qwen3-8B-Q4_K_M.gguf
```
