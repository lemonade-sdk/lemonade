# OpenAI-Compatible API

This spec defines Lemonade's implementation of the [OpenAI API](https://developers.openai.com/api/docs).

<!-- BEGIN GENERATED: openai.summary -->
| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/chat/completions`](#post-v1chatcompletions) | Chat Completions | messages -> completion |
| `POST` | [`/v1/completions`](#post-v1completions) | Text Completions | prompt -> completion |
| `POST` | [`/v1/responses`](#post-v1responses) | Responses API | prompt/messages -> event |
| `POST` | [`/v1/embeddings`](#post-v1embeddings) | Embeddings | text -> vector representations |
| `POST` | [`/v1/audio/transcriptions`](#post-v1audiotranscriptions) | Audio Transcription | audio file -> text |
| `POST` | [`/v1/audio/speech`](#post-v1audiospeech) | Text to speech | text -> audio |
| `WS` | [`/realtime`](#ws-realtime) | Realtime Audio Transcription, OpenAI SDK compatible | streaming audio -> text |
| `POST` | [`/v1/images/generations`](#post-v1imagesgenerations) | Image Generation | prompt -> image |
| `POST` | [`/v1/images/edits`](#post-v1imagesedits) | Image Editing | image + prompt -> edited image |
| `POST` | [`/v1/images/variations`](#post-v1imagesvariations) | Image Variations | image -> varied image |
| `POST` | [`/v1/images/upscale`](#post-v1imagesupscale) | Image Upscaling | image + ESRGAN model -> upscaled image |
| `GET` | [`/v1/models`](#get-v1models) | List models available locally | n/a |
| `GET` | [`/v1/models/{id}/files`](#get-v1modelsidfiles) | List resolved local file metadata for one model | n/a |
| `GET` | [`/v1/models/{id}/options`](#get-v1modelsidoptions) | Read a model's saved, effective, and default recipe options | n/a |
| `POST` | [`/v1/models/{id}/options`](#post-v1modelsidoptions) | Save recipe options for a model without loading it | n/a |
| `DELETE` | [`/v1/models/{id}/options`](#delete-v1modelsidoptions) | Reset a model's recipe options to defaults | n/a |
| `GET` | [`/v1/models/{id}`](#get-v1modelsid) | Retrieve a specific model by ID | n/a |
<!-- END GENERATED: openai.summary -->

<!-- BEGIN GENERATED: openai.chat_completions -->
## `POST /v1/chat/completions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Chat Completions API. You provide a list of messages and receive a completion. This API will also load the model if it is not already loaded.

