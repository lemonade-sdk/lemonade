# Lemonade Server Spec

The Lemonade Server is a standards-compliant server process that provides an HTTP API to enable integration with other applications.

Lemonade Server currently supports these backends:

| Backend                                                                 | Model Format | Description                                                                                                                |
|----------------------------------------------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------------|
| [Llama.cpp](https://github.com/ggml-org/llama.cpp)    | `.GGUF`      | Uses llama.cpp's `llama-server` backend. More details [here](#gguf-support).                    |
| [ONNX Runtime GenAI (OGA)](https://github.com/microsoft/onnxruntime-genai) | `.ONNX`      | Uses Lemonade's own `ryzenai-server` backend.                                                |
| [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM)    | `.q4nx`      | Uses FLM's `flm serve` backend. More details [here](#fastflowlm-support).                    |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | `.bin` | Uses whisper.cpp's `whisper-server` backend for audio transcription. Models: Whisper-Tiny, Whisper-Base, Whisper-Small. |
| [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) | `.safetensors` | Uses sd.cpp's `sd-cli` backend for image generation. Models: SD-Turbo, SDXL-Turbo, etc. |
| [Kokoros](https://github.com/lucasjinreal/Kokoros) | `.onnx` | Uses Kokoro's `koko` backend for speech generation. Models: kokoro-v1 |


## Endpoints Overview

The [key endpoints of the OpenAI API](#openai-compatible-endpoints) are available.

We are also actively investigating and developing [additional endpoints](#lemonade-specific-endpoints) that will improve the experience of local applications.

### OpenAI-Compatible Endpoints
- POST `/api/v1/chat/completions` - Chat Completions (messages -> completion)
- POST `/api/v1/completions` - Text Completions (prompt -> completion)
- POST `/api/v1/embeddings` - Embeddings (text -> vector representations)
- POST `/api/v1/responses` - Chat Completions (prompt|messages -> event)
- POST `/api/v1/audio/transcriptions` - Audio Transcription (audio file -> text)
- POST `/api/v1/audio/speech` - Text to speech (text -> audio)
- WS `/realtime` - Realtime Audio Transcription (streaming audio -> text, OpenAI SDK compatible)
- WS `/logs/stream` - Log Streaming (subscribe -> snapshot + live log entries)
- POST `/api/v1/images/generations` - Image Generation (prompt -> image)
- POST `/api/v1/images/edits` - Image Editing (image + prompt -> edited image)
- POST `/api/v1/images/variations` - Image Variations (image -> varied image)
- POST `/api/v1/images/upscale` - Image Upscaling (image + ESRGAN model -> upscaled image)
- GET `/api/v1/models` - List models available locally
- GET `/api/v1/models/{model_id}` - Retrieve a specific model by ID

### llama.cpp Endpoints

These endpoints defined by `llama.cpp` extend the OpenAI-compatible API with additional functionality.

- POST `/api/v1/reranking` - Reranking (query + documents -> relevance-scored documents)

### Lemonade-Specific Endpoints

We have designed a set of Lemonade-specific endpoints to enable client applications by extending the existing cloud-focused APIs (e.g., OpenAI). These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:

- Download models at setup time.
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Unload models to save memory space.
- Understand system resources and state to make dynamic choices.

The additional endpoints are:

- POST `/api/v1/install` - Install or update a backend
- POST `/api/v1/uninstall` - Remove a backend
- POST `/api/v1/pull` - Install a model
- GET `/api/v1/pull/variants` - Enumerate GGUF variants for a Hugging Face checkpoint
- POST `/api/v1/delete` - Delete a model
- POST `/api/v1/load` - Load a model
- POST `/api/v1/unload` - Unload a model
- GET `/api/v1/health` - Check server status, such as models loaded
- GET `/api/v1/stats` - Performance statistics from the last request
- GET `/api/v1/system-info` - System information and device enumeration
- GET `/live` - Check server liveness for load balancers and orchestrators

### Ollama-Compatible API

Lemonade supports the [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), allowing applications built for Ollama to work with Lemonade without modification.

To enable auto-detection by Ollama-integrated apps, configure the server to use the Ollama default port. See [Server Configuration](./configuration.md#environment-variables) for how to change the port.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /api/chat` | Supported | Streaming and non-streaming |
| `POST /api/generate` | Supported | Text completion + image generation |
| `GET /api/tags` | Supported | Lists downloaded models |
| `POST /api/show` | Supported | Model details |
| `DELETE /api/delete` | Supported | |
| `POST /api/pull` | Supported | Download with progress |
| `POST /api/embed` | Supported | New embeddings format |
| `POST /api/embeddings` | Supported | Legacy embeddings |
| `GET /api/ps` | Supported | Running models |
| `GET /api/version` | Supported | |
| `POST /api/create` | Not supported | Returns 501 |
| `POST /api/copy` | Not supported | Returns 501 |
| `POST /api/push` | Not supported | Returns 501 |

### Anthropic-Compatible API (Initial)

Lemonade supports an initial Anthropic Messages compatibility endpoint for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |

Current scope focuses on message generation parity for common fields (`model`, `messages`, `system`, `max_tokens`, `temperature`, `stream`, and basic `tools`). Unsupported or unimplemented Anthropic-specific fields are ignored and surfaced via warning logs/headers.

## Multi-Model Support

Lemonade Server supports loading multiple models simultaneously, allowing you to keep frequently-used models in memory for faster switching. The server uses a Least Recently Used (LRU) cache policy to automatically manage model eviction when limits are reached.

### Configuration

Configure via `lemonade config set max_loaded_models=N`. See [Server Configuration](./configuration.md).

**Default:** `1` (one model of each type). Use `-1` for unlimited.

### Model Types

Models are categorized into these types:

- **LLM** - Chat and completion models (default type)
- **Embedding** - Models for generating text embeddings (identified by the `embeddings` label)
- **Reranking** - Models for document reranking (identified by the `reranking` label)
- **Audio** - Models for audio transcription using Whisper (identified by the `audio` label)
- **Image** - Models for image generation (identified by the `image` label)

Each type has its own independent LRU cache, all sharing the same slot limit set by `max_loaded_models`.

### Device Constraints

- **NPU Exclusivity:** `flm`, `ryzenai-llm`, and `whispercpp` are mutually exclusive on the NPU.
    - Loading a model from one of these backends will automatically evict all NPU models from the other backends.
    - `flm` supports loading 1 ASR model, 1 LLM, and 1 embedding model on the NPU at the same time.
    - `ryzenai-llm` supports loading exactly 1 LLM, which uses the entire NPU.
    - `whispercpp` supports loading exactly 1 ASR model at a time, which uses the entire NPU.
- **CPU/GPU:** No inherent limits beyond available RAM. Multiple models can coexist on CPU or GPU.

### Eviction Policy

When a model slot is full:
1. The least recently used model of that type is evicted
2. The new model is loaded
3. If loading fails (except file-not-found errors), all models are evicted and the load is retried

Models currently processing inference requests cannot be evicted until they finish.

### Per-Model Settings

Each model can be loaded with custom settings (context size, llamacpp backend, llamacpp args) via the `/api/v1/load` endpoint. These per-model settings override the default values set via CLI arguments or environment variables. See the [`/api/v1/load` endpoint documentation](#post-apiv1load) for details.

**Setting Priority Order:**
1. Values passed explicitly in `/api/v1/load` request (highest priority)
2. Values from environment variables or server startup arguments (see [Server Configuration](./configuration.md))
3. Hardcoded defaults in `lemond` (lowest priority)

## Start the HTTP Server

> **NOTE:** This server is intended for use on local systems only. Do not expose the server port to the open internet.

Lemonade Server starts automatically with the OS after installation. See the [Getting Started instructions](./README.md). For server configuration options, see [Server Configuration](./configuration.md).

## OpenAI-Compatible Endpoints




## Additional Endpoints

### `POST /api/v1/pull` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register and install models for use with Lemonade Server.

#### Parameters

The Lemonade Server built-in model registry has a collection of model names that can be pulled and loaded. The `pull` endpoint can install any registered model, and it can also register-then-install any model available on Hugging Face.

**Common Parameters**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `stream` | No | If `true`, returns Server-Sent Events (SSE) with download progress. Defaults to `false`. |

**Install a Model that is Already Registered**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to install. |

Example request:

```bash
curl -X POST http://localhost:13305/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Installed model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

**Register and Install a Model**

Registration will place an entry for that model in the `user_models.json` file, which is located in the user's Lemonade cache (default: `~/.cache/lemonade`). Then, the model will be installed. Once the model is registered and installed, it will show up in the `models` endpoint alongside the built-in models and can be loaded.

The `recipe` field defines which software framework and device will be used to load and run the model. For more information on OGA and Hugging Face recipes, see the [Lemonade API README](../lemonade_api.md). For information on GGUF recipes, see [llamacpp](#gguf-support).

> Note: the `model_name` for registering a new model must use the `user` namespace, to prevent collisions with built-in models. For example, `user.Phi-4-Mini-GGUF`.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | Namespaced [Lemonade Server model name](https://lemonade-server.ai/models.html) to register and install. |
| `checkpoint` | Yes | HuggingFace checkpoint to install. |
| `recipe` | Yes | Lemonade API recipe to load the model with. |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). Adds 'reasoning' label. |
| `vision` | No | Whether the model has vision capabilities for processing images (default: false). Adds 'vision' label. |
| `embedding` | No | Whether the model is an embedding model (default: false). Adds 'embeddings' label. |
| `reranking` | No | Whether the model is a reranking model (default: false). Adds 'reranking' label. |
| `mmproj` | No | Multimodal Projector (mmproj) file to use for vision models. |

Example request:

```bash
curl -X POST http://localhost:13305/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Installed model: user.Phi-4-Mini-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

#### Streaming Response (stream=true)

When `stream=true`, the endpoint returns Server-Sent Events with real-time download progress:

```
event: progress
data: {"file":"model.gguf","file_index":1,"total_files":2,"bytes_downloaded":1073741824,"bytes_total":2684354560,"percent":40}

event: progress
data: {"file":"config.json","file_index":2,"total_files":2,"bytes_downloaded":1024,"bytes_total":1024,"percent":100}

event: complete
data: {"file_index":2,"total_files":2,"percent":100}
```

**Event Types:**

| Event | Description |
|-------|-------------|
| `progress` | Sent during download with current file and byte progress |
| `complete` | Sent when all files are downloaded successfully |
| `error` | Sent if download fails, with `error` field containing the message |

### `GET /api/v1/pull/variants` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Inspect a Hugging Face GGUF repository and enumerate the variants (quantizations and sharded folder groups) available for installation. Used by the `lemonade pull <owner/repo>` CLI flow and by the desktop app's model search to auto-populate the install form. The endpoint reads only public Hugging Face metadata; if the `HF_TOKEN` environment variable is set on the server, it is forwarded as a bearer token to access gated repositories.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `checkpoint` | Yes | Hugging Face repo id, e.g. `unsloth/Qwen3-8B-GGUF`. Passed as a query string. |

Example request:

```bash
curl 'http://localhost:13305/api/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF'
```

#### Response

```json
{
  "checkpoint": "unsloth/Qwen3-8B-GGUF",
  "recipe": "llamacpp",
  "suggested_name": "Qwen3-8B-GGUF",
  "suggested_labels": ["vision"],
  "mmproj_files": ["mmproj-model-f16.gguf"],
  "variants": [
    {
      "name": "Q4_K_M",
      "primary_file": "Qwen3-8B-Q4_K_M.gguf",
      "files": ["Qwen3-8B-Q4_K_M.gguf"],
      "sharded": false,
      "size_bytes": 4920000000
    },
    {
      "name": "Q8_0",
      "primary_file": "Q8_0/Qwen3-8B-Q8_0-00001-of-00002.gguf",
      "files": ["Q8_0/Qwen3-8B-Q8_0-00001-of-00002.gguf", "Q8_0/Qwen3-8B-Q8_0-00002-of-00002.gguf"],
      "sharded": true,
      "size_bytes": 8500000000
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `checkpoint` | Echoed input. |
| `recipe` | Suggested recipe (always `llamacpp` today; future expansion may return other values). |
| `suggested_name` | Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name. |
| `suggested_labels` | Inferred labels — `vision` if any `mmproj-*.gguf` files exist, plus `embeddings`/`reranking` if those substrings appear in the repo id. |
| `mmproj_files` | Bare filenames of `mmproj-*.gguf` files in the repo; the first one should be passed as `mmproj` to `/api/v1/pull` for vision models. |
| `variants[]` | Top quantizations for the repo, capped at 5. Each entry has `name` (e.g. `Q4_K_M`, `UD-Q4_K_XL`), `primary_file`, `files`, `sharded`, and `size_bytes` (from the HF `?blobs=true` listing). Ranked by frequency of use in `server_models.json` (`Q4_K_M`, `UD-Q4_K_XL`, `Q8_0`, `Q4_0` first, everything else sorted lexicographically). The CLI `lemonade pull` menu adds a free-text "Other" option for quants outside the top 5. |

#### Error responses

| Status | Cause |
|--------|-------|
| 400 | `checkpoint` query parameter missing or malformed (must contain `/`). |
| 404 | Hugging Face returned 404 for the checkpoint. |
| 500 | Other transport or parsing failures; the response body contains an `error` message. |

### `POST /api/v1/delete` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Delete a model by removing it from local storage. If the model is currently loaded, it will be unloaded first.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to delete. |

Example request:

```bash
curl -X POST http://localhost:13305/api/v1/delete \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Deleted model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

<a id="post-apiv1load"></a>
### `POST /api/v1/load` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a registered model into memory. This is useful to ensure that the model is loaded before you make a request. Installs the model if necessary.

#### Parameters

| Parameter | Required | Applies to | Description |
|-----------|----------|------------|-------------|
| `model_name` | Yes | All | [Lemonade Server model name](https://lemonade-server.ai/models.html) to load. |
| `save_options` | No | All | Boolean. If true, saves recipe options to `recipe_options.json`. Any previously stored value for `model_name` is replaced. |
| `ctx_size` | No | llamacpp, flm, ryzenai-llm | Context size for the model. Overrides the default value. |
| `llamacpp_backend` | No | llamacpp | LlamaCpp backend to use (`vulkan`, `rocm`, `metal` or `cpu`). |
| `llamacpp_args` | No | llamacpp | Custom arguments to pass to llama-server. The following are NOT allowed: `-m`, `--port`, `--ctx-size`, `-ngl`, `--jinja`, `--mmproj`, `--embeddings`, `--reranking`. |
| `whispercpp_backend` | No | whispercpp | WhisperCpp backend: `npu` or `cpu` on Windows; `cpu` or `vulkan` on Linux. Default is `npu` if supported. |
| `whispercpp_args` | No | whispercpp | Custom arguments to pass to whisper-server. The following are NOT allowed: `-m`, `--model`, `--port`. Example: `--convert`. |
| `steps` | No | sd-cpp | Number of inference steps for image generation. Default: 20. |
| `cfg_scale` | No | sd-cpp | Classifier-free guidance scale for image generation. Default: 7.0. |
| `width` | No | sd-cpp | Image width in pixels. Default: 512. |
| `height` | No | sd-cpp | Image height in pixels. Default: 512. |

**Setting Priority:**

When loading a model, settings are applied in this priority order:
1. Values explicitly passed in the `load` request (highest priority)
2. Per-model values configurable in `recipe_options.json` (see below for details)
3. Values from environment variables or server startup arguments (see [Server Configuration](./configuration.md))
4. Default hardcoded values in `lemond` (lowest priority)

#### Per-model options

You can configure recipe-specific options on a per-model basis. Lemonade manages a file called `recipe_options.json` in the user's Lemonade cache (default: `~/.cache/lemonade`). The available options depend on the model's recipe:

```json
{
  "user.Qwen2.5-Coder-1.5B-Instruct": {
    "ctx_size": 16384,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "-np 2 -kvu"
  },
  "Qwen3-Coder-30B-A3B-Instruct-GGUF" : {
    "llamacpp_backend": "rocm"
  },
  "whisper-large-v3-turbo-q8_0.bin": {
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }
}
```

Note that model names include any applicable prefix, such as `user.` and `extra.`.

#### Example requests

Basic load:

```bash
curl -X POST http://localhost:13305/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Load with custom settings:

```bash
curl -X POST http://localhost:13305/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "ctx_size": 8192,
    "llamacpp_backend": "rocm",
    "llamacpp_args": "--flash-attn on --no-mmap"
  }'
```

Load and save settings:

```bash
curl -X POST http://localhost:13305/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "ctx_size": 8192,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "--no-context-shift --no-mmap",
    "save_options": true
  }'
```

Load a Whisper model with NPU backend and conversion enabled:

```bash
curl -X POST http://localhost:13305/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "whisper-large-v3-turbo-q8_0.bin",
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }'
```

Load an image generation model with custom settings:

```bash
curl -X POST http://localhost:13305/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "sd-turbo",
    "steps": 4,
    "cfg_scale": 1.0,
    "width": 512,
    "height": 512
  }'
```

#### Response format

```json
{
  "status":"success",
  "message":"Loaded model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v1/unload` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly unload a model from memory. This is useful to free up memory while still leaving the server process running (which takes minimal resources but a few seconds to start).

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | No | Name of the specific model to unload. If not provided, all loaded models will be unloaded. |

#### Example requests

Unload a specific model:

```bash
curl -X POST http://localhost:13305/api/v1/unload \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Qwen3-0.6B-GGUF"}'
```

Unload all models:

```bash
curl -X POST http://localhost:13305/api/v1/unload
```

#### Response format

Success response:

```json
{
  "status": "success",
  "message": "Model unloaded successfully"
}
```

Error response (model not found):

```json
{
  "status": "error",
  "message": "Model not found: Qwen3-0.6B-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `GET /api/v1/health` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint returns information about loaded models.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:13305/api/v1/health
```

#### Response format

```json
{
  "status": "ok",
  "version":"9.3.3",
  "websocket_port":9000,
  "model_loaded": "Llama-3.2-1B-Instruct-Hybrid",
  "all_models_loaded": [
    {
      "model_name": "Llama-3.2-1B-Instruct-Hybrid",
      "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
      "last_use": 1732123456.789,
      "type": "llm",
      "device": "gpu npu",
      "recipe": "ryzenai-llm",
      "recipe_options": {
        "ctx_size": 4096
      },
      "backend_url": "http://127.0.0.1:8001/v1"
    },
    {
      "model_name": "nomic-embed-text-v1-GGUF",
      "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      "last_use": 1732123450.123,
      "type": "embedding",
      "device": "gpu",
      "recipe": "llamacpp",
      "recipe_options": {
        "ctx_size": 8192,
        "llamacpp_args": "--no-mmap",
        "llamacpp_backend": "rocm"
      },
      "backend_url": "http://127.0.0.1:8002/v1"
    }
  ],
  "max_models": {
    "audio":1,
    "embedding":1,
    "image":1,
    "llm":1,
    "reranking":1,
    "tts":1
  }
}
```

**Field Descriptions:**

- `status` - Server health status, always `"ok"`
- `version` - Version number of Lemonade Server
- `model_loaded` - Model name of the most recently accessed model
- `all_models_loaded` - Array of all currently loaded models with details:
  - `model_name` - Name of the loaded model
  - `checkpoint` - Full checkpoint identifier
  - `last_use` - Unix timestamp of last access (load or inference)
  - `type` - Model type: `"llm"`, `"embedding"`, or `"reranking"`
  - `device` - Space-separated device list: `"cpu"`, `"gpu"`, `"npu"`, or combinations like `"gpu npu"`
  - `backend_url` - URL of the backend server process handling this model (useful for debugging)
  - `recipe`: - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
  - `recipe_options`: - Options used to load the model (e.g., `"ctx_size"`, `"llamacpp_backend"`, `"llamacpp_args"`, `"whispercpp_args"`)
- `max_models` - Maximum number of models that can be loaded simultaneously per type (set via `max_loaded_models` in [Server Configuration](./configuration.md)):
  - `llm` - Maximum LLM/chat models
  - `embedding` - Maximum embedding models
  - `reranking` - Maximum reranking models
  - `audio` - Maximum speech-to-text models
  - `image` - Maximum image models
  - `tts` - Maximum text-to-speech models
- `websocket_port` - *(optional)* Port of the WebSocket server for the [Realtime Audio Transcription API](#realtime-audio-transcription-api-websocket) and [Log Streaming API](#log-streaming-api-websocket). Only present when the WebSocket server is running. The port is OS-assigned or set via `--websocket-port`.

### `GET /api/v1/stats` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:13305/api/v1/stats
```

#### Response format

```json
{
  "time_to_first_token": 2.14,
  "tokens_per_second": 33.33,
  "input_tokens": 128,
  "output_tokens": 5,
  "decode_token_times": [0.01, 0.02, 0.03, 0.04, 0.05],
  "prompt_tokens": 9
}
```

**Field Descriptions:**

- `time_to_first_token` - Time in seconds until the first token was generated
- `tokens_per_second` - Generation speed in tokens per second
- `input_tokens` - Number of tokens processed
- `output_tokens` - Number of tokens generated
- `decode_token_times` - Array of time taken for each generated token
- `prompt_tokens` - Total prompt tokens including cached tokens

### `GET /api/v1/system-info` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

System information endpoint that provides complete hardware details and device enumeration.

#### Example request

```bash
curl "http://localhost:13305/api/v1/system-info"
```

#### Response format

```json
{
  "OS Version": "Windows-10-10.0.26100-SP0",
  "Processor": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
  "Physical Memory": "32.0 GB",
  "OEM System": "ASUS Zenbook S 16",
  "BIOS Version": "1.0.0",
  "CPU Max Clock": "5100 MHz",
  "Windows Power Setting": "Balanced",
  "devices": {
    "cpu": {
      "name": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
      "cores": 12,
      "threads": 24,
      "available": true,
      "family": "x86_64"
    },
    "amd_gpu": [
      {
        "name": "AMD Radeon(TM) 890M Graphics",
        "vram_gb": 0.5,
        "available": true,
        "family": "gfx1150"
      }
    ],
    "amd_npu": {
      "name": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
      "power_mode": "Default",
      "available": true,
      "family": "XDNA2"
    }
  },
  "recipes": {
    "llamacpp": {
      "default_backend": "vulkan",
      "backends": {
        "vulkan": {
          "devices": ["cpu", "amd_gpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "b7869"
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install llamacpp:rocm"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": ""
        },
        "cpu": {
          "devices": ["cpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install llamacpp:cpu"
        }
      }
    },
    "whispercpp": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install whispercpp:default"
        }
      }
    },
    "sd-cpp": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install sd-cpp:default"
        }
      }
    },
    "flm": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["amd_npu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "1.2.0"
        }
      }
    },
    "ryzenai-llm": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["amd_npu"],
          "state": "installed",
          "message": "",
          "action": ""
        }
      }
    }
  }
}
```

**Field Descriptions:**

- **System fields:**
  - `OS Version` - Operating system name and version
  - `Processor` - CPU model name
  - `Physical Memory` - Total RAM
  - `OEM System` - System/laptop model name (Windows only)
  - `BIOS Version` - BIOS information (Windows only)
  - `CPU Max Clock` - Maximum CPU clock speed (Windows only)
  - `Windows Power Setting` - Current power plan (Windows only)

- `devices` - Hardware devices detected on the system (no software/support information)
  - `cpu` - CPU information (name, cores, threads)
  - `amd_gpu` - Array of AMD GPUs, both integrated and discrete (if present)
  - `nvidia_gpu` - Array of NVIDIA GPUs (if present)
  - `amd_npu` - AMD NPU device (if present)

- `recipes` - Software recipes and their backend support status
  - Each recipe (e.g., `llamacpp`, `whispercpp`, `flm`) contains:
    - `default_backend` - Preferred backend selected by server policy for this system (present when at least one backend is not `unsupported`)
    - `backends` - Available backends for this recipe
      - Each backend contains:
        - `devices` - List of devices **on this system** that support this backend (empty if not supported)
        - `state` - Backend lifecycle state: `unsupported`, `installable`, `update_required`, or `installed`
        - `message` - Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`.
        - `action` - Actionable user instruction string. For install/update cases this is typically an exact CLI command; for other states it may be empty or another actionable value (for example, a URL).
        - `version` - Installed or configured backend version (when available)

