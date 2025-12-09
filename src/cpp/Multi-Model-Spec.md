# Multi-Model Specification

Lemonade Server supports loading multiple models simultaneously. Each model runs in its own WrappedServer instance, and a set of rules governs when WrappedServers are evicted or kept in memory.

## Eviction Logic

### Max Loaded Models

The `--max-loaded-models LLMS EMBEDDINGS RERANKINGS AUDIO` argument controls how many WrappedServers can be in memory by type.

Models are categorized into four types:
- `llm`: Chat and completion models (default type)
- `embedding`: Models with the `embedding` label
- `reranking`: Models with the `reranking` label
- `audio`: Models with the `audio` label (e.g., Whisper)

The default value is `1 1 1 1`:
- 1x `llm` WrappedServer
- 1x `embedding` WrappedServer
- 1x `reranking` WrappedServer
- 1x `audio` WrappedServer

Examples:
- `--max-loaded-models 3` → 3 LLMs, 1 embedding, 1 reranking, 1 audio (missing values default to 1)
- `--max-loaded-models 5 2 4 2` → 5 LLMs, 2 embedding, 4 reranking, 2 audio

ModelInfo and WrappedServer include an explicit enum field for tracking the model type. This field is used throughout the codebase (e.g., in `llamacpp_server.cpp`) to adjust settings based on model type.

### Least Recently Used Cache

When loading a new WrappedServer of a given TYPE:
1. If a slot is available for that TYPE, the new WrappedServer loads without evicting existing ones.
2. If no slot is available, the least-recently-used WrappedServer of that TYPE is evicted first.

Each WrappedServer maintains a timestamp of its most recent access. The following operations update this timestamp:
- WrappedServer load start and completion
- Inference request start and completion (chat/completions, completions, responses, embedding, reranking)

### Error Handling

If a WrappedServer load fails (with exceptions noted below), all WrappedServers of every type are evicted, and the load is re-attempted.

This "nuclear" policy simplifies implementation while remaining effective in practice.

**Exception:** Models are not evicted if the load failed because a file was not found on disk.

### NPU Exclusivity

Only one model can occupy the NPU at a time. The following recipes use the NPU:
- `oga-hybrid`
- `oga-npu`
- `flm`

When loading a WrappedServer with an NPU recipe, any existing NPU-using WrappedServer is evicted regardless of type or `--max-loaded-models` settings.

> Note: CPU and GPU have no inherent model limit beyond available RAM. Since RAM usage is not tracked, the error handling policy (evict-all-on-failure) serves as the fallback mechanism.

**Recipe to Device Mapping:**
| Recipe | Device(s) |
|--------|-----------|
| `llamacpp` | gpu |
| `oga-hybrid` | gpu, npu |
| `oga-npu` | npu |
| `oga-cpu` | cpu |
| `flm` | npu |

ModelInfo and WrappedServer include a bitmask enum field for tracking target devices, enabling checks like `if (model.device & Device::NPU)`.

## Concurrency

1. **Busy WrappedServers are protected:** A WrappedServer actively fulfilling an inference request cannot be evicted until it finishes. Pending loads queue indefinitely.

2. **Serialized loading:** Only one WrappedServer loads at a time. Eviction decisions are made when a queued load begins (not when it enters the queue).

3. **Auto-load protection:** When a WrappedServer is auto-loaded for an inference request, the request executes before that WrappedServer becomes eligible for eviction.
   - Auto-load: If an inference request arrives for an unloaded model, Lemonade loads a WrappedServer for that model, then forwards the request.