Also served at `/api/v0/chat/completions`, `/api/v1/chat/completions` and `/v0/chat/completions`. Requires `LEMONADE_API_KEY` when it is set.

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
| `tools` | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_completion_tokens`. This value is now deprecated by OpenAI in favor of `max_completion_tokens` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_thinking` | No | Lemonade extension: `false` turns off reasoning on models that think. The OpenAI-compatible `thinking: false` is accepted too. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming an Omni collection (`recipe: "collection.omni"`) runs a server-side tool-calling loop instead; see [Server-side tools](#server-side-tools). Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Image understanding input format (OpenAI-compatible)

To send images to `chat/completions`, pass a `messages[*].content` array that mixes `text` and `image_url` items. The image can be provided as a base64 data URL (for example, from `FileReader.readAsDataURL(...)` in web apps).

#### Example request

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

#### Example response

```json
{
  "id": "0",
  "object": "chat.completion",
  "created": 1742927481,
  "model": "Qwen2.5-VL-7B-Instruct",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "The image shows a red apple resting on a wooden table."
    },
    "finish_reason": "stop"
  }]
}
```

### Server-side tools

> **Note:** Omni collection orchestration is a Lemonade-specific extension to the Chat Completions API; it is not part of the OpenAI specification. Requests that target ordinary (non-collection) models are unaffected and are fully OpenAI-compatible.

When `model` names an Omni **collection** model (`recipe: "collection.omni"`, e.g., `LMX-Omni-52B-Halo`), this endpoint runs an internal tool-calling loop instead of a plain completion. The server injects the reference system prompt and tools, routes to the collection's chat component, executes the omni tools (image generation/editing, text-to-speech) against the matching components, and returns one OpenAI-compatible response. Generated media is embedded in the assistant `content`:

- **images** → markdown `![generated image](data:image/png;base64,…)`
- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`

Both non-streaming and `stream: true` are supported. In streaming mode the media arrives as a content delta on a `chat.completion.chunk` frame the moment its tool finishes.

**Merge semantics.** A client-provided system prompt is prepended by the built-in omni system prompt. Client-provided `tools` are merged with the built-in omni tools. The server resolves omni tool calls internally; calls to client-provided tools are returned in a `finish_reason: "tool_calls"` response for the client to execute and resume. Targeting a collection name invokes the server-side loop; targeting a component LLM name bypasses it and returns a plain completion. See [Lemonade Omni Models](../dev/lemonade-omni.md) for details.

#### Example request

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "LMX-Omni-52B-Halo",
        "messages": [{"role": "user", "content": "Draw a red apple on a table."}],
        "stream": false
      }'
```

#### Example response

The image tool runs during the loop and its output is embedded as a markdown image in the assistant `content`:

```json
{
  "id": "0",
  "object": "chat.completion",
  "created": 1742927481,
  "model": "LMX-Omni-52B-Halo",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "Here is a red apple on a table.\n\n![generated image](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAA...)"
    },
    "finish_reason": "stop"
  }]
}
```

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the population of Paris?"}],
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the population of Paris?"}],
          "stream": false
        }'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the population of Paris?"}],
          "stream": true
        }'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the population of Paris?"}],
          "stream": true
        }'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "id": "chatcmpl-uaGB7ivcmQDudT7hrUZQAZLuOunBDFN4",
      "object": "chat.completion",
      "created": 1791943133,
      "model": "Qwen3-0.6B-GGUF",
      "choices": [
        {
          "index": 0,
          "message": {
            "role": "assistant",
            "content": "The population of Paris, France, is approximately **10 million**. This number is consistent with other European cities, though specific details may vary slightly depending on the source.",
            "reasoning_content": "Okay, so I need to figure out the population of Paris. Let me start by recalling what I know. I remember that Paris is a big city in France. I think the population is around 10 million, but wait, I'm not sure. Maybe I should check some sources. Wait, I think I heard something about it being 10 million. But I need to be careful here. Maybe I'm mixing up other cities. For example, in the UK, the population is around 60 million. So Paris, being a European city, should have a population similar to other European cities. But I'm not sure if the number is 10 million. Let me think. Maybe I'm correct. I think the population of Paris is approximately 10 million. But I need to be sure. Alternatively, maybe it's smaller. Wait, I've heard somewhere that the population is 10 million. But I need to verify. Let me think. Oh right, Paris has a population of around 10 million. So that's the answer.\n"
          },
          "finish_reason": "stop"
        }
      ],
      "usage": {
        "prompt_tokens": 15,
        "completion_tokens": 259,
        "total_tokens": 274,
        "prompt_tokens_details": {"cached_tokens": 0}
      },
      "system_fingerprint": "b11393-dbe4c3ed4",
      "timings": {
        "cache_n": 0,
        "predicted_ms": 764.562,
        "predicted_n": 259,
        "predicted_per_second": 337.4481075439271,
        "predicted_per_token_ms": 2.963418604651163,
        "prompt_ms": 11.02,
        "prompt_n": 15,
        "prompt_per_second": 1361.1615245009075,
        "prompt_per_token_ms": 0.7346666666666667
      }
    }
    ```

    **Field Descriptions:**

    - `choices`
      - `message`
        - `reasoning_content` - Reasoning models only: the model's thinking.
        - `tool_calls` - Calls to the request's tools.
    - `x_lemonade_route` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.

=== "Streaming responses"

    `200`

    ```text
    data: {"id": "chatcmpl-nKCifR4mrpaGKh6LqfITk0uzIEousoHd", "object": "chat.completion.chunk", "created": 1791943133, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"role": "assistant", "content": null}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-nKCifR4mrpaGKh6LqfITk0uzIEousoHd", "object": "chat.completion.chunk", "created": 1791943133, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"reasoning_content": "Okay"}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-nKCifR4mrpaGKh6LqfITk0uzIEousoHd", "object": "chat.completion.chunk", "created": 1791943133, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"reasoning_content": ","}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    ...
    data: {"id": "chatcmpl-nKCifR4mrpaGKh6LqfITk0uzIEousoHd", "object": "chat.completion.chunk", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "system_fingerprint": "b11393-dbe4c3ed4", "timings": {"cache_n": 14, "prompt_n": 1, "prompt_ms": 4.309, "prompt_per_token_ms": 4.309, "prompt_per_second": 232.07240659085633, "predicted_n": 323, "predicted_ms": 955.598, "predicted_per_token_ms": 2.967695652173913, "predicted_per_second": 336.9617768140997}}
    data: [DONE]
    ```

    **Field Descriptions:**

    - `usage` - Final chunk only: token usage.
    - `x_lemonade_route` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.
<!-- END GENERATED: openai.chat_completions -->

<!-- BEGIN GENERATED: openai.completions -->
## `POST /v1/completions`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Text Completions API. You provide a prompt and receive a completion. This API will also load the model if it is not already loaded.

Also served at `/api/v0/completions`, `/api/v1/completions` and `/v0/completions`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The prompt to use for the completion. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the completion. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | Echo back the prompt in addition to the completion. Available on non-streaming mode. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Include log probabilities of the output tokens. If true, returns the log probability of each output token. Defaults to false. Only available when `stream=False`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0. 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Integer that controls the number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Float between 0.0 and 1.0 that controls the cumulative probability of top tokens to consider during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "What is the population of Paris?",
          "stream": false,
          "max_tokens": 64
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "What is the population of Paris?",
          "stream": false,
          "max_tokens": 64
        }'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "What is the population of Paris?",
          "stream": true,
          "max_tokens": 64
        }'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "What is the population of Paris?",
          "stream": true,
          "max_tokens": 64
        }'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "id": "chatcmpl-f4ExiyuFYahpItfZ9kgOsndtQcaHrBjz",
      "object": "text_completion",
      "created": 1791943134,
      "model": "Qwen3-0.6B-GGUF",
      "choices": [
        {
          "index": 0,
          "text": " And what does the Parisian metro system include?\n\nThe population of Paris is approximately 2 million people. The Parisian metro system includes the Paris Métro, the Paris Underground, and the Paris TGV, which is a train system.\n\nNow, the question is: What is the population of the city of Paris?\n\n",
          "finish_reason": "length",
          "logprobs": null
        }
      ],
      "usage": {
        "completion_tokens": 64,
        "prompt_tokens": 7,
        "prompt_tokens_details": {"cached_tokens": 0},
        "total_tokens": 71
      },
      "system_fingerprint": "b11393-dbe4c3ed4",
      "timings": {
        "cache_n": 0,
        "predicted_ms": 184.8,
        "predicted_n": 64,
        "predicted_per_second": 340.9090909090909,
        "predicted_per_token_ms": 2.9333333333333336,
        "prompt_ms": 6.219,
        "prompt_n": 7,
        "prompt_per_second": 1125.5828911400547,
        "prompt_per_token_ms": 0.8884285714285715
      }
    }
    ```

    **Field Descriptions:**

    - `x_lemonade_route` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.

=== "Streaming responses"

    `200`

    ```text
    data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "text": " What", "finish_reason": null, "logprobs": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "text": " is", "finish_reason": null, "logprobs": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "text": " the", "finish_reason": null, "logprobs": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    ...
    data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "text": "", "finish_reason": "length", "logprobs": null}], "usage": {"completion_tokens": 64, "prompt_tokens": 7, "total_tokens": 71, "prompt_tokens_details": {"cached_tokens": 6}}, "system_fingerprint": "b11393-dbe4c3ed4", "timings": {"cache_n": 6, "prompt_n": 1, "prompt_ms": 3.04, "prompt_per_token_ms": 3.04, "prompt_per_second": 328.9473684210526, "predicted_n": 64, "predicted_ms": 184.45, "predicted_per_token_ms": 2.9277777777777776, "predicted_per_second": 341.5559772296015}}
    data: [DONE]
    ```

    **Field Descriptions:**

    - `usage` - Final chunk only: token usage.
    - `x_lemonade_route` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.
<!-- END GENERATED: openai.completions -->

<!-- BEGIN GENERATED: openai.responses -->
## `POST /v1/responses`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Responses API. You provide an input and receive a response. This API will also load the model if it is not already loaded.

Also served at `/api/v0/responses`, `/api/v1/responses` and `/v0/responses`. Requires `LEMONADE_API_KEY` when it is set.

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
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Streaming Events

The Responses API uses semantic events for streaming. Each event is typed with a predefined schema, so you can listen for events you care about. Our initial implementation offers support to:

- `response.created`
- `response.output_text.delta`
- `response.completed`