### `POST /api/v1/install` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Install or update a backend for a specific recipe/backend pair. If the backend is already installed but outdated, this endpoint updates it to the configured version.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `recipe` | Yes | Recipe name (for example, `llamacpp`, `flm`, `whispercpp`, `sd-cpp`, `ryzenai-llm`) |
| `backend` | Yes | Backend name within the recipe (for example, `vulkan`, `rocm`, `cpu`, `default`) |
| `stream` | No | If `true`, returns Server-Sent Events with progress. Defaults to `false`. |
| `force` | No | If `true`, bypasses hardware filtering for `unsupported` backends and attempts installation anyway. Defaults to `false`. |

#### Example request

```bash
curl -X POST http://localhost:13305/api/v1/install \
  -H "Content-Type: application/json" \
  -d '{
    "recipe": "llamacpp",
    "backend": "vulkan",
    "stream": false
  }'
```

#### Response format

```json
{
  "status":"success",
  "recipe":"llamacpp",
  "backend":"vulkan"
}
```

In case of an error, returns an `error` field with details.

### `POST /api/v1/uninstall` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Uninstall a backend for a specific recipe/backend pair. If loaded models are using that backend, they are unloaded first.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `recipe` | Yes | Recipe name |
| `backend` | Yes | Backend name |

#### Example request

