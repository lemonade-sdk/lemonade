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
4. The top-level directory a model sits in selects how it runs. `chat`, `embeddings`, and `reranking` are reserved for this; anything else defaults to chat. Filenames are never used to guess. See [Embedding and Reranking Detection](#embedding-and-reranking-detection).

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

### Preserved Folder Names

When a folder is split into variants, its folder name is still accepted in requests as a hidden input alias, resolving to the first variant alphabetically. It is not listed as an extra model, so existing scripts keep working without a duplicate entry appearing in `/api/v1/models`.

Reserving a directory has the same effect. Any directory holding GGUF files is listed as a single model named after that directory, so `embeddings/` produced `extra.embeddings`. Once the directory is reserved, its files are listed separately instead.

For a directory holding `all-MiniLM-L6-v2.gguf` and `nomic-embed-text-v2.gguf`:

| | Model ids |
|---|---|
| Before it was reserved | `extra.embeddings` |
| After | `extra.all-MiniLM-L6-v2`, `extra.nomic-embed-text-v2` |

`extra.embeddings` is still accepted in requests so existing configs keep working. It resolves to the first file alphabetically, which is the file the single model used.

### Multimodal Detection

If a model directory contains a file with `mmproj` anywhere in the filename, it is automatically set as the model's `mmproj` field and the `vision` label is applied. When several `mmproj` files are present, the first by filename is chosen, so the selection is stable across restarts.

A direct `mmproj` inside a reserved directory is attached only when that directory contains one logical main model. Convention: if a reserved directory contains multiple multimodal models, place each model and its `mmproj` together in their own subdirectory. This makes it clear which files belong together.

### Embedding and Reranking Detection

The folder a model sits in tells the server how to run it, and filenames are never used to guess. The top-level directory selects the deployment mode, and the reserved directories are `chat`, `embeddings`, and `reranking`:

```text
extra_models_dir/
├── chat/
├── embeddings/
└── reranking/
```

Files directly inside a reserved directory are listed as separate models. Files whose numbered shard names declare that they belong together are grouped as one model. Nested folder models and split variants inherit the mode of their reserved top-level directory. Models at the root or under any other directory default to chat.

Reserved directory names must match exactly. `embeddings` is reserved; `Embedding`, `embedding`, and `embeddings 2` are ordinary directories.

The server does not try to infer near-matches because doing so could accidentally select the wrong runtime behavior. Filenames are not used to infer model type for the same reason, so `embeddings/bge-reranker-v2.gguf` is still an embedding model.

## Model Properties

Discovered models receive the following default properties:

| Property | Value |
|----------|-------|
| `recipe` | `llamacpp` |
| `suggested` | `true` |
| `downloaded` | `true` |
| `labels` | `["custom"]`, the directory-selected mode (or `"chat"` by default), and `"vision"` if multimodal |
| `type` | Derived from `labels` |
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

Names are assigned in a fixed order so the result is stable across restarts and platforms: files at the root of `extra_models_dir` first, then files inside reserved directories, then directory models, each group in sorted path order.

Root files come first so that adding a reserved directory later never renames a model that already exists:

| File | Model Name |
|------|------------|
| `nomic-embed-text-v2.gguf` | `extra.nomic-embed-text-v2` |
| `embeddings/nomic-embed-text-v2.gguf` | `extra.embeddings-nomic-embed-text-v2` |

## Model Deletion

Models discovered from `--extra-models-dir` cannot be deleted via the API. They are user-managed external files. Attempting to delete an `extra.*` model will return an error with the file path for manual deletion.

Example error:
```
Cannot delete extra models via API. Models in --extra-models-dir are user-managed. Delete the file directly from: C:\Users\Jeremy\.lmstudio\models\Qwen3-8B-Q4_K_M.gguf
```