For a full list of event types, see the [API reference for streaming](https://platform.openai.com/docs/api-reference/responses-streaming).

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Llama-3.2-1B-Instruct-GGUF",
          "input": "What is the population of Paris?",
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Llama-3.2-1B-Instruct-GGUF",
          "input": "What is the population of Paris?",
          "stream": false
        }'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Llama-3.2-1B-Instruct-GGUF",
          "input": "What is the population of Paris?",
          "stream": true
        }'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Llama-3.2-1B-Instruct-GGUF",
          "input": "What is the population of Paris?",
          "stream": true
        }'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "id": "resp_UMCsVMSLV0H1cGKewMgTRY02v5PgKvr3",
      "object": "response",
      "created_at": 1791943139,
      "model": "Llama-3.2-1B-Instruct-GGUF",
      "status": "completed",
      "output": [
        {
          "content": [
            {
              "annotations": [],
              "logprobs": [],
              "text": "As of my knowledge cutoff in December 2023, the estimated population of Paris, the capital city of France, is approximately 2.1 million people. However, please note that this information may have changed since my knowledge cutoff date.",
              "type": "output_text"
            }
          ],
          "id": "msg_10jrFYwxMq9LWHeZAWQTw2bwfytTnjmU",
          "role": "assistant",
          "status": "completed",
          "type": "message"
        }
      ],
      "usage": {
        "input_tokens": 42,
        "input_tokens_details": {"cached_tokens": 0},
        "output_tokens": 49,
        "total_tokens": 91
      },
      "completed_at": 1791943139
    }
    ```

    **Field Descriptions:**

    - `output` - Output items; a message item carries its text in content[].text.
    - `x_lemonade_route` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.

=== "Streaming responses"

    `200`

    ```text
    event: response.created
    data: {"type": "response.created", "response": {"id": "resp_8drxh5e9wHyCQgOlZJEKtK6LYBvjZjhQ", "object": "response", "status": "in_progress"}}
    event: response.in_progress
    data: {"type": "response.in_progress", "response": {"id": "resp_8drxh5e9wHyCQgOlZJEKtK6LYBvjZjhQ", "object": "response", "status": "in_progress"}}
    event: response.output_item.added
    data: {"type": "response.output_item.added", "item": {"content": [], "id": "msg_uHVyDCXC7R9egagst1wl8LBcZewNzxgB", "role": "assistant", "status": "in_progress", "type": "message"}}
    ...
    event: response.completed
    data: {"type": "response.completed", "response": {"id": "resp_8drxh5e9wHyCQgOlZJEKtK6LYBvjZjhQ", "object": "response", "created_at": 1791943139, "status": "completed", "model": "~/.cache/huggingface/hub/models--unsloth--Llama-3.2-1B-Instruct-GGUF/snapshots/b69aef112e9f895e6f98d7ae0949f72ff09aa401/Llama-3.2-1B-Instruct-UD-Q4_K_XL.gguf", "output": [{"type": "message", "status": "completed", "id": "msg_uHVyDCXC7R9egagst1wl8LBcZewNzxgB", "content": [{"type": "output_text", "annotations": [], "logprobs": [], "text": "As of the latest available estimates, the population of Paris, the capital city of France, is approximately 2.1 million people within the city limits. However, the larger metropolitan area, which includes the surrounding regions, has a population of over 13.2 million people, according to the City of Paris's official statistics for 2020."}], "role": "assistant"}], "usage": {"input_tokens": 42, "output_tokens": 71, "total_tokens": 113, "input_tokens_details": {"cached_tokens": 41}}}, "timings": {"cache_n": 41, "prompt_n": 1, "prompt_ms": 4.586, "prompt_per_token_ms": 4.586, "prompt_per_second": 218.0549498473615, "predicted_n": 71, "predicted_ms": 310.157, "predicted_per_token_ms": 4.430814285714286, "predicted_per_second": 225.69214945978973}}
    data: [DONE]
    ```

    **Field Descriptions:**

    - `type` - Event type, one of the backend's OpenAI event types such as response.created, response.output_text.delta or response.completed.
    - `response` - response.created and response.completed: the response so far.
    - `delta` - response.output_text.delta: the next text.
<!-- END GENERATED: openai.responses -->

<!-- BEGIN GENERATED: openai.embeddings -->
## `POST /v1/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Embeddings API. You provide input text and receive vector representations (embeddings) that can be used for semantic search, clustering, and similarity comparisons. This API will also load the model if it is not already loaded.

> **Note:** This endpoint is only available for models using the `llamacpp` or `flm` recipes. ONNX models (OGA recipes) do not support embeddings.

Also served at `/api/v0/embeddings`, `/api/v1/embeddings` and `/v0/embeddings`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The input text or array of texts to embed. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for generating embeddings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | The format to return embeddings in. Supported values: `"float"` (default), `"base64"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

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
    curl http://localhost:13305/v1/embeddings \
      -H "Content-Type: application/json" \
      -d '{
          "model": "nomic-embed-text-v1-GGUF",
          "input": ["Hello, world!", "How are you?"],
          "encoding_format": "float"
        }'
    ```

### Response format

`200`

```json
{
  "object": "list",
  "data": [
    {
      "object": "embedding",
      "index": 0,
      "embedding": [0.03513138368725777, 0.0010724958265200257, 0.0043548960238695145, -0.024502824991941452, -0.02874191850423813, 0.029377534985542297, -0.01320538017898798, 0.01702793687582016, ... 760 more]
    },
    {
      "object": "embedding",
      "index": 1,
      "embedding": [-0.024310413748025894, -0.03385873883962631, -0.003393277060240507, 0.0042533064261078835, -0.015559273771941662, 0.026714053004980087, -0.009407692588865757, 0.033160898834466934, ... 760 more]
    }
  ],
  "model": "nomic-embed-text-v1-GGUF",
  "usage": {"prompt_tokens": 12, "total_tokens": 12}
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
<!-- END GENERATED: openai.embeddings -->

<!-- BEGIN GENERATED: openai.audio_transcriptions -->
## `POST /v1/audio/transcriptions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Audio Transcription API. You provide an audio file and receive a text transcription. This API will also load the model if it is not already loaded.