```bash
curl -X POST http://localhost:13305/api/v1/uninstall \
  -H "Content-Type: application/json" \
  -d '{
    "recipe": "llamacpp",
    "backend": "vulkan"
  }'
```

#### Response format

```json
{
  "status":"success",
  "recipe":"llamacpp",
  "backend":"vulkan"
}
```

In case of an error, returns an `error` field with details.

# Debugging

To control logging verbosity, use `lemonade config set log_level=debug` (see [Server Configuration](./configuration.md)).

Available levels:

- **critical**: Only critical errors that prevent server operation.
- **error**: Error conditions that might allow continued operation.
- **warning**: Warning conditions that should be addressed.
- **info**: (Default) General informational messages about server operation.
- **debug**: Detailed diagnostic information for troubleshooting, including metrics such as input/output token counts, Time To First Token (TTFT), and Tokens Per Second (TPS).
- **trace**: Very detailed tracing information, including everything from debug level plus all input prompts.

# GGUF Support

The `llama-server` backend works with Lemonade's suggested `*-GGUF` models, as well as any .gguf model from Hugging Face. Windows and Ubuntu Linux are supported. Details:
- Lemonade Server wraps `llama-server` with support for the `lemonade` CLI, client web app, and endpoints (e.g., `models`, `pull`, `load`, etc.).
  - The `chat/completions`, `completions`, `embeddings`, and `reranking` endpoints are supported.
  - The `embeddings` endpoint requires embedding-specific models (e.g., nomic-embed-text models).
  - The `reranking` endpoint requires reranker-specific models (e.g., bge-reranker models).
  - `responses` is not supported at this time.
