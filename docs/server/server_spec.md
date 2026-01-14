# Lemonade Server Spec

The Lemonade Server is a standards-compliant server process that provides an HTTP API to enable integration with other applications.

Lemonade Server currently supports these backends:

| Backend                                                                 | Model Format | Description                                                                                                                |
|----------------------------------------------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------------|
| [Llama.cpp](https://github.com/ggml-org/llama.cpp)    | `.GGUF`      | Uses llama.cpp's `llama-server` backend. More details [here](#gguf-support).                    |
| [ONNX Runtime GenAI (OGA)](https://github.com/microsoft/onnxruntime-genai) | `.ONNX`      | Uses Lemonade's own `ryzenai-server` backend.                                                |
| [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM)    | `.q4nx`      | Uses FLM's `flm serve` backend. More details [here](#fastflowlm-support).                    |


## Endpoints Overview

The [key endpoints of the OpenAI API](#openai-compatible-endpoints) are available.

We are also actively investigating and developing [additional endpoints](#lemonade-specific-endpoints) that will improve the experience of local applications.

### OpenAI-Compatible Endpoints
- POST `/api/v1/chat/completions` - Chat Completions (messages -> completion)
- POST `/api/v1/completions` - Text Completions (prompt -> completion)
- POST `/api/v1/embeddings` - Embeddings (text -> vector representations)
- POST `/api/v1/responses` - Chat Completions (prompt|messages -> event)
- POST `/api/v1/audio/transcriptions` - Audio Transcription (audio -> text)
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

- POST `/api/v1/pull` - Install a model
- POST `/api/v1/delete` - Delete a model
- POST `/api/v1/load` - Load a model
- POST `/api/v1/unload` - Unload a model
- GET `/api/v1/health` - Check server health
- GET `/api/v1/stats` - Performance statistics from the last request
- GET `/api/v1/system-info` - System information and device enumeration

## Multi-Model Support

Lemonade Server supports loading multiple models simultaneously, allowing you to keep frequently-used models in memory for faster switching. The server uses a Least Recently Used (LRU) cache policy to automatically manage model eviction when limits are reached.

### Configuration

Use the `--max-loaded-models` option to specify how many models to keep loaded:

```bash
# Load up to 3 LLMs, 2 embedding models, 1 reranking model, and 1 audio model
lemonade-server serve --max-loaded-models 3 2 1 1

# Load up to 5 LLMs (embeddings, reranking, and audio default to 1 each)
lemonade-server serve --max-loaded-models 5
```

**Default:** `1 1 1 1` (one model of each type)

### Model Types

Models are categorized into these types:
- **LLM** - Chat and completion models (default type)
- **Embedding** - Models for generating text embeddings (identified by the `embeddings` label)
- **Reranking** - Models for document reranking (identified by the `reranking` label)
- **Audio** - Models for audio transcription using Whisper (identified by the `audio` label)

Each type has its own independent limit and LRU cache.

### Device Constraints

- **NPU Exclusivity:** Only one model can use the NPU at a time. Loading a new NPU model will evict any existing NPU model regardless of type or limits.
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
2. Values from `lemonade-server` CLI arguments or environment variables
3. Hardcoded defaults in `lemonade-router` (lowest priority)

## Start the HTTP Server

> **NOTE:** This server is intended for use on local systems only. Do not expose the server port to the open internet.

See the [Lemonade Server getting started instructions](./README.md).

```bash
lemonade-server serve
```

## OpenAI-Compatible Endpoints


### `POST /api/v1/chat/completions` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Chat Completions API. You provide a list of messages and receive a completion. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `messages` | Yes | Array of messages in the conversation. Each message should have a `role` ("user" or "assistant") and `content` (the message text). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the completion. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Include log probabilities of the output tokens. If true, returns the log probability of each output token. Defaults to false. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0. 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Integer that controls the number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Float between 0.0 and 1.0 that controls the cumulative probability of top tokens to consider during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools`       | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_completion_tokens`. This value is now deprecated by OpenAI in favor of `max_completion_tokens` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:8000/api/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "messages": [
          {
            "role": "user",
            "content": "What is the population of Paris?"
          }
        ],
        "stream": false
      }'
    ```
=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Llama-3.2-1B-Instruct-Hybrid",
            "messages": [
              {"role": "user", "content": "What is the population of Paris?"}
            ],
            "stream": false
          }'
    ```

#### Response format

=== "Non-streaming responses"

    ```json
    {
      "id": "0",
      "object": "chat.completion",
      "created": 1742927481,
      "model": "Llama-3.2-1B-Instruct-Hybrid",
      "choices": [{
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Paris has a population of approximately 2.2 million people in the city proper."
        },
        "finish_reason": "stop"
      }]
    }
    ```
=== "Streaming responses"
    For streaming responses, the API returns a stream of server-sent events (however, Open AI recommends using their streaming libraries for parsing streaming responses):

    ```json
    {
      "id": "0",
      "object": "chat.completion.chunk",
      "created": 1742927481,
      "model": "Llama-3.2-1B-Instruct-Hybrid",
      "choices": [{
        "index": 0,
        "delta": {
          "role": "assistant",
          "content": "Paris"
        }
      }]
    }
    ```


### `POST /api/v1/completions` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Text Completions API. You provide a prompt and receive a completion. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The prompt to use for the completion.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the completion.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | Echo back the prompt in addition to the completion. Available on non-streaming mode. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Include log probabilities of the output tokens. If true, returns the log probability of each output token. Defaults to false. Only available when `stream=False`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0. 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Integer that controls the number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Float between 0.0 and 1.0 that controls the cumulative probability of top tokens to consider during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion, including input tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:8000/api/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "prompt": "What is the population of Paris?",
        "stream": false
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/completions \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Llama-3.2-1B-Instruct-Hybrid",
            "prompt": "What is the population of Paris?",
            "stream": false
          }'
    ```

#### Response format

The following format is used for both streaming and non-streaming responses:

```json
{
  "id": "0",
  "object": "text_completion",
  "created": 1742927481,
  "model": "Llama-3.2-1B-Instruct-Hybrid",
  "choices": [{
    "index": 0,
    "text": "Paris has a population of approximately 2.2 million people in the city proper.",
    "finish_reason": "stop"
  }],
}
```



### `POST /api/v1/embeddings` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Embeddings API. You provide input text and receive vector representations (embeddings) that can be used for semantic search, clustering, and similarity comparisons. This API will also load the model if it is not already loaded.

> **Note:** This endpoint is only available for models using the `llamacpp` or `flm` recipes. ONNX models (OGA recipes) do not support embeddings.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The input text or array of texts to embed. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for generating embeddings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | The format to return embeddings in. Supported values: `"float"` (default), `"base64"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:8000/api/v1/embeddings" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "nomic-embed-text-v1-GGUF",
        "input": ["Hello, world!", "How are you?"],
        "encoding_format": "float"
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/embeddings \
      -H "Content-Type: application/json" \
      -d '{
            "model": "nomic-embed-text-v1-GGUF",
            "input": ["Hello, world!", "How are you?"],
            "encoding_format": "float"
          }'
    ```

#### Response format

```json
{
  "object": "list",
  "data": [
    {
      "object": "embedding",
      "index": 0,
      "embedding": [0.0234, -0.0567, 0.0891, ...]
    },
    {
      "object": "embedding",
      "index": 1,
      "embedding": [0.0456, -0.0678, 0.1234, ...]
    }
  ],
  "model": "nomic-embed-text-v1-GGUF",
  "usage": {
    "prompt_tokens": 12,
    "total_tokens": 12
  }
}
```

**Field Descriptions:**

- `object` - Type of response object, always `"list"`
- `data` - Array of embedding objects
  - `object` - Type of embedding object, always `"embedding"`
  - `index` - Index position of the input text in the request
  - `embedding` - Vector representation as an array of floats
- `model` - Model identifier used to generate the embeddings
- `usage` - Token usage statistics
  - `prompt_tokens` - Number of tokens in the input
  - `total_tokens` - Total tokens processed



### `POST /api/v1/reranking` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reranking API. You provide a query and a list of documents, and receive the documents reordered by their relevance to the query with relevance scores. This is useful for improving search results quality. This API will also load the model if it is not already loaded.

> **Note:** This endpoint follows API conventions similar to OpenAI's format but is not part of the official OpenAI API. It is inspired by llama.cpp and other inference server implementations.

> **Note:** This endpoint is only available for models using the `llamacpp` recipe. It is not available for FLM or ONNX models.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` | Yes | The search query text. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Array of document strings to be reranked. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for reranking. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:8000/api/v1/reranking" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "bge-reranker-v2-m3-GGUF",
        "query": "What is the capital of France?",
        "documents": [
          "Paris is the capital of France.",
          "Berlin is the capital of Germany.",
          "Madrid is the capital of Spain."
        ]
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/reranking \
      -H "Content-Type: application/json" \
      -d '{
            "model": "bge-reranker-v2-m3-GGUF",
            "query": "What is the capital of France?",
            "documents": [
              "Paris is the capital of France.",
              "Berlin is the capital of Germany.",
              "Madrid is the capital of Spain."
            ]
          }'
    ```

#### Response format

```json
{
  "model": "bge-reranker-v2-m3-GGUF",
  "object": "list",
  "results": [
    {
      "index": 0,
      "relevance_score": 8.60673713684082
    },
    {
      "index": 1,
      "relevance_score": -5.3886260986328125
    },
    {
      "index": 2,
      "relevance_score": -3.555561065673828
    }
  ],
  "usage": {
    "prompt_tokens": 51,
    "total_tokens": 51
  }
}
```

**Field Descriptions:**

- `model` - Model identifier used for reranking
- `object` - Type of response object, always `"list"`
- `results` - Array of all documents with relevance scores
  - `index` - Original index of the document in the input array
  - `relevance_score` - Relevance score assigned by the model (higher = more relevant)
- `usage` - Token usage statistics
  - `prompt_tokens` - Number of tokens in the input
  - `total_tokens` - Total tokens processed

> **Note:** The results are returned in their original input order, not sorted by relevance score. To get documents ranked by relevance, you need to sort the results by `relevance_score` in descending order on the client side.



### `POST /api/v1/responses` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Responses API. You provide an input and receive a response. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | A list of dictionaries or a string input for the model to respond to. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | The maximum number of output tokens to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0. 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Integer that controls the number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Float between 0.0 and 1.0 that controls the cumulative probability of top tokens to consider during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |


#### Streaming Events

The Responses API uses semantic events for streaming. Each event is typed with a predefined schema, so you can listen for events you care about. Our initial implementation only offers support to:

- `response.created`
- `response.output_text.delta`
- `response.completed`

For a full list of event types, see the [API reference for streaming](https://platform.openai.com/docs/api-reference/responses-streaming).

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:8000/api/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "input": "What is the population of Paris?",
        "stream": false
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Llama-3.2-1B-Instruct-Hybrid",
            "input": "What is the population of Paris?",
            "stream": false
          }'
    ```


