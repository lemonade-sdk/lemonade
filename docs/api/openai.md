# OpenAI-Compatible API

This spec defines Lemonade's implementation of the [OpenAI API](https://developers.openai.com/api/docs).

## Summary

| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/chat/completions`](#post-v1-chat-completions) | Chat Completions | messages -> completion |
| `POST` | [`/v1/completions`](#post-v1-completions) | Text Completions | prompt -> completion |
| `POST` | [`/v1/embeddings`](#post-v1-embeddings) | Embeddings | text -> vector representations |
| `POST` | [`/v1/responses`](#post-v1-responses) | Chat Completions | prompt/messages -> event |
| `POST` | [`/v1/audio/transcriptions`](#post-v1-audio-transcriptions) | Audio Transcription | audio file -> text |
| `POST` | [`/v1/audio/speech`](#post-v1-audio-speech) | Text to speech | text -> audio |
| `WS` | [`/realtime`](#ws-realtime) | Realtime Audio Transcription, OpenAI SDK compatible | streaming audio -> text |
| `POST` | [`/v1/images/generations`](#post-v1-images-generations) | Image Generation | prompt -> image |
| `POST` | [`/v1/images/edits`](#post-v1-images-edits) | Image Editing | image + prompt -> edited image |
| `POST` | [`/v1/images/variations`](#post-v1-images-variations) | Image Variations | image -> varied image |
| `POST` | [`/v1/images/upscale`](#post-v1-images-upscale) | Image Upscaling | image + ESRGAN model -> upscaled image |
| `GET` | [`/v1/models`](#get-v1-models) | List models available locally | n/a |
| `GET` | [`/v1/models/{model_id}`](#get-v1-modelsmodel_id) | Retrieve a specific model by ID | n/a |

## `POST /v1/chat/completions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Chat Completions API. You provide a list of messages and receive a completion. This API will also load the model if it is not already loaded.

### Parameters

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

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Qwen3-0.6B-GGUF",
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
    curl -X POST http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Qwen3-0.6B-GGUF",
            "messages": [
              {"role": "user", "content": "What is the population of Paris?"}
            ],
            "stream": false
          }'
    ```

### Image understanding input format (OpenAI-compatible)

To send images to `chat/completions`, pass a `messages[*].content` array that mixes `text` and `image_url` items. The image can be provided as a base64 data URL (for example, from `FileReader.readAsDataURL(...)` in web apps).

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "Qwen2.5-VL-7B-Instruct",
        "messages": [
          {
            "role": "user",
            "content": [
              {"type": "text", "text": "What is in this image?"},
              {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAAAQABAAD..."}}
            ]
          }
        ],
        "stream": false
      }'
```

### Response format

=== "Non-streaming responses"

    ```json
    {
      "id": "0",
      "object": "chat.completion",
      "created": 1742927481,
      "model": "Qwen3-0.6B-GGUF",
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
      "model": "Qwen3-0.6B-GGUF",
      "choices": [{
        "index": 0,
        "delta": {
          "role": "assistant",
          "content": "Paris"
        }
      }]
    }
    ```


## `POST /v1/completions`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Text Completions API. You provide a prompt and receive a completion. This API will also load the model if it is not already loaded.

### Parameters

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

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Qwen3-0.6B-GGUF",
        "prompt": "What is the population of Paris?",
        "stream": false
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/completions \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "What is the population of Paris?",
            "stream": false
          }'
    ```

### Response format

The following format is used for both streaming and non-streaming responses:

```json
{
  "id": "0",
  "object": "text_completion",
  "created": 1742927481,
  "model": "Qwen3-0.6B-GGUF",
  "choices": [{
    "index": 0,
    "text": "Paris has a population of approximately 2.2 million people in the city proper.",
    "finish_reason": "stop"
  }],
}
```



## `POST /v1/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Embeddings API. You provide input text and receive vector representations (embeddings) that can be used for semantic search, clustering, and similarity comparisons. This API will also load the model if it is not already loaded.