> **Note:** This endpoint uses [whisper.cpp](https://github.com/ggerganov/whisper.cpp) as the backend. Whisper models are automatically downloaded when first used.
>
> **Limitations:** Only `wav` audio input is currently supported. The `response_format` field supports `json`, `verbose_json`, `text`, `srt`, and `vtt`. On the FastFlowLM (FLM) backend, `srt` and `vtt` are rejected with a `400` because FLM returns no segment timestamps, and `verbose_json` returns the compact shape without a `segments` field.

Also served at `/api/v0/audio/transcriptions`, `/api/v1/audio/transcriptions` and `/v0/audio/transcriptions`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `file` | Yes | The audio file to transcribe. Supported formats: wav. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `model` | Yes | The Whisper model to use for transcription (e.g., `Whisper-Tiny`, `Whisper-Base`, `Whisper-Small`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). Defaults to `auto`, which tells whisper.cpp to detect the source language instead of using whisper-server's English default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | The response format. Supported values: `json`, `verbose_json`, `text`, `srt`, `vtt`. `srt` and `vtt` require a backend that reports segment timestamps (whisper.cpp). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Text to guide the transcription's style or continue a previous segment. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/audio/transcriptions `
      -F "model=Whisper-Tiny" `
      -F file=@speech.wav
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/transcriptions \
      -F "model=Whisper-Tiny" \
      -F file=@speech.wav
    ```

=== "PowerShell (text)"

    ```powershell
    curl.exe http://localhost:13305/v1/audio/transcriptions `
      -F "model=Whisper-Tiny" `
      -F "response_format=text" `
      -F file=@speech.wav
    ```

=== "Bash (text)"

    ```bash
    curl http://localhost:13305/v1/audio/transcriptions \
      -F "model=Whisper-Tiny" \
      -F "response_format=text" \
      -F file=@speech.wav
    ```

### Response format

=== "JSON responses"

    `200`

    ```json
    {"text": " Just seeing if this is working.\n"}
    ```

    **Field Descriptions:**

    - `text` - The transcribed text from the audio file
    - `segments` - verbose_json only: timestamped segments.

=== "Text responses"

    `200`

    ```text
     Just seeing if this is working.
    ```
<!-- END GENERATED: openai.audio_transcriptions -->

<!-- BEGIN GENERATED: openai.audio_speech -->
## `POST /v1/audio/speech`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Speech Generation API. You provide a text input and receive an audio file. Which engine serves the request depends on the model.

> **Note:** Supported models are `kokoro-v1` (fixed voices, [Kokoros](https://github.com/lucasjinreal/Kokoros) backend) and the OpenMOSS family — `OpenMOSS-TTS` and `MOSS-TTS-Local` support cloning and integrated voice design. `MOSS-VoiceGen` remains available as a legacy compatibility model while existing GUI/settings paths migrate to the integrated design flow.
>
> **Limitations:** Which `response_format` values are accepted depends on the model's backend: `kokoro-v1` encodes `mp3`, `wav`, `opus`, and `pcm`; OpenMOSS v0.3 encodes buffered `wav` or `pcm`. Native streaming is narrower for both backends and uses `pcm` only, so an explicit non-PCM `response_format` on a streaming request is rejected rather than mislabeled or silently transcoded. OpenMOSS raw PCM is returned as `audio/pcm` with `X-MOSS-Sample-Rate` and `X-MOSS-Channels`, because its native format is model-dependent (24 kHz mono for OpenMOSS-TTS and 48 kHz stereo for MOSS-TTS-Local).

Also served at `/api/v0/audio/speech`, `/api/v1/audio/speech` and `/v0/audio/speech`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The text to speak. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use (e.g., `kokoro-v1`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `speed` | No | Speaking speed. Default: `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice` | No | The voice to use. All OpenAI-defined voices can be used (`alloy`, `ash`, ...), as well as those defined by the kokoro model (`af_sky`, `am_echo`, ...). Default: `shimmer` | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `voice` | No | For OpenMOSS models the field is a free-text voice/style instruction instead of a fixed voice name (e.g. `a calm, deep male narrator voice`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reference_wav_b64` | No | Lemonade extension (OpenMOSS voice cloning): base64-encoded WAV sample of the voice to clone. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice_design_description` | No | Lemonade extension (OpenMOSS voice design): a description of the voice to invent, e.g. `a warm low female voice with a British accent`. Lemonade renders a short sample in that voice and uses it as the reference, so the effect is the same as supplying `reference_wav_b64` yourself. Ignored when `reference_wav_b64` is also present. Design is opt-in through this field only — `voice` never triggers it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Container for the returned audio. Which values are accepted depends on the model's backend (see Limitations above). Default: `mp3` when buffered and `pcm` when streaming, falling back to the backend's first supported format when it cannot encode that default. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream_format` | No | If set, the response is streamed. Only `audio` is supported. This selects the transport only — the container still comes from `response_format`, and an explicit one is honored on both transports. Default: not set | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream` | No | `true` streams the response, like `stream_format: audio`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A request for a model that is not a text-to-speech model answers `400` with `code` `model_not_applicable`, before any model loads.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/speech" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "kokoro-v1",
          "input": "Lemonade can speak!",
          "speed": 1.0,
          "response_format": "mp3"
        }' `
      -OutFile output.mp3
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/speech \
      -H "Content-Type: application/json" \
      -d '{
          "model": "kokoro-v1",
          "input": "Lemonade can speak!",
          "speed": 1.0,
          "response_format": "mp3"
        }' \
      --output output.mp3
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/speech" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "kokoro-v1",
          "input": "Lemonade can speak!",
          "speed": 1.0,
          "stream_format": "audio"
        }' `
      -OutFile output.l16
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/audio/speech \
      -H "Content-Type: application/json" \
      -d '{
          "model": "kokoro-v1",
          "input": "Lemonade can speak!",
          "speed": 1.0,
          "stream_format": "audio"
        }' \
      --output output.l16
    ```

### Response format

=== "Non-streaming responses"

    `200`, `Content-Type: audio/mpeg`, 40,516 bytes

=== "Streaming responses"

    `200`, `Content-Type: audio/l16;rate=24000;endianness=little-endian`, 97,200 bytes
<!-- END GENERATED: openai.audio_speech -->

## `WS /realtime`
<sub>![Status](https://img.shields.io/badge/status-partial-yellow)</sub>

Realtime Audio Transcription API via WebSocket (OpenAI SDK compatible). Stream audio from a microphone and receive transcriptions in real-time with Voice Activity Detection (VAD).

> **Limitations:** Only 16kHz mono PCM16 audio format is supported. Uses the same Whisper models as the HTTP transcription endpoint.

### Connection

WebSocket upgrades are accepted **directly on the main HTTP port** (default 13305) — the same port as all REST endpoints. Connect with the model name:

```
ws://localhost:13305/v1/realtime?model=Whisper-Tiny
```

Accepted paths are `/realtime` and `/logs/stream`, bare or under any of the standard prefixes (`/v1`, `/v0`, `/api/v1`, `/api/v0`), so OpenAI Realtime SDK clients (`/v1/realtime`) connect as-is. When `LEMONADE_API_KEY` is set, pass `?api_key=KEY` as a query parameter.

A dedicated WebSocket port also remains for backward compatibility; it is OS-assigned and reported by the [`/v1/health`](./lemonade.md#get-v1health) endpoint (`websocket_port` field). New clients should prefer the main port:

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

A complete, runnable example:

- **[`realtime_transcription.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/realtime_transcription.py)** - Python CLI for microphone streaming

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
- **Migrating off the dedicated port**: Clients that discover `websocket_port` via `/v1/health` and connect there can switch to `ws://HOST:13305/v1/realtime?model=...` — the protocol (events, audio format, auth) is identical on both ports, so it is a URL change only. This also simplifies remote setups (one port to expose) and works through reverse proxies that pass `Upgrade: websocket`. Keep the `websocket_port` fallback only if you must support servers older than this release.

<!-- BEGIN GENERATED: openai.images_generations -->
## `POST /v1/images/generations`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Image Generation API. You provide a text prompt and receive a generated image.

