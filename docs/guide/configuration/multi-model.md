# Multi-Model Support

Lemonade supports loading multiple models simultaneously, allowing you to keep frequently-used models in memory for faster switching. The server uses a Least Recently Used (LRU) cache policy to automatically manage model eviction when limits are reached.

## Configuration

Configure via `lemonade config set max_loaded_models=N`. See [Server Configuration](./README.md).

**Default:** `1` (one model of each type). Use `-1` for unlimited.

## Model Types

Models are categorized into these types:

- **LLM** - Chat and completion models (default type)
- **Embedding** - Models for generating text embeddings (identified by the `embeddings` label)
- **Reranking** - Models for document reranking (identified by the `reranking` label)
- **Audio** - Models for audio transcription using Whisper (identified by the `audio` label)
- **Image** - Models for image generation (identified by the `image` label)

Each type has its own independent LRU cache, all sharing the same slot limit set by `max_loaded_models`.

## Device Constraints

- **NPU Exclusivity:** `flm`, `ryzenai-llm`, and `whispercpp` are mutually exclusive on the NPU.
    - Loading a model from one of these backends will automatically evict all NPU models from the other backends.
    - `flm` supports loading 1 ASR model, 1 LLM, and 1 embedding model on the NPU at the same time.
    - `ryzenai-llm` supports loading exactly 1 LLM, which uses the entire NPU.
    - `whispercpp` supports loading exactly 1 ASR model at a time, which uses the entire NPU.
- **CPU/GPU:** No inherent limits beyond available RAM. Multiple models can coexist on CPU or GPU.

## Eviction Policy

When a model slot is full:
1. The least recently used model of that type is evicted
2. The new model is loaded
3. If loading fails (except file-not-found errors), all models are evicted and the load is retried

Models currently processing inference requests cannot be evicted until they finish.

## Per-Model Settings

Each model can be loaded with custom settings (context size, llamacpp backend, llamacpp args) via the `/api/v1/load` endpoint. These per-model settings override the default values set via CLI arguments or environment variables. See the [`/api/v1/load` endpoint documentation](../../api/lemonade.md#post-v1load) for details.

**Setting Priority Order:**
1. Values passed explicitly in `/api/v1/load` request (highest priority)
2. Values from environment variables or server startup arguments (see [Server Configuration](./README.md))
3. Hardcoded defaults in `lemond` (lowest priority)