> **Note:** This endpoint is only available for models using the `llamacpp` or `flm` recipes. ONNX models (OGA recipes) do not support embeddings.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The input text or array of texts to embed. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for generating embeddings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | The format to return embeddings in. Supported values: `"float"` (default), `"base64"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/embeddings" `
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
    curl -X POST http://localhost:13305/v1/embeddings \
      -H "Content-Type: application/json" \
      -d '{
            "model": "nomic-embed-text-v1-GGUF",
            "input": ["Hello, world!", "How are you?"],
            "encoding_format": "float"
          }'
    ```

### Response format

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



## `POST /v1/reranking`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reranking API. You provide a query and a list of documents, and receive the documents reordered by their relevance to the query with relevance scores. This is useful for improving search results quality. This API will also load the model if it is not already loaded.

> **Note:** This endpoint follows API conventions similar to OpenAI's format but is not part of the official OpenAI API. It is inspired by llama.cpp and other inference server implementations.

> **Note:** This endpoint is only available for models using the `llamacpp` recipe. It is not available for FLM or ONNX models.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` | Yes | The search query text. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Array of document strings to be reranked. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for reranking. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/reranking" `
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
    curl -X POST http://localhost:13305/v1/reranking \
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

### Response format

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



## `POST /v1/responses`
<sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Responses API. You provide an input and receive a response. This API will also load the model if it is not already loaded.

### Parameters

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


### Streaming Events

The Responses API uses semantic events for streaming. Each event is typed with a predefined schema, so you can listen for events you care about. Our initial implementation only offers support to:

- `response.created`
- `response.output_text.delta`
- `response.completed`