- A single Lemonade Server process can seamlessly switch between GGUF, ONNX, and FastFlowLM models.
  - Lemonade Server will attempt to load models onto GPU with Vulkan first, and if that doesn't work it will fall back to CPU.
  - From the end-user's perspective, OGA vs. GGUF should be completely transparent: they wont be aware of whether the built-in server or `llama-server` is serving their model.

## Installing GGUF Models

To install an arbitrary GGUF from Hugging Face, open the Lemonade web app by navigating to http://localhost:13305 in your web browser, click the Model Management tab, and use the Add a Model form.

## Platform Support Matrix

| Platform | GPU Acceleration | CPU Architecture |
|----------|------------------|------------------|
| Windows  | ✅ Vulkan, ROCm        | ✅ x64           |
| Ubuntu   | ✅ Vulkan, ROCm        | ✅ x64           |
| Other Linux | ⚠️* Vulkan    | ⚠️* x64          |

*Other Linux distributions may work but are not officially supported.

# FastFlowLM Support

Similar to the [llama-server support](#gguf-support), Lemonade can also route OpenAI API requests to a FastFlowLM `flm serve` backend.

The `flm serve` backend works with Lemonade's suggested `*-FLM` models, as well as any model mentioned in `flm list`. Windows is the only supported operating system. Details:
- Lemonade Server wraps `flm serve` with support for the `lemonade` CLI, client web app, and all Lemonade custom endpoints (e.g., `pull`, `load`, etc.).
  - OpenAI API endpoints supported: `models`, `chat/completions` (streaming), and `embeddings`.
  - The `embeddings` endpoint requires embedding-specific models supported by FLM.
- A single Lemonade Server process can seamlessly switch between FLM, OGA, and GGUF models.

## Installing FLM Models

To install an arbitrary FLM model:
1. `flm list` to view the supported models.
1. Open the Lemonade web app by navigating to http://localhost:13305 in your web browser, click the Model Management tab, and use the Add a Model form.
1. Use the model name from `flm list` as the "checkpoint name" in the Add a Model form and select "flm" as the recipe.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