> **Performance:** CPU inference takes ~4-5 minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/generations`, `/api/v1/images/generations` and `/v0/images/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The text description of the image to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The diffusion model to use (e.g., `SD-Turbo`, `Krea-2-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | The size of the generated image. Format: `WIDTHxHEIGHT` (e.g., `512x512`, `256x256`). Default: `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Currently only `1` is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. Only `b64_json` (base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. SD-Turbo works well with 4 steps. Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. SD-Turbo uses low values (~1.0). Default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. If not specified, a random seed is used. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
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
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/generations \
      -H "Content-Type: application/json" \
      -d '{
          "model": "SD-Turbo",
          "prompt": "A serene mountain landscape at sunset",
          "size": "512x512",
          "steps": 4,
          "response_format": "b64_json"
        }'
    ```

### Response format

`200`

```json
{
  "created": 1791943145,
  "data": [
    {
      "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAgAAAAIACAIAAAB7GkOtAAAEgnRFWHRwYXJhbWV0ZXJzAEEgc2VyZW5l... (886,104 characters)"
    }
  ],
  "output_format": "png"
}
```

**Field Descriptions:**

- `created` - Unix timestamp of when the image was generated.
- `data`
  - `b64_json` - Base64-encoded PNG.
<!-- END GENERATED: openai.images_generations -->

<!-- BEGIN GENERATED: openai.images_edits -->
## `POST /v1/images/edits`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Image Editing API. Provide one or more reference images and a prompt describing the desired change, and receive an edited image.

> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Use editing-capable models such as `Flux-2-Klein-4B` or `SD-Turbo`.
>
> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/edits`, `/api/v1/images/edits` and `/v0/images/edits`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | PNG or JPEG image upload. For multiple references, repeat `image[]` in order. Files are forwarded without re-encoding. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | A text description of the desired edit. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mask` | No | An optional mask image (PNG). White areas indicate regions to edit; black areas are preserved. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Output dimensions as `WIDTHxHEIGHT` (e.g., `512x512`). When omitted, use the first reference image's dimensions, scaling down proportionally to the model's optional `image_defaults.soft_pixel_cap` cap (default: 2048x2048 pixels, ~4 MP). An explicit size bypasses this soft cap; backend limits still apply. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
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

### Edit with the OpenAI Python Client

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

### Example request

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/images/edits `
      -F "model=Flux-2-Klein-4B" `
      -F "prompt=Add a red barn and mountains in the background, photorealistic" `
      -F "size=512x512" `
      -F "n=1" `
      -F "response_format=b64_json" `
      -F image=@image.png
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/edits \
      -F "model=Flux-2-Klein-4B" \
      -F "prompt=Add a red barn and mountains in the background, photorealistic" \
      -F "size=512x512" \
      -F "n=1" \
      -F "response_format=b64_json" \
      -F image=@image.png
    ```

### Response format

`200`

```json
{
  "created": 1791943169,
  "data": [
    {
      "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAgAAAAIACAIAAAB7GkOtAAAFR3RFWHRwYXJhbWV0ZXJzAEFkZCBhIHJl... (689,496 characters)"
    }
  ],
  "output_format": "png"
}
```

**Field Descriptions:**

- `created` - Unix timestamp of when the image was generated.
- `data`
  - `b64_json` - Base64-encoded PNG.
<!-- END GENERATED: openai.images_edits -->

<!-- BEGIN GENERATED: openai.images_variations -->
## `POST /v1/images/variations`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Image Variations API. You provide a source image and receive a variation of it.

> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Unlike `/images/edits`, a `prompt` parameter is not supported and will be ignored — the model generates a variation based solely on the input image.
>
> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/variations`, `/api/v1/images/variations` and `/v0/images/variations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The Stable Diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | The source image file (PNG). Sent as a file in multipart/form-data. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | The size of the output image. Format: `WIDTHxHEIGHT` (e.g., `512x512`). Default: `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of variations to generate. Integer between 1 and 10 inclusive. Default: `1`. Values outside this range result in a 400 Bad Request error. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Format of the response. Only `b64_json` (base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `user` | No | OpenAI API compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Create Variations with the OpenAI Python Client

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

### Example request

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/images/variations `
      -F "model=Flux-2-Klein-4B" `
      -F "size=512x512" `
      -F "n=1" `
      -F "response_format=b64_json" `
      -F image=@image.png
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/variations \
      -F "model=Flux-2-Klein-4B" \
      -F "size=512x512" \
      -F "n=1" \
      -F "response_format=b64_json" \
      -F image=@image.png
    ```

### Response format

`200`

```json
{
  "created": 1791943188,
  "data": [
    {
      "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAgAAAAIACAIAAAB7GkOtAAAE3XRFWHRwYXJhbWV0ZXJzAHZhcmlhdGlv... (204,076 characters)"
    }
  ],
  "output_format": "png"
}
```

**Field Descriptions:**

- `created` - Unix timestamp of when the image was generated.
- `data`
  - `b64_json` - Base64-encoded PNG.
<!-- END GENERATED: openai.images_variations -->

<!-- BEGIN GENERATED: openai.images_upscale -->
## `POST /v1/images/upscale`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Image Upscaling API. You provide a base64-encoded image and a Real-ESRGAN model name, and receive an upscaled image. The upscale factor depends on the selected model, and it is usually reflected in its name.

> **Note:** Unlike `/images/edits` and `/images/variations`, this endpoint accepts a JSON body (not multipart/form-data). The image must be provided as a base64-encoded string.

Also served at `/api/v0/images/upscale`, `/api/v1/images/upscale` and `/v0/images/upscale`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `image` | Yes | Base64-encoded PNG image to upscale. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The ESRGAN model to use (e.g., `RealESRGAN-x4plus`, `Remacri-4x-TheNoise`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The model must carry the `upscaling` label and is downloaded on first use.

A missing `image` or `model` answers `400`, an unknown model `404`, and a failed upscale `500`, each with an `error` object.

### Upscale a Generated Image

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

### Example request

=== "PowerShell"

    ```powershell
    $image = [Convert]::ToBase64String([IO.File]::ReadAllBytes("image.png"))
    $body = @"
    {"image": "$image", "model": "RealESRGAN-x4plus"}
    "@
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/images/upscale" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body $body
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/upscale \
      -H "Content-Type: application/json" \
      -d "{\"image\": \"$(base64 -w0 image.png)\", \"model\": \"RealESRGAN-x4plus\"}"
    ```

### Response format

`200`

```json
{
  "created": 1791943191,
  "data": [
    {
      "b64_json": "iVBORw0KGgoAAAANSUhEUgAABAAAAAQACAIAAADwf7zUAAADZHRFWHRwYXJhbWV0ZXJzAApTdGVwczog... (1,818,596 characters)"
    }
  ]
}
```

**Field Descriptions:**

- `created` - Unix timestamp of when the upscaled image was generated
- `data` - Array containing the upscaled image
  - `b64_json` - Base64-encoded PNG of the upscaled image
<!-- END GENERATED: openai.images_upscale -->

<!-- BEGIN GENERATED: openai.models -->
## `GET /v1/models`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns a list of models available on the server in an OpenAI-compatible format. Each model object includes extended fields like `checkpoint`, `recipe`, `size`, `downloaded`, `labels`, `context_length`, and, when known, `max_context_window`.

By default, only models available locally (downloaded) are shown, matching OpenAI API behavior.

When `lemond` is configured with cloud providers, cloud-routed models appear here alongside local ones with `recipe: "cloud"` and a `cloud_provider` field. They are dot-namespaced by provider (e.g. `fireworks.kimi-k2p5`) and accept the standard chat-completions / completions requests below — see [Cloud Offload](../guide/configuration/cloud.md).

Also served at `/api/v0/models`, `/api/v1/models` and `/v0/models`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `show_all` (query) | No | If set to `true`, returns all models from the catalog including those not yet downloaded. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Model aliases are listed too, each as a copy of its target model with the alias as its `id`.

`HEAD` answers `200` with no body.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models
    ```

### Response format

`200`

```json
{
  "object": "list",
  "data": [
    {
      "id": "Phishing-Email-Detection-ONNX",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "lemonade-sdk/phishing-email-detection-distilbert-ONNX",
      "recipe": "onnxruntime",
      "size": 0.25,
      "downloaded": true,
      "update_available": false,
      "suggested": false,
      "labels": ["classification"],
      "components": [],
      "checkpoints": {"main": "lemonade-sdk/phishing-email-detection-distilbert-ONNX"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "Qwen3-0.6B-GGUF",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "recipe": "llamacpp",
      "size": 0.356,
      "max_context_window": 40960,
      "context_length": 40960,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["chat", "reasoning", "tool-calling"],
      "components": [],
      "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Q4_0"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "Qwen3-0.6B-HRX",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf",
      "recipe": "llamacpp-hrx",
      "size": 0.356,
      "max_context_window": 40960,
      "context_length": 40960,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["chat", "reasoning", "tool-calling"],
      "components": [],
      "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "RealESRGAN-x4plus",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "amd/realesrgan-x4plus:RealESRGAN_x4plus.pth",
      "recipe": "sd-cpp",
      "size": 0.062,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["upscaling", "image"],
      "components": [],
      "checkpoints": {"main": "amd/realesrgan-x4plus:RealESRGAN_x4plus.pth"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "SD-Turbo",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "stabilityai/sd-turbo:sd_turbo.safetensors",
      "recipe": "sd-cpp",
      "size": 4.86,
      "context_length": 22691,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["image"],
      "image_defaults": {"steps": 4, "cfg_scale": 1.0, "width": 512, "height": 512},
      "components": [],
      "checkpoints": {"main": "stabilityai/sd-turbo:sd_turbo.safetensors"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {"cfg_scale": 1.0, "height": 512, "steps": 4, "width": 512}
    },
    {
      "id": "TRELLIS-3D",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "ilintar/trellis2-gguf",
      "recipe": "trellis",
      "size": 30.7,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["3d"],
      "components": [],
      "checkpoints": {"main": "ilintar/trellis2-gguf"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "ThinkSound-SFX",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "ilintar/thinksound-gguf",
      "recipe": "thinksound",
      "size": 6.4,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["audio-generation"],
      "components": [],
      "checkpoints": {"main": "ilintar/thinksound-gguf"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "Whisper-Tiny",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "ggerganov/whisper.cpp:ggml-tiny.bin",
      "recipe": "whispercpp",
      "size": 0.072,
      "context_length": 32768,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["transcription", "realtime-transcription"],
      "components": [],
      "checkpoints": {
        "main": "ggerganov/whisper.cpp:ggml-tiny.bin",
        "npu_cache": "amd/whisper-tiny-onnx-npu:ggml-tiny-encoder-vitisai.rai"
      },
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "jina-reranker-v1-tiny-en-GGUF",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "mradermacher/jina-reranker-v1-tiny-en-GGUF:Q8_0",
      "recipe": "llamacpp",
      "size": 0.034,
      "max_context_window": 8192,
      "context_length": 8192,
      "downloaded": true,
      "update_available": false,
      "suggested": false,
      "labels": ["reranking"],
      "components": [],
      "checkpoints": {"main": "mradermacher/jina-reranker-v1-tiny-en-GGUF:Q8_0"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "kokoro-v1",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "mikkoph/kokoro-onnx",
      "recipe": "kokoro",
      "context_length": 32768,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["tts"],
      "components": [],
      "checkpoints": {"main": "mikkoph/kokoro-onnx"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
    },
    {
      "id": "nomic-embed-text-v1-GGUF",
      "created": 1234567890,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      "recipe": "llamacpp",
      "size": 0.073,
      "max_context_window": 2048,
      "context_length": 8192,
      "downloaded": true,
      "update_available": false,
      "suggested": true,
      "labels": ["embeddings"],
      "components": [],
      "checkpoints": {"main": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S"},
      "source": "huggingface",
      "registry_source": "huggingface",
      "recipe_options": {}
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
  - `max_context_window` - Optional integer indicating the maximum model-supported text context discovered from local static metadata. Currently populated for downloaded GGUF/llama.cpp models and installed FLM text-context models.
  - `context_length` - Number of tokens the model can handle in one request. Uses the loaded value when the model is running, the configured `ctx_size`, or the discovered cloud provider context length otherwise (omitted when neither is known).
  - `max_output_tokens` - Optional integer indicating the maximum output tokens the model can generate in a single completion, when reported by cloud providers.
  - `max_completion_tokens` - Optional integer alias for `max_output_tokens` adhering to OpenAI convention, when reported by cloud providers.
  - `downloaded` - Boolean indicating if the model is downloaded and available locally
  - `update_available` - Boolean indicating a newer commit exists on HuggingFace for this model. Only set for downloaded HF-backed models. `false` otherwise.
  - `suggested` - Boolean indicating if the model is recommended for general use
  - `labels` - Array of tags describing the model's capabilities and characteristics. See [Model Labels](./openai.md#model-labels) for the full list.
  - `image_defaults` - (Image models only) Default generation parameters for the model:
    - `steps` - Number of inference steps (e.g., 4 for turbo models, 20 for standard models)
    - `cfg_scale` - Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)
    - `width` - Default image width in pixels
    - `height` - Default image height in pixels
    - `soft_pixel_cap` - Largest output area, in pixels, that images/edits infers from the first reference image when size is omitted.
  - `components` - (Omni collections only, `recipe: "collection.omni"`) Ordered array of the component model names that make up the collection
  - `models` - (Omni collections only) Ordered array embedding each component's full model object (same shape as the entries in this list), parallel to `components`. This makes a collection's `/v1/models/{id}` response self-contained — exporting it produces a file that can be imported elsewhere via [`/v1/pull`](./lemonade.md#post-v1pull)
  - `checkpoints` - Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.
  - `source` - Where the model came from: a registry name, local_upload, local_path or extra_models_dir.
  - `registry_source` - Remote registry the checkpoint downloads from: huggingface or modelscope.
  - `recipe_options` - Options saved for this model in recipe_options.json.
  - `cloud_provider` - Cloud models only: the provider that serves the model.
  - `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
  - `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
  - `system_prompt` - Omni collections only: the collection's system prompt override.
  - `audio_defaults` - Audio models only: default generation parameters.
  - `version` - Router collections only: the policy document's version.
  - `routing` - Router collections only: the routing policy.
<!-- END GENERATED: openai.models -->

### Model Labels

Labels describe what a model can do. A model may carry multiple labels.

**Deployment labels** — determine which backend endpoint the model is routed to.
Every model names exactly one deployment mode, and a model is never given two
labels that name different modes:

| Label | Endpoint | Description |
|-------|----------|-------------|
| `chat` | `/chat/completions`, `/completions`, `/responses` | Text-generating LLM. This label is what makes a model an LLM — it is not inferred from `reasoning`/`vision`/`tool-calling`/`chat-transcription`, which are characteristics rather than deployment modes. |
| `transcription` | `/audio/transcriptions` | Speech-to-text transcription model (e.g. Whisper). An omni LLM that accepts audio in a chat turn is not one of these — it carries `chat` and the `chat-transcription` capability below. |
| `embeddings` | `/embeddings` | Produces text embedding vectors. Also accepted as `embedding`. |
| `reranking` | `/rerank` | Scores and reranks a list of passages given a query. Also reachable at the aliases `/reranking` and `/reranker`. |
| `image` | `/images/generations`, `/images/edits`, `/images/variations` | Text-to-image generation model. |
| `tts` | `/audio/speech` | Text-to-speech synthesis model. |
| `audio-generation` | `/audio/generations` | Text-to-audio generation model (e.g. music, sound effects). |
| `classification` | `/classify` | Text classification model. Also accepted as `classifier`. |
| `3d` | `/3d/generations` | Text- or image-to-3D mesh generation model. |

When a model declares no deployment label at all, it inherits its recipe's
default — `chat` for `llamacpp`, `flm`, `ryzenai-llm`, `vllm` and `cloud`,
`transcription` for `whispercpp`, `image` for `sd-cpp`, `tts` for `kokoro`, and
so on.

Two label sets describe a model that cannot exist, and are refused rather than
repaired:

- **A mode the recipe's backend does not serve.** `/classify` is served only by
  `onnxruntime`, so `labels: ["classification"]` on a `llamacpp` model is an
  error — register it as the chat model it is.
- **Two different modes.** `labels: ["chat", "embeddings"]` on a `llamacpp` model
  is an error even though llama.cpp serves both: the subprocess is launched for
  one mode, so the second would name an endpoint it was never configured to
  answer. Register one model per mode. The legacy `embedding` and `reranking`
  booleans count as mode claims here, exactly as the labels do.

[`POST /v1/pull`](./lemonade.md#post-v1pull) answers `400` and registers nothing.
An entry already stored in `user_models.json` — written before these rules — is
skipped at startup with an error naming it, and the file is left untouched so it
can be corrected by hand.

**Input-modality labels** — the model accepts additional input types in `/chat/completions`:

| Label | Description |
|-------|-------------|
| `vision` | Accepts image attachments in chat messages. |
| `chat-transcription` | Accepts audio attachments in chat messages and transcribes them as part of its answer (e.g. Qwen2.5-Omni). Like `vision`, this is something a chat model can do, not a deployment mode of its own — a model carrying it also carries `chat`. It is distinct from `transcription`, which deploys a dedicated ASR model on `/audio/transcriptions`. |

**Streaming labels** — capability flags for real-time features:

| Label | Description |
|-------|-------------|
| `realtime-transcription` | Supports the WebSocket `/realtime` endpoint for live microphone transcription. |

**Runtime labels** — affect backend launch defaults:

| Label | Description |
|-------|-------------|
| `mtp` | Enables llama.cpp MTP draft decoding defaults (`--spec-type draft-mtp --spec-draft-n-max 3 --spec-draft-p-min 0.75`); users can override these with `llamacpp_args`. |

**Image capability labels** — carried alongside `image`; they refine what the model is offered for without changing its deployment mode:

| Label | Description |
|-------|-------------|
| `edit` | Tuned for editing an input image (`/images/edits`). Also selects the model for the `edit_image` role in an omni collection. |
| `upscaling` | Image upscaling model (e.g. Real-ESRGAN, `/images/upscale`). Used as a component in image pipelines rather than offered on its own. |

**Characteristic labels** — informational, do not affect routing:

| Label | Description |
|-------|-------------|
| `hot` | Featured or popular model, highlighted in the UI. |
| `reasoning` | Uses extended chain-of-thought reasoning (e.g. DeepSeek, Qwen3). |
| `tool-calling` | Supports function/tool calling in chat completions. |
| `coding` | Tuned for code generation and software tasks. |

<!-- BEGIN GENERATED: openai.models_id_files -->
## `GET /v1/models/{id}/files`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

List resolved local file metadata for a single model. This endpoint is intended for model-detail UIs such as the Files tab. It is per-model inventory, not system or drive storage accounting.

By default, the response does not include absolute filesystem paths. Trusted local clients that need paths for native UI actions can request them explicitly with `?include_paths=true`. Absolute paths may reveal local usernames and cache layout, so clients should only request them when that disclosure is acceptable.

Also served at `/api/v0/models/{id}/files`, `/api/v1/models/{id}/files` and `/v0/models/{id}/files`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `include_paths` (query) | No | `true` adds each file's absolute `path`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown model answers `404` with an `error` object whose `code` is `model_not_found`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files
    ```

### Response format

`200`

```json
{
  "model_id": "Qwen3-0.6B-GGUF",
  "files": [
    {"name": "Qwen3-0.6B-Q4_0.gguf", "role": "main", "size_bytes": 382156480, "exists": true}
  ]
}
```

**Field Descriptions:**

- `model_id` - Public model ID for the requested model.
- `files` - Array of resolved model files known to the registry.
  - `name` - Base filename from the resolved path.
  - `path` - Absolute resolved path on the local system. Only included when `include_paths=true`; privacy-sensitive.
  - `role` - Checkpoint role, for example `main`, `mmproj`, or another recipe-specific role.
  - `size_bytes` - File size in bytes. Directories are summed recursively. Missing files report `0`.
  - `exists` - Whether the resolved path currently exists on disk.
<!-- END GENERATED: openai.models_id_files -->

<!-- BEGIN GENERATED: openai.models_id_options_get -->
## `GET /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Read a model's recipe options, separated by layer, without loading it. With `POST` and `DELETE` on the same path, this manages per-model options independently of [`/v1/load`](./lemonade.md#post-v1load).

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

`effective` is the exact request body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses right now, with every option the recipe accepts resolved through the full priority chain. `defaults` is what a reset model would get.

Per-architecture defaults come from the model's GGUF metadata. For a model that has not been downloaded yet, every key is still present but carries the value it has before those defaults apply.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options
    ```

### Response format

`200`

```json
{
  "model_name": "Qwen3-0.6B-GGUF",
  "recipe": "llamacpp",
  "saved": {},
  "effective": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--temp 0.6 --top-p 0.85 --top-k 20 --min-p 0.0 --repeat-penalty 1.0 --parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-0.6B-GGUF"
  },
  "defaults": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--temp 0.6 --top-p 0.85 --top-k 20 --min-p 0.0 --repeat-penalty 1.0 --parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-0.6B-GGUF"
  },
  "resolved_ctx_size": 40960
}
```

**Field Descriptions:**

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The `/v1/load` body shown above. Posting it back whole to this endpoint saves every resolved value as an override, so send only the options the user changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_get -->

<!-- BEGIN GENERATED: openai.models_id_options_post -->
## `POST /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Save recipe options for a model without loading it. The request body is a flat object of the same recipe options [`/v1/load`](./lemonade.md#post-v1load) accepts. The URL identifies the model; a `model_name` in the body is ignored.

The request merges into the model's saved entry, so keys you don't mention are left alone. `null` removes an option, and the model falls back to the next layer of the [priority chain](./lemonade.md#post-v1load). [`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.

`dry_run: true` validates and resolves the request identically but persists nothing: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk. Use it to preview a change before committing it.

`ctx_size` takes a positive whole number, or `-1` to pin the model to automatic sizing even when the server-wide `ctx_size` is a specific number.

A `400` reports an unrecognized option name, an option from a different recipe, a value of the wrong type, or an invalid `ctx_size`, and nothing from that request is saved.

Saving never loads or reloads the model, so a model that is already running keeps its current options until it is next loaded.

> Note: `pinned` is not settable here and is omitted from `effective` and `defaults`. It belongs to [`/v1/load`](./lemonade.md#post-v1load) and `/internal/pin`.

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | A positive whole number, or `-1` to pin the model to automatic sizing even when the server-wide `ctx_size` is a specific number. Any other option the model's recipe accepts is set the same way. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | `true` validates and resolves the request identically but persists nothing: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | No | Ignored; the URL names the model. Accepted so `effective` can be posted back as is. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/models/Qwen3-4B-GGUF/options" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"ctx_size": 8192}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-4B-GGUF/options \
      -H "Content-Type: application/json" \
      -d '{"ctx_size": 8192}'
    ```

### Response format

`200`

```json
{
  "model_name": "Qwen3-4B-GGUF",
  "recipe": "llamacpp",
  "saved": {"ctx_size": 8192},
  "effective": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": 8192,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-4B-GGUF"
  },
  "defaults": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-4B-GGUF"
  },
  "resolved_ctx_size": 8192
}
```

**Field Descriptions:**

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The `/v1/load` body shown above. Posting it back whole to this endpoint saves every resolved value as an override, so send only the options the user changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_post -->

<!-- BEGIN GENERATED: openai.models_id_options_delete -->
## `DELETE /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reset a model to its defaults by erasing its `recipe_options.json` entry entirely. The model keeps the defaults that come from its registry entry and from the server's global configuration; only the user's saved overrides are removed.

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/models/Qwen3-4B-GGUF/options" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/v1/models/Qwen3-4B-GGUF/options
    ```

### Response format

`200`

```json
{
  "model_name": "Qwen3-4B-GGUF",
  "recipe": "llamacpp",
  "saved": {},
  "effective": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-4B-GGUF"
  },
  "defaults": {
    "auto_evict": null,
    "auto_update": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--parallel 1",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-4B-GGUF"
  },
  "resolved_ctx_size": 32768
}
```

**Field Descriptions:**

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The `/v1/load` body shown above. Posting it back whole to this endpoint saves every resolved value as an override, so send only the options the user changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_delete -->

<!-- BEGIN GENERATED: openai.models_id -->
## `GET /v1/models/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Retrieve a specific model by its ID. Returns the same model object format as the list endpoint above.