For a full list of event types, see the [API reference for streaming](https://platform.openai.com/docs/api-reference/responses-streaming).

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/responses" `
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
    curl -X POST http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Llama-3.2-1B-Instruct-Hybrid",
            "input": "What is the population of Paris?",
            "stream": false
          }'
    ```


### Response format

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



## `POST /v1/audio/transcriptions`
<sub>![Status](https://img.shields.io/badge/status-partial-yellow)</sub>

Audio Transcription API. You provide an audio file and receive a text transcription. This API will also load the model if it is not already loaded.

> **Note:** This endpoint uses [whisper.cpp](https://github.com/ggerganov/whisper.cpp) as the backend. Whisper models are automatically downloaded when first used.
>
> **Limitations:** Only `wav` audio format and `json` response format are currently supported.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `file` | Yes | The audio file to transcribe. Supported formats: wav. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `model` | Yes | The Whisper model to use for transcription (e.g., `Whisper-Tiny`, `Whisper-Base`, `Whisper-Small`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). If not specified, Whisper will auto-detect the language. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | The format of the response. Currently only `json` is supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "Windows"

    ```bash
    curl -X POST http://localhost:13305/v1/audio/transcriptions ^
      -F "file=@C:\path\to\audio.wav" ^
      -F "model=Whisper-Tiny"
    ```

=== "Linux"

    ```bash
    curl -X POST http://localhost:13305/v1/audio/transcriptions \
      -F "file=@/path/to/audio.wav" \
      -F "model=Whisper-Tiny"
    ```

### Response format

```json
{
  "text": "Hello, this is a sample transcription of the audio file."
}
```

**Field Descriptions:**

- `text` - The transcribed text from the audio file



## `WS /realtime`
<sub>![Status](https://img.shields.io/badge/status-partial-yellow)</sub>

Realtime Audio Transcription API via WebSocket (OpenAI SDK compatible). Stream audio from a microphone and receive transcriptions in real-time with Voice Activity Detection (VAD).

> **Limitations:** Only 16kHz mono PCM16 audio format is supported. Uses the same Whisper models as the HTTP transcription endpoint.

### Connection

The WebSocket server runs on a dynamically assigned port. Discover the port via the [`/v1/health`](#get-apiv1health) endpoint (`websocket_port` field), then connect with the model name:

```
ws://localhost:<websocket_port>/realtime?model=Whisper-Tiny
```

Upon connection, the server sends a `session.created` message with a session ID.

### Client → Server Messages

| Message Type | Description |
|--------------|-------------|
| `session.update` | Configure the session (set model, VAD settings, or disable turn detection) |
| `input_audio_buffer.append` | Send audio data (base64-encoded PCM16) |
| `input_audio_buffer.commit` | Force transcription of buffered audio |
| `input_audio_buffer.clear` | Clear audio buffer without transcribing |

### Server → Client Messages

| Message Type | Description |
|--------------|-------------|
| `session.created` | Session established, contains session ID |
| `session.updated` | Session configuration updated |
| `input_audio_buffer.speech_started` | VAD detected speech start |
| `input_audio_buffer.speech_stopped` | VAD detected speech end, transcription triggered |
| `input_audio_buffer.committed` | Audio buffer committed for transcription |
| `input_audio_buffer.cleared` | Audio buffer cleared |
| `conversation.item.input_audio_transcription.delta` | Interim/partial transcription (replaceable) |
| `conversation.item.input_audio_transcription.completed` | Final transcription result |
| `error` | Error message |

### Example: Configure Session

```json
{
  "type": "session.update",
  "session": {
    "model": "Whisper-Tiny"
  }
}
```

### Example: Send Audio

```json
{
  "type": "input_audio_buffer.append",
  "audio": "<base64-encoded PCM16 audio>"
}
```

Audio should be:
- 16kHz sample rate
- Mono (single channel)
- 16-bit signed integer (PCM16)
- Base64 encoded
- Sent in chunks (~85ms recommended)

### Example: Transcription Result

```json
{
  "type": "conversation.item.input_audio_transcription.completed",
  "transcript": "Hello, this is a test transcription."
}
```

### VAD Configuration

VAD settings can be configured via `session.update`:

```json
{
  "type": "session.update",
  "session": {
    "model": "Whisper-Tiny",
    "turn_detection": {
      "threshold": 0.01,
      "silence_duration_ms": 800,
      "prefix_padding_ms": 250
    }
  }
}
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `threshold` | 0.01 | RMS energy threshold for speech detection |
| `silence_duration_ms` | 800 | Silence duration to trigger speech end |
| `prefix_padding_ms` | 250 | Minimum speech duration before triggering |

Set `turn_detection` to `null` to disable server-side VAD and use explicit commits instead:

```json
{
  "type": "session.update",
  "session": {
    "model": "Whisper-Tiny",
    "turn_detection": null
  }
}
```

### Code Examples

See the [`examples/`](../../examples/) directory for a complete, runnable example:

- **[`realtime_transcription.py`](../../examples/realtime_transcription.py)** - Python CLI for microphone streaming

```bash
# Stream from microphone
python examples/realtime_transcription.py --model Whisper-Tiny
```

### Integration Notes

- **Audio Format**: Server expects 16kHz mono PCM16. Higher sample rates must be downsampled client-side.
- **Chunk Size**: Send audio in ~85-256ms chunks for optimal latency/efficiency.
- **VAD Behavior**: Server automatically detects speech boundaries and triggers transcription on speech end.
- **Manual Commit**: Set `turn_detection` to `null`, then use `input_audio_buffer.commit` to force transcription. In this mode the server buffers audio but does not emit VAD or interim transcription events.
- **Clear Buffer**: Use `input_audio_buffer.clear` to discard audio without transcribing.
- **Chunking**: We are still tuning the chunking to balance latency vs. accuracy.


## `POST /v1/images/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Image Generation API. You provide a text prompt and receive a generated image. This API uses [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) as the backend.