#### Response format

=== "Non-streaming responses"

    ```json
    {
      "id": "0",
      "created_at": 1746225832.0,
      "model": "Llama-3.2-1B-Instruct-Hybrid",
      "object": "response",
      "output": [{
        "id": "0",
        "content": [{
          "annotations": [],
          "text": "Paris has a population of approximately 2.2 million people in the city proper."
        }]
      }]
    }
    ```

=== "Streaming Responses"
    For streaming responses, the API returns a series of events. Refer to [OpenAI streaming guide](https://platform.openai.com/docs/guides/streaming-responses?api-mode=responses) for details.



### `POST /api/v1/audio/transcriptions` <sub>![Status](https://img.shields.io/badge/status-partial-yellow)</sub>

Audio Transcription API. You provide an audio file and receive a text transcription. This API will also load the model if it is not already loaded.

> **Note:** This endpoint uses [whisper.cpp](https://github.com/ggerganov/whisper.cpp) as the backend. Whisper models are automatically downloaded when first used.
>
> **Limitations:** Only `wav` audio format and `json` response format are currently supported.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `file` | Yes | The audio file to transcribe. Supported formats: wav. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `model` | Yes | The Whisper model to use for transcription (e.g., `Whisper-Tiny`, `Whisper-Base`, `Whisper-Small`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). If not specified, Whisper will auto-detect the language. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | The format of the response. Currently only `json` is supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "Windows"

    ```bash
    curl -X POST http://localhost:8000/api/v1/audio/transcriptions ^
      -F "file=@C:\path\to\audio.wav" ^
      -F "model=Whisper-Tiny"
    ```

=== "Linux"

    ```bash
    curl -X POST http://localhost:8000/api/v1/audio/transcriptions \
      -F "file=@/path/to/audio.wav" \
      -F "model=Whisper-Tiny"
    ```

#### Response format

```json
{
  "text": "Hello, this is a sample transcription of the audio file."
}
```

**Field Descriptions:**

- `text` - The transcribed text from the audio file



### `GET /api/v1/models` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns a list of models available on the server in an OpenAI-compatible format. Each model object includes extended fields like `checkpoint`, `recipe`, `size`, `downloaded`, and `labels`.

By default, only models available locally (downloaded) are shown, matching OpenAI API behavior.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `show_all` | No | If set to `true`, returns all models from the catalog including those not yet downloaded. Defaults to `false`. |

#### Example request

```bash
# Show only downloaded models (OpenAI-compatible)
curl http://localhost:8000/api/v1/models

# Show all models including not-yet-downloaded (extended usage)
curl http://localhost:8000/api/v1/models?show_all=true
```

#### Response format

```json
{
  "object": "list",
  "data": [
    {
      "id": "Qwen3-0.6B-GGUF",
      "created": 1744173590,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "recipe": "llamacpp",
      "size": 0.38,
      "downloaded": true,
      "suggested": true,
      "labels": ["reasoning"]
    },
    {
      "id": "Gemma-3-4b-it-GGUF",
      "created": 1744173590,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
      "recipe": "llamacpp",
      "size": 3.61,
      "downloaded": true,
      "suggested": true,
      "labels": ["hot", "vision"]
    }
  ]
}
```

**Field Descriptions:**

- `object` - Type of response object, always `"list"`
- `data` - Array of model objects with the following fields:
  - `id` - Model identifier (used for loading and inference requests)
  - `created` - Unix timestamp of when the model entry was created
  - `object` - Type of object, always `"model"`
  - `owned_by` - Owner of the model, always `"lemonade"`
  - `checkpoint` - Full checkpoint identifier on Hugging Face
  - `recipe` - Backend/device recipe used to load the model (e.g., `"oga-cpu"`, `"oga-hybrid"`, `"llamacpp"`, `"flm"`)
  - `size` - Model size in GB (omitted for models without size information)
  - `downloaded` - Boolean indicating if the model is downloaded and available locally
  - `suggested` - Boolean indicating if the model is recommended for general use
  - `labels` - Array of tags describing the model (e.g., `"hot"`, `"reasoning"`, `"vision"`, `"embeddings"`, `"reranking"`, `"coding"`, `"tool-calling"`)


### `GET /api/v1/models/{model_id}` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Retrieve a specific model by its ID. Returns the same model object format as the list endpoint above.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_id` | Yes | The ID of the model to retrieve. Must match one of the model IDs from the [models list](./server_models.md). |

#### Example request

```bash
curl http://localhost:8000/api/v1/models/Qwen3-0.6B-GGUF
```

#### Response format

Returns a single model object with the same fields as described in the [models list endpoint](#get-apiv1models) above.

```json
{
  "id": "Qwen3-0.6B-GGUF",
  "created": 1744173590,
  "object": "model",
  "owned_by": "lemonade",
  "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
  "recipe": "llamacpp",
  "size": 0.38,
  "downloaded": true,
  "suggested": true,
  "labels": ["reasoning"],
  "recipe_options" {
    "ctx_size": 8192,
    "llamacpp_args": "--no-mmap",
    "llamacpp_backend": "rocm"
  }
}
```

#### Error responses

If the model is not found, the endpoint returns a 404 error:

```json
{
  "error": {
    "message": "Model Llama-3.2-1B-Instruct-Hybrid has not been found",
    "type": "not_found"
  }
}
```

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
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to install. |

Example request:

```bash
curl -X POST http://localhost:8000/api/v1/pull \
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
| `model_name` | Yes | Namespaced [Lemonade Server model name](./server_models.md) to register and install. |
| `checkpoint` | Yes | HuggingFace checkpoint to install. |
| `recipe` | Yes | Lemonade API recipe to load the model with. |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). Adds 'reasoning' label. |
| `vision` | No | Whether the model has vision capabilities for processing images (default: false). Adds 'vision' label. |
| `embedding` | No | Whether the model is an embedding model (default: false). Adds 'embeddings' label. |
| `reranking` | No | Whether the model is a reranking model (default: false). Adds 'reranking' label. |
| `mmproj` | No | Multimodal Projector (mmproj) file to use for vision models. |

Example request:

```bash
curl -X POST http://localhost:8000/api/v1/pull \
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

### `POST /api/v1/delete` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Delete a model by removing it from local storage. If the model is currently loaded, it will be unloaded first.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to delete. |

Example request:

```bash
curl -X POST http://localhost:8000/api/v1/delete \
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

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to load. |
| `ctx_size` | No | Context size for the model. Overrides the default value for this model. |
| `llamacpp_backend` | No | LlamaCpp backend to use (`vulkan`, `rocm`, `metal` or `cpu`). Only applies to llamacpp models. |
| `llamacpp_args` | No | Custom arguments to pass to llama-server. The following are NOT allowed: `-m`, `--port`, `--ctx-size`, `-ngl`. |
| `save_options` | No | Boolean. If true, saves `ctx_size`, `llamacpp_backend` and `llamacpp_args` to the `recipe_options.json` file. Any previously stored value for `model_name` is replaced with the parameters of this request. |

**Setting Priority:**

When loading a model, settings are applied in this priority order:
1. Values explicitly passed in the `load` request (highest priority)
2. Per-model values configurable in `recipe_options.json` (see below for details)
3. Values set via `lemonade-server` CLI arguments or environment variables
4. Default hardcoded values in `lemonade-router` (lowest priority)

#### Per-model options

You can configure a default `ctx_size`, `llamacpp_backend` and `llamacpp_args` on a per-model basis. To achieve this, Lemonade manages a file called `recipe_options.json` in the user's Lemonade cache (default: `~/.cache/lemonade`). An example `recipe_options.json` file follows:

```json
{
  "user.Qwen2.5-Coder-1.5B-Instruct": {
    "ctx_size": 16384,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "-np 2 -kvu"
  },
  "Qwen3-Coder-30B-A3B-Instruct-GGUF" : {
    "llamacpp_backend": "rocm"
  }
}
```

Note that model names include any applicable prefix, such as `user.` and `extra.`.

#### Example requests

Basic load:

```bash
curl -X POST http://localhost:8000/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Load with custom settings:

```bash
curl -X POST http://localhost:8000/api/v1/load \
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
curl -X POST http://localhost:8000/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "ctx_size": 8192,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "--no-context-shift --no-mmap",
    "save_options": true
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
curl -X POST http://localhost:8000/api/v1/unload \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Llama-3.2-1B-Instruct-Hybrid"}'
```

Unload all models:

```bash
curl -X POST http://localhost:8000/api/v1/unload
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
  "message": "Model not found: Llama-3.2-1B-Instruct-Hybrid"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `GET /api/v1/health` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint returns information about loaded models.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/health
```

#### Response format

```json
{
  "status": "ok",
  "checkpoint_loaded": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
  "model_loaded": "Llama-3.2-1B-Instruct-Hybrid",
  "all_models_loaded": [
    {
      "model_name": "Llama-3.2-1B-Instruct-Hybrid",
      "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
      "last_use": 1732123456.789,
      "type": "llm",
      "device": "gpu npu",
      "backend_url": "http://127.0.0.1:8001/v1"
    },
    {
      "model_name": "nomic-embed-text-v1-GGUF",
      "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      "last_use": 1732123450.123,
      "type": "embedding",
      "device": "gpu",
      "backend_url": "http://127.0.0.1:8002/v1"
    }
  ],
  "max_models": {
    "llm": 3,
    "embedding": 1,
    "reranking": 1
  }
}
```

**Field Descriptions:**

- `status` - Server health status, always `"ok"`
- `checkpoint_loaded` - Checkpoint identifier of the most recently accessed model
- `model_loaded` - Model name of the most recently accessed model
- `all_models_loaded` - Array of all currently loaded models with details:
  - `model_name` - Name of the loaded model
  - `checkpoint` - Full checkpoint identifier
  - `last_use` - Unix timestamp of last access (load or inference)
  - `type` - Model type: `"llm"`, `"embedding"`, or `"reranking"`
  - `device` - Space-separated device list: `"cpu"`, `"gpu"`, `"npu"`, or combinations like `"gpu npu"`
  - `backend_url` - URL of the backend server process handling this model (useful for debugging)
- `max_models` - Maximum number of models that can be loaded simultaneously (set via `--max-loaded-models`):
  - `llm` - Maximum LLM/chat models
  - `embedding` - Maximum embedding models
  - `reranking` - Maximum reranking models

### `GET /api/v1/stats` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/stats
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

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `verbose` | No | Include detailed system information. When `false` (default), returns essential information (OS, processor, memory, devices). When `true`, includes additional details like Python packages and extended system information. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

#### Example request

=== "Basic system information"

    ```bash
    curl "http://localhost:8000/api/v1/system-info"
    ```

=== "Detailed system information"

    ```bash
    curl "http://localhost:8000/api/v1/system-info?verbose=true"
    ```

#### Response format

=== "Basic response (verbose=false)"

    ```json
    {
      "OS Version": "Windows-10-10.0.26100-SP0",
      "Processor": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
      "Physical Memory": "32.0 GB",
      "devices": {
        "cpu": {
          "name": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
          "cores": 12,
          "threads": 24,
          "available": true
        },
        "amd_igpu": {
          "name": "AMD Radeon(TM) 890M Graphics",
          "memory_mb": 512,
          "driver_version": 32.0.12010.10001,
          "available": true
        },
        "amd_dgpu": [],
        "npu": {
          "name": "AMD NPU",
          "driver_version": "32.0.203.257",
          "power_mode": "Default",
          "available": true
        }
      }
    }
    ```

# Debugging

To help debug the Lemonade server, you can use the `--log-level` parameter to control the verbosity of logging information. The server supports multiple logging levels that provide increasing amounts of detail about server operations.

```
lemonade-server serve --log-level [level]
```

Where `[level]` can be one of:

- **critical**: Only critical errors that prevent server operation.
- **error**: Error conditions that might allow continued operation.
- **warning**: Warning conditions that should be addressed.
- **info**: (Default) General informational messages about server operation.
- **debug**: Detailed diagnostic information for troubleshooting, including metrics such as input/output token counts, Time To First Token (TTFT), and Tokens Per Second (TPS).
- **trace**: Very detailed tracing information, including everything from debug level plus all input prompts.

# GGUF Support

The `llama-server` backend works with Lemonade's suggested `*-GGUF` models, as well as any .gguf model from Hugging Face. Windows and Ubuntu Linux are supported. Details:
- Lemonade Server wraps `llama-server` with support for the `lemonade-server` CLI, client web app, and endpoints (e.g., `models`, `pull`, `load`, etc.).
  - The `chat/completions`, `completions`, `embeddings`, and `reranking` endpoints are supported.
  - The `embeddings` endpoint requires embedding-specific models (e.g., nomic-embed-text models).
  - The `reranking` endpoint requires reranker-specific models (e.g., bge-reranker models).
  - `responses` is not supported at this time.
- A single Lemonade Server process can seamlessly switch between GGUF, ONNX, and FastFlowLM models.
  - Lemonade Server will attempt to load models onto GPU with Vulkan first, and if that doesn't work it will fall back to CPU.
  - From the end-user's perspective, OGA vs. GGUF should be completely transparent: they wont be aware of whether the built-in server or `llama-server` is serving their model.

## Installing GGUF Models

To install an arbitrary GGUF from Hugging Face, open the Lemonade web app by navigating to http://localhost:8000 in your web browser, click the Model Management tab, and use the Add a Model form.

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
- Lemonade Server wraps `flm serve` with support for the `lemonade-server` CLI, client web app, and all Lemonade custom endpoints (e.g., `pull`, `load`, etc.).
  - OpenAI API endpoints supported: `models`, `chat/completions` (streaming), and `embeddings`.
  - The `embeddings` endpoint requires embedding-specific models supported by FLM.
- A single Lemonade Server process can seamlessly switch between FLM, OGA, and GGUF models.

## Installing FLM Models

To install an arbitrary FLM model:
1. `flm list` to view the supported models.
1. Open the Lemonade web app by navigating to http://localhost:8000 in your web browser, click the Model Management tab, and use the Add a Model form.
1. Use the model name from `flm list` as the "checkpoint name" in the Add a Model form and select "flm" as the recipe.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