Also served at `/api/v0/models/{id}`, `/api/v1/models/{id}` and `/v0/models/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | The ID of the model to retrieve. Must match one of the model IDs from the [models list](https://lemonade-server.ai/models.html) or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown model answers `404` with `code` `model_not_found`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF
    ```

### Response format

`200`

```json
{
  "id": "Qwen3-0.6B-GGUF",
  "created": 1234567890,
  "object": "model",
  "owned_by": "lemonade",
  "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
  "recipe": "llamacpp",
  "size": 0.356,
  "max_context_window": 40960,
  "context_length": 40960,
  "downloaded": true,
  "update_available": false,
  "suggested": true,
  "labels": ["chat", "reasoning", "tool-calling"],
  "components": [],
  "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Q4_0"},
  "source": "huggingface",
  "registry_source": "huggingface",
  "recipe_options": {}
}
```

**Field Descriptions:**

- `id` - Model identifier (used for loading and inference requests)
- `created` - Unix timestamp of when the model entry was created
- `object` - Type of object, always `"model"`
- `owned_by` - Owner of the model, always `"lemonade"`
- `checkpoint` - Full checkpoint identifier on Hugging Face
- `recipe` - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
- `size` - Model size in GB (omitted for models without size information)
- `max_context_window` - Optional integer indicating the maximum model-supported text context discovered from local static metadata. Currently populated for downloaded GGUF/llama.cpp models and installed FLM text-context models.
- `context_length` - Number of tokens the model can handle in one request. Uses the loaded value when the model is running, the configured `ctx_size`, or the discovered cloud provider context length otherwise (omitted when neither is known).
- `max_output_tokens` - Optional integer indicating the maximum output tokens the model can generate in a single completion, when reported by cloud providers.
- `max_completion_tokens` - Optional integer alias for `max_output_tokens` adhering to OpenAI convention, when reported by cloud providers.
- `downloaded` - Boolean indicating if the model is downloaded and available locally
- `update_available` - Boolean indicating a newer commit exists on HuggingFace for this model. Only set for downloaded HF-backed models. `false` otherwise.
- `suggested` - Boolean indicating if the model is recommended for general use
- `labels` - Array of tags describing the model's capabilities and characteristics. See [Model Labels](./openai.md#model-labels) for the full list.
- `image_defaults` - (Image models only) Default generation parameters for the model:
  - `steps` - Number of inference steps (e.g., 4 for turbo models, 20 for standard models)
  - `cfg_scale` - Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)
  - `width` - Default image width in pixels
  - `height` - Default image height in pixels
  - `soft_pixel_cap` - Largest output area, in pixels, that images/edits infers from the first reference image when size is omitted.
- `components` - (Omni collections only, `recipe: "collection.omni"`) Ordered array of the component model names that make up the collection
- `models` - (Omni collections only) Ordered array embedding each component's full model object (same shape as the entries in this list), parallel to `components`. This makes a collection's `/v1/models/{id}` response self-contained — exporting it produces a file that can be imported elsewhere via [`/v1/pull`](./lemonade.md#post-v1pull)
- `checkpoints` - Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.
- `source` - Where the model came from: a registry name, local_upload, local_path or extra_models_dir.
- `registry_source` - Remote registry the checkpoint downloads from: huggingface or modelscope.
- `recipe_options` - Options saved for this model in recipe_options.json.
- `cloud_provider` - Cloud models only: the provider that serves the model.
- `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
- `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
- `system_prompt` - Omni collections only: the collection's system prompt override.
- `audio_defaults` - Audio models only: default generation parameters.
- `version` - Router collections only: the policy document's version.
- `routing` - Router collections only: the routing policy.
<!-- END GENERATED: openai.models_id -->