> **Note:** Image generation uses Stable Diffusion models. Available models include `SD-Turbo` (fast, ~4 steps), `SDXL-Turbo`, `SD-1.5`, and `SDXL-Base-1.0`.
>
> **Performance:** CPU inference takes ~4-5 minutes per image. GPU (Vulkan) is faster but may have compatibility issues with some hardware.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The text description of the image to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The Stable Diffusion model to use (e.g., `SD-Turbo`, `SDXL-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | The size of the generated image. Format: `WIDTHxHEIGHT` (e.g., `512x512`, `256x256`). Default: `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Currently only `1` is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. Only `b64_json` (base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. SD-Turbo works well with 4 steps. Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. SD-Turbo uses low values (~1.0). Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. If not specified, a random seed is used. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/images/generations \
      -H "Content-Type: application/json" \
      -d '{
            "model": "SD-Turbo",
            "prompt": "A serene mountain landscape at sunset",
            "size": "512x512",
            "steps": 4,
            "response_format": "b64_json"
          }'
    ```

## `POST /v1/images/edits`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Image Editing API. You provide a source image and a text prompt describing the desired change, and receive an edited image. This API uses [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) as the backend.

> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Use editing-capable models such as `Flux-2-Klein-4B` or `SD-Turbo`.
>
> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The Stable Diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | The source image file to edit (PNG). Sent as a file in multipart/form-data. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | A text description of the desired edit. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mask` | No | An optional mask image (PNG). White areas indicate regions to edit; black areas are preserved. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | The size of the output image. Format: `WIDTHxHEIGHT` (e.g., `512x512`). Default: `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Allowed range: `1`–`10`. Default: `1`. Values outside this range are rejected with `400 Bad Request`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. Only `b64_json` (base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `user` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `background` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `quality` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `input_fidelity` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `output_compression` | No | OpenAI API compatibility field. Accepted; silently ignored by the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Example request

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/images/edits \
      -F "model=Flux-2-Klein-4B" \
      -F "prompt=Add a red barn and mountains in the background, photorealistic" \
      -F "size=512x512" \
      -F "n=1" \
      -F "response_format=b64_json" \
      -F "image=@/path/to/source_image.png"
    ```

=== "Python (OpenAI client)"

    ```python
    from openai import OpenAI
    client = OpenAI(base_url="http://localhost:13305/api/v1", api_key="not-needed")
    with open("source_image.png", "rb") as image_file:
        response = client.images.edit(
            model="Flux-2-Klein-4B",
            image=image_file,
            prompt="Add a red barn and mountains in the background, photorealistic",
            size="512x512",
        )
    import base64
    image_data = base64.b64decode(response.data[0].b64_json)
    open("edited_image.png", "wb").write(image_data)
    ```

---

## `POST /v1/images/variations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Image Variations API. You provide a source image and receive a variation of it. This API uses [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) as the backend.

> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Unlike `/images/edits`, a `prompt` parameter is not supported and will be ignored — the model generates a variation based solely on the input image.
>
> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The Stable Diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | The source image file (PNG). Sent as a file in multipart/form-data. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | The size of the output image. Format: `WIDTHxHEIGHT` (e.g., `512x512`). Default: `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of variations to generate. Integer between 1 and 10 inclusive. Default: `1`. Values outside this range result in a 400 Bad Request error. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. Only `b64_json` (base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `user` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Example request

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/images/variations \
      -F "model=Flux-2-Klein-4B" \
      -F "size=512x512" \
      -F "n=1" \
      -F "response_format=b64_json" \
      -F "image=@/path/to/source_image.png"
    ```

=== "Python (OpenAI client)"

    ```python
    from openai import OpenAI
    client = OpenAI(base_url="http://localhost:13305/api/v1", api_key="not-needed")
    with open("source_image.png", "rb") as image_file:
        response = client.images.create_variation(
            model="Flux-2-Klein-4B",
            image=image_file,
            size="512x512",
            n=1,
        )
    import base64
    image_data = base64.b64decode(response.data[0].b64_json)
    open("variation.png", "wb").write(image_data)
    ```

---

## `POST /v1/images/upscale`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Image Upscaling API. You provide a base64-encoded image and a Real-ESRGAN model name, and receive a 4x upscaled image. This API uses the `sd-cli` binary from [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) to perform super-resolution.

> **Note:** Available upscale models are `RealESRGAN-x4plus` (general-purpose, 64 MB) and `RealESRGAN-x4plus-anime` (optimized for anime-style art, 17 MB). Both produce a 4x resolution increase (e.g., 256x256 → 1024x1024).
>
> **Note:** Unlike `/images/edits` and `/images/variations`, this endpoint accepts a JSON body (not multipart/form-data). The image must be provided as a base64-encoded string.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `image` | Yes | Base64-encoded PNG image to upscale. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The ESRGAN model to use (e.g., `RealESRGAN-x4plus`, `RealESRGAN-x4plus-anime`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

A typical workflow is to generate an image first, then upscale it:

=== "Bash"

    ```bash
    # Step 1: Generate an image and save the base64 response
    RESPONSE=$(curl -s -X POST http://localhost:13305/v1/images/generations \
      -H "Content-Type: application/json" \
      -d '{
            "model": "SD-Turbo",
            "prompt": "A serene mountain landscape at sunset",
            "size": "512x512",
            "steps": 4,
            "response_format": "b64_json"
          }')

    # Step 2: Build the upscale JSON payload and pipe it to curl via stdin
    # (base64 images are too large for command-line interpolation)
    echo "$RESPONSE" | python3 -c "
    import sys, json
    b64 = json.load(sys.stdin)['data'][0]['b64_json']
    print(json.dumps({'image': b64, 'model': 'RealESRGAN-x4plus'}))
    " | curl -X POST http://localhost:13305/v1/images/upscale \
      -H "Content-Type: application/json" \
      -d @-
    ```

=== "PowerShell"

    ```powershell
    # Step 1: Generate an image
    $genResponse = Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/images/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "SD-Turbo",
        "prompt": "A serene mountain landscape at sunset",
        "size": "512x512",
        "steps": 4,
        "response_format": "b64_json"
      }'

    # Step 2: Extract the base64 image
    $imageB64 = ($genResponse.Content | ConvertFrom-Json).data[0].b64_json

    # Step 3: Upscale the image with Real-ESRGAN
    $body = @{ image = $imageB64; model = "RealESRGAN-x4plus" } | ConvertTo-Json
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/images/upscale" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body $body
    ```

=== "Python (requests)"

    ```python
    import requests
    import base64

    BASE_URL = "http://localhost:13305/api/v1"

    # Step 1: Generate an image
    gen_response = requests.post(f"{BASE_URL}/images/generations", json={
        "model": "SD-Turbo",
        "prompt": "A serene mountain landscape at sunset",
        "size": "512x512",
        "steps": 4,
        "response_format": "b64_json",
    })
    image_b64 = gen_response.json()["data"][0]["b64_json"]

    # Step 2: Upscale the image with Real-ESRGAN (512x512 -> 2048x2048)
    upscale_response = requests.post(f"{BASE_URL}/images/upscale", json={
        "image": image_b64,
        "model": "RealESRGAN-x4plus",
    })

    # Step 3: Save the upscaled image to a file
    upscaled_b64 = upscale_response.json()["data"][0]["b64_json"]
    with open("upscaled.png", "wb") as f:
        f.write(base64.b64decode(upscaled_b64))
    ```

### Response format

```json
{
  "created": 1742927481,
  "data": [
    {
      "b64_json": "<base64-encoded upscaled PNG>"
    }
  ]
}
```

**Field Descriptions:**

- `created` - Unix timestamp of when the upscaled image was generated
- `data` - Array containing the upscaled image
  - `b64_json` - Base64-encoded PNG of the upscaled image

### Error responses

| Status Code | Condition | Example |
|-------------|-----------|---------|
| 400 | Missing `image` field | `{"error": {"message": "Missing 'image' field (base64 encoded)", "type": "invalid_request_error"}}` |
| 400 | Missing `model` field | `{"error": {"message": "Missing 'model' field", "type": "invalid_request_error"}}` |
| 404 | Unknown model name | `{"error": {"message": "Upscale model not found: bad-model", "type": "invalid_request_error"}}` |
| 500 | Upscale failed | `{"error": {"message": "ESRGAN upscale failed", "type": "server_error"}}` |

---

## `POST /v1/audio/speech`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Speech Generation API. You provide a text input and receive an audio file. This API uses [Kokoros](https://github.com/lucasjinreal/Kokoros) as the backend.

> **Note:** The model to use is called `kokoro-v1`. No other model is supported at the moment.
>
> **Limitations:** Only `mp3`, `wav`, `opus`, and `pcm` are supported. Streaming is supported in `audio` (`pcm`) mode.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The text to speak. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use (e.g., `kokoro-v1`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `speed` | No | Speaking speed. Default: `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice` | No | The voice to use. All OpenAI-defined voices can be used (`alloy`, `ash`, ...), as well as those defined by the kokoro model (`af_sky`, `am_echo`, ...). Default: `shimmer` | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. `mp3`, `wav`, `opus`, and `pcm` are supported. Default: `mp3`| <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream_format` | No | If set, the response will be streamed. Only `audio` is supported, which will output `pcm` audio. Default: not set| <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |

### Example request

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/audio/speech \
      -H "Content-Type: application/json" \
      -d '{
            "model": "kokoro-v1",
            "input": "Lemonade can speak!",
            "speed": 1.0,
            "steps": 4,
            "response_format": "mp3"
          }'
    ```

### Response format

The generated audio file is returned as-is.


## `GET /v1/models`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns a list of models available on the server in an OpenAI-compatible format. Each model object includes extended fields like `checkpoint`, `recipe`, `size`, `downloaded`, and `labels`.

By default, only models available locally (downloaded) are shown, matching OpenAI API behavior.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `show_all` | No | If set to `true`, returns all models from the catalog including those not yet downloaded. Defaults to `false`. |

### Example request

```bash
# Show only downloaded models (OpenAI-compatible)
curl http://localhost:13305/v1/models

# Show all models including not-yet-downloaded (extended usage)
curl http://localhost:13305/v1/models?show_all=true
```

### Response format

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
    },
    {
      "id": "SD-Turbo",
      "created": 1744173590,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "stabilityai/sd-turbo:sd_turbo.safetensors",
      "recipe": "sd-cpp",
      "size": 5.2,
      "downloaded": true,
      "suggested": true,
      "labels": ["image"],
      "image_defaults": {
        "steps": 4,
        "cfg_scale": 1.0,
        "width": 512,
        "height": 512
      }
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
  - `recipe` - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
  - `size` - Model size in GB (omitted for models without size information)
  - `downloaded` - Boolean indicating if the model is downloaded and available locally
  - `suggested` - Boolean indicating if the model is recommended for general use
  - `labels` - Array of tags describing the model (e.g., `"hot"`, `"reasoning"`, `"vision"`, `"embeddings"`, `"reranking"`, `"coding"`, `"tool-calling"`, `"image"`)
  - `image_defaults` - (Image models only) Default generation parameters for the model:
    - `steps` - Number of inference steps (e.g., 4 for turbo models, 20 for standard models)
    - `cfg_scale` - Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)
    - `width` - Default image width in pixels
    - `height` - Default image height in pixels


## `GET /v1/models/{model_id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Retrieve a specific model by its ID. Returns the same model object format as the list endpoint above.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_id` | Yes | The ID of the model to retrieve. Must match one of the model IDs from the [models list](https://lemonade-server.ai/models.html). |

### Example request

```bash
curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF
```

### Response format

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

### Error responses

If the model is not found, the endpoint returns a 404 error:

```json
{
  "error": {
    "message": "Model Qwen3-0.6B-GGUF has not been found",
    "type": "not_found"
  }
}
```
