# OpenAI-Compatible API

This page documents Lemonade's implementation of the [OpenAI API](https://developers.openai.com/api/docs).

<!-- BEGIN GENERATED: openai.summary -->
| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/chat/completions`](#post-v1chatcompletions) | Generate the next message in a conversation | messages -> completion |
| `POST` | [`/v1/completions`](#post-v1completions) | Continue a text prompt | prompt -> completion |
| `POST` | [`/v1/embeddings`](#post-v1embeddings) | Embed text as vectors | text -> vector representations |
| `POST` | [`/v1/responses`](#post-v1responses) | Generate a response in OpenAI's Responses format | prompt/messages -> event |
| `POST` | [`/v1/audio/transcriptions`](#post-v1audiotranscriptions) | Transcribe an audio file to text | audio file -> text |
| `POST` | [`/v1/images/generations`](#post-v1imagesgenerations) | Generate an image from a text prompt | prompt -> image |
| `POST` | [`/v1/images/edits`](#post-v1imagesedits) | Edit images as a prompt describes | image + prompt -> edited image |
| `POST` | [`/v1/images/variations`](#post-v1imagesvariations) | Generate a variation of an image | image -> varied image |
| `POST` | [`/v1/images/upscale`](#post-v1imagesupscale) | Upscale an image | image + ESRGAN model -> upscaled image |
| `POST` | [`/v1/audio/speech`](#post-v1audiospeech) | Generate speech from text | text -> audio |
| `WS` | [`/realtime`](#ws-realtime) | Realtime Audio Transcription, OpenAI SDK compatible | streaming audio -> text |
| `GET` | [`/v1/models`](#get-v1models) | List the server's models | n/a |
| `GET` | [`/v1/models/{id}`](#get-v1modelsid) | Read one model | n/a |
| `GET` | [`/v1/models/{id}/files`](#get-v1modelsidfiles) | List one model's local files | n/a |
| `GET` | [`/v1/models/{id}/options`](#get-v1modelsidoptions) | Read a model's recipe options by layer | n/a |
| `POST` | [`/v1/models/{id}/options`](#post-v1modelsidoptions) | Save a model's recipe options | n/a |
| `DELETE` | [`/v1/models/{id}/options`](#delete-v1modelsidoptions) | Reset a model's recipe options | n/a |
<!-- END GENERATED: openai.summary -->

<!-- BEGIN GENERATED: openai.chat_completions -->
## `POST /v1/chat/completions`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates the next assistant message for a conversation, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `messages` | Yes | The conversation so far, in OpenAI's message format: each message has a `role` and `content`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends tokens as server-sent events as they are generated; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Sequences that stop generation, as a string or an array of strings. The returned text leaves them out. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | `true` returns the log probability of each output token. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. Wins over `max_completion_tokens` when both are sent. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | OpenAI's name for `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_thinking` | No | Lemonade extension: `false` turns off reasoning on models that think. `thinking: false` does the same, and `enable_thinking` wins when both are sent. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Image Input

To send images to `chat/completions`, pass a `messages[*].content` array that mixes `text` and `image_url` items. The image can be provided as a base64 data URL (for example, from `FileReader.readAsDataURL(...)` in web apps).

#### Example request

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "Qwen2.5-VL-7B-Instruct-GGUF",
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
  "model": "Qwen2.5-VL-7B-Instruct-GGUF",
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

### Server-Side Tools

Omni collection orchestration is a Lemonade extension to the Chat Completions API; requests to other models are unaffected.

When `model` names an Omni **collection** model (`recipe: "collection.omni"`, e.g., `LMX-Omni-52B-Halo`), the route runs an internal tool-calling loop instead of a plain completion. The server injects the reference system prompt and tools, routes to the collection's chat component, executes the omni tools (image generation/editing, text-to-speech) against the matching components, and returns one OpenAI-compatible response. Generated media is embedded in the assistant `content`:

- **images** → markdown `![generated image](data:image/png;base64,…)`
- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`

Both non-streaming and `stream: true` are supported. In streaming mode the media arrives as a content delta on a `chat.completion.chunk` frame the moment its tool finishes.

**Merge semantics.** The built-in omni system prompt goes before a client-provided one. Client-provided `tools` are merged with the built-in omni tools. The server resolves omni tool calls internally; calls to client-provided tools are returned in a `finish_reason: "tool_calls"` response for the client to execute and resume. Targeting a collection name invokes the server-side loop; targeting a component LLM name bypasses it and returns a plain completion. See [Lemonade Omni Models](../dev/lemonade-omni.md) for details.

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
      "id": "chatcmpl-Ur8WXBSFmCy4xSW1gr9J6imPMcSBsmG7",
      "object": "chat.completion",
      "created": 1791990411,
      "model": "Qwen3-0.6B-GGUF",
      "choices": [
        {
          "index": 0,
          "message": {
            "role": "assistant",
            "content": "The population of Paris, France, is approximately **2,015,000**. This number reflects the city's size and population, making it one of the most populous cities in the world.",
            "reasoning_content": "Okay, so I need to figure out the population of Paris. Let me think. First, I remember that Paris is a big city in France, right? I think it's the capital. But how do I find the exact population?\n\nWait, I think I heard that the population is around 2 million. But I'm not sure. Maybe I should check a reliable source. Oh, right! The official website of Paris might have the population numbers. Let me recall... Oh! Oh, right! The city's population is about 2,015,000. I think that's correct. But I'm a bit fuzzy on the exact number. Maybe I should confirm with another source. Wait, I'm not sure. Hmm. Let me think... Oh, yeah, I think it's 2,015,000. But sometimes people might say 2 million. But 2,015,000 is more accurate. So I'll go with that.\n"
          },
          "finish_reason": "stop"
        }
      ],
      "usage": {
        "prompt_tokens": 15,
        "completion_tokens": 255,
        "total_tokens": 270,
        "prompt_tokens_details": {"cached_tokens": 0}
      },
      "system_fingerprint": "b11393-dbe4c3ed4",
      "timings": {
        "cache_n": 0,
        "predicted_ms": 756.876,
        "predicted_n": 255,
        "predicted_per_second": 335.5899777506487,
        "predicted_per_token_ms": 2.9798267716535434,
        "prompt_ms": 9.652,
        "prompt_n": 15,
        "prompt_per_second": 1554.0820555325322,
        "prompt_per_token_ms": 0.6434666666666666
      }
    }
    ```

    **Field Descriptions:**

    - `choices`
      - `message`
        - `reasoning_content` - Reasoning models only: the model's thinking.
        - `tool_calls` - Calls to the request's tools.
    - `x_lemonade_route` - The routing decision for a request that named a `collection.router` model, present when the request sets `route_trace`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed to `default_model`.

=== "Streaming responses"

    `200`

    ```text
    data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"role": "assistant", "content": null}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"reasoning_content": "Okay"}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {"reasoning_content": ","}, "finish_reason": null}], "system_fingerprint": "b11393-dbe4c3ed4"}
    ...
    data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990412, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "system_fingerprint": "b11393-dbe4c3ed4", "timings": {"cache_n": 14, "prompt_n": 1, "prompt_ms": 4.081, "prompt_per_token_ms": 4.081, "prompt_per_second": 245.03798088703746, "predicted_n": 392, "predicted_ms": 1176.378, "predicted_per_token_ms": 3.008639386189258, "predicted_per_second": 332.37615800363494}}
    data: [DONE]
    ```

    **Field Descriptions:**

    - `usage` - Final chunk only: token usage.
    - `x_lemonade_route` - The routing decision for a request that named a `collection.router` model, present when the request sets `route_trace`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed to `default_model`.
<!-- END GENERATED: openai.chat_completions -->

<!-- BEGIN GENERATED: openai.completions -->
## `POST /v1/completions`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Continues a text prompt, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The text to continue, as a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends tokens as server-sent events as they are generated; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Sequences that stop generation, as a string or an array of strings. The returned text leaves them out. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | `true` returns the prompt before the completion. Non-streaming only. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | `true`, or a number of top alternatives per token, returns the log probability of each output token. Defaults to `false`. Non-streaming only. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
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

    - `x_lemonade_route` - The routing decision for a request that named a `collection.router` model, present when the request sets `route_trace`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed to `default_model`.

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
    - `x_lemonade_route` - The routing decision for a request that named a `collection.router` model, present when the request sets `route_trace`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed to `default_model`.
<!-- END GENERATED: openai.completions -->

<!-- BEGIN GENERATED: openai.embeddings -->
## `POST /v1/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns vector representations (embeddings) of the input text for semantic search, clustering and similarity comparisons, loading the model on first use. Embedding models use the `llamacpp` or `flm` recipe.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The text to embed, as a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The embedding model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | `float` or `base64`. Defaults to `float`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
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

- `object` - Always `"list"`.
- `data` - One embedding per input.
  - `object` - Always `"embedding"`.
  - `index` - Position of the input text in the request.
  - `embedding` - The vector, as an array of floats, or a base64 string when `encoding_format` is `base64`.
- `model` - The model that produced the embeddings.
- `usage` - Token usage.
  - `prompt_tokens` - Tokens in the input.
  - `total_tokens` - Tokens processed in total.
<!-- END GENERATED: openai.embeddings -->

<!-- BEGIN GENERATED: openai.responses -->
## `POST /v1/responses`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates a model response to an input, in OpenAI's Responses format, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The input, as a string or an array of input items. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends tokens as server-sent events as they are generated; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

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

    - `output` - Output items; a message item carries its text in `content[].text`.
    - `x_lemonade_route` - The routing decision for a request that named a `collection.router` model, present when the request sets `route_trace`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed to `default_model`.

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

    - `type` - Event type, passed through from the backend, such as `response.created`, `response.output_text.delta` or `response.completed`; see OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming).
    - `response` - `response.created` and `response.completed` only: the response so far.
    - `delta` - `response.output_text.delta` only: the next text.
<!-- END GENERATED: openai.responses -->

<!-- BEGIN GENERATED: openai.audio_transcriptions -->
## `POST /v1/audio/transcriptions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Transcribes an audio file to text, loading the model on first use. Whisper (whisper.cpp), Moonshine and FastFlowLM models serve this endpoint.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `file` | Yes | The audio file to transcribe, as WAV. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `model` | Yes | The transcription model to use (e.g., `Whisper-Tiny`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). Defaults to the backend's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | `json`, `verbose_json`, `text`, `srt` or `vtt`. Defaults to `json`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Text to guide the transcription's style or continue a previous segment. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

On FastFlowLM, which reports no segment timestamps, `srt` and `vtt` answer `400` and `verbose_json` has no `segments`.

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

=== "PowerShell (plain text)"

    ```powershell
    curl.exe http://localhost:13305/v1/audio/transcriptions `
      -F "model=Whisper-Tiny" `
      -F "response_format=text" `
      -F file=@speech.wav
    ```

=== "Bash (plain text)"

    ```bash
    curl http://localhost:13305/v1/audio/transcriptions \
      -F "model=Whisper-Tiny" \
      -F "response_format=text" \
      -F file=@speech.wav
    ```

### Response format

=== "Transcript responses"

    `200`

    ```json
    {"text": " Just seeing if this is working.\n"}
    ```

    **Field Descriptions:**

    - `text` - The transcribed text.
    - `segments` - `verbose_json` only: timestamped segments.

=== "Plain text responses"

    `200`

    ```text
     Just seeing if this is working.
    ```
<!-- END GENERATED: openai.audio_transcriptions -->

## `WS /realtime`
<sub>![Status](https://img.shields.io/badge/status-partial-yellow)</sub>

Realtime Audio Transcription API via WebSocket (OpenAI SDK compatible). Stream audio from a microphone and receive transcriptions in real-time with Voice Activity Detection (VAD).

Models with the `realtime-transcription` [label](#model-labels) serve it: the Whisper models and the Moonshine streaming models. Audio must be 16 kHz mono PCM16; downsample higher rates on the client.

### Connection

Connect on the main HTTP port (default 13305), the same port as the REST endpoints, naming the model:

```
ws://localhost:13305/v1/realtime?model=Whisper-Tiny
```

OpenAI Realtime SDK clients connect to `/v1/realtime` as-is. When `LEMONADE_API_KEY` is set, pass `?api_key=KEY` as a query parameter.

A dedicated WebSocket port also remains for backward compatibility. It is OS-assigned and reported as `websocket_port` by [`/v1/health`](./lemonade.md#get-v1health), and it speaks the identical protocol, so moving a client to the main port changes only its URL. The main port also leaves one port to expose remotely and works through reverse proxies that pass `Upgrade: websocket`:

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

Send base64-encoded PCM16 audio in chunks of about 85 to 256 ms.

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

Set `turn_detection` to `null` to disable server-side VAD and use explicit commits instead. The server then buffers audio and emits no VAD or interim transcription events until `input_audio_buffer.commit`:

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

<!-- BEGIN GENERATED: openai.images_generations -->
## `POST /v1/images/generations`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates an image from a text prompt. CPU inference takes minutes per image; a GPU backend is much faster.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The text description of the image to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The diffusion model to use (e.g., `SD-Turbo`, `Krea-2-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Image size as `WIDTHxHEIGHT`, such as `256x256`. Defaults to the model's own, else `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Only `b64_json`, a base64-encoded image, is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. Defaults to the model's own, else `20`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. Defaults to the model's own, else `7.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output; a negative value picks a random seed. Defaults to the backend's fixed seed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

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

Edits one or more reference images as a prompt describes. The request is `multipart/form-data`. CPU inference takes minutes per image; a GPU backend is much faster.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | PNG or JPEG image upload. For multiple references, repeat `image[]` in order. Files are forwarded without re-encoding. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | A text description of the desired edit. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mask` | No | A PNG mask: white areas are edited and black areas are kept. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`, such as `512x512`. Defaults to the first reference image's size, scaled down to fit the model's `image_defaults.soft_pixel_cap` (2048x2048 pixels, about 4 MP, unless the model sets one). An explicit size skips that cap; backend limits still apply. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate, from `1` to `10`; other values answer `400`. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Only `b64_json`, a base64-encoded image, is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `user` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `background` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `quality` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `input_fidelity` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `output_compression` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Edit with the OpenAI Python Client

=== "Python (OpenAI client)"

    ```python
    from openai import OpenAI
    client = OpenAI(base_url="http://localhost:13305/v1", api_key="not-needed")
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

Generates a variation of a source image, sent as `multipart/form-data`. CPU inference takes minutes per image; a GPU backend is much faster.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The image model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | The source image (PNG). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Ignored; the variation depends only on the image. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`. Defaults to the model's own, else `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate, from `1` to `10`; other values answer `400`. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Only `b64_json`, a base64-encoded image, is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `user` | No | Accepted for OpenAI compatibility and ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Create Variations with the OpenAI Python Client

=== "Python (OpenAI client)"

    ```python
    from openai import OpenAI
    client = OpenAI(base_url="http://localhost:13305/v1", api_key="not-needed")
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

Upscales an image with an upscaling model such as `RealESRGAN-x4plus`. The model sets the upscale factor, which its name usually states.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `image` | Yes | Base64-encoded PNG image to upscale. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The upscaling model to use, such as `RealESRGAN-x4plus` or `Remacri-4x-TheNoise`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The model is downloaded on first use.

A missing `image` or `model`, a model without the `upscaling` label, or one whose recipe cannot upscale answers `400`; an unknown model `404`; a failed upscale `500`; each with an `error` object.

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

    BASE_URL = "http://localhost:13305/v1"

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

- `created` - Unix timestamp of when the upscaled image was generated.
- `data` - The upscaled image.
  - `b64_json` - Base64-encoded PNG of the upscaled image.
<!-- END GENERATED: openai.images_upscale -->

<!-- BEGIN GENERATED: openai.audio_speech -->
## `POST /v1/audio/speech`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates speech audio from text. The model selects the engine: `kokoro-v1` speaks with fixed voices ([Kokoros](https://github.com/lucasjinreal/Kokoros)), while `OpenMOSS-TTS` and `MOSS-TTS-Local` can also clone a voice or design one from a description. `MOSS-VoiceGen` is kept for compatibility.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The text to speak. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use (e.g., `kokoro-v1`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `speed` | No | Speaking speed. Defaults to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice` | No | For `kokoro-v1`, a voice name: OpenAI's voices (`alloy`, `ash`, ...) or Kokoro's own (`af_sky`, `am_echo`, ...), defaulting to `shimmer`. For OpenMOSS models, a free-text voice or style instruction, e.g. `a calm, deep male narrator voice`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `reference_wav_b64` | No | Lemonade extension (OpenMOSS voice cloning): base64-encoded WAV sample of the voice to clone. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice_design_description` | No | Lemonade extension (OpenMOSS voice design): a description of the voice to invent, e.g. `a warm low female voice with a British accent`. Lemonade renders a short sample in that voice and uses it as the reference, so the effect is the same as supplying `reference_wav_b64` yourself. Ignored when `reference_wav_b64` is also present. Only this field triggers design; `voice` never does. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Container for the returned audio. Defaults to `mp3` when buffered and `pcm` when streaming, or to the backend's first supported format when it cannot encode those. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream_format` | No | `audio` streams the response. It selects the transport only: the container still comes from `response_format`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream` | No | `true` streams the response, like `stream_format: audio`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Accepted `response_format` values depend on the backend: `kokoro-v1` encodes `mp3`, `wav`, `opus` and `pcm`, and OpenMOSS encodes `wav` or `pcm`. Streaming produces `pcm` only, so a streaming request with another explicit `response_format` answers `400`. OpenMOSS PCM is returned as `audio/pcm` with `X-MOSS-Sample-Rate` and `X-MOSS-Channels` headers, since its format depends on the model (24 kHz mono for `OpenMOSS-TTS`, 48 kHz stereo for `MOSS-TTS-Local`).

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

<!-- BEGIN GENERATED: openai.models -->
## `GET /v1/models`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the models available on the server, in OpenAI's format extended with the fields described below.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `show_all` (query) | No | `true` also lists catalog models that are not downloaded. Defaults to `false`, which lists downloaded models only. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A model alias whose target is listed appears too, as a copy of the target with the alias as its `id`.

With cloud providers configured, their models are listed too, with `recipe: "cloud"`, a `cloud_provider` field and a provider-prefixed id such as `fireworks.kimi-k2p5`. Models of `openai`-wire providers accept chat completions and completions requests; `anthropic`-wire providers are served through [`POST /v1/messages`](./anthropic.md#post-v1messages). See [Cloud Offload](../guide/configuration/cloud.md).

`HEAD` returns `200 OK` with an empty body.

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

- `object` - Always `"list"`.
- `data` - The model objects.
  - `id` - Model identifier for loading and inference requests.
  - `created` - Always `1234567890`, for OpenAI compatibility.
  - `object` - Always `"model"`.
  - `owned_by` - Always `"lemonade"`.
  - `checkpoint` - The main checkpoint in the model's registry; see `registry_source`.
  - `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
  - `size` - Size in GB; omitted when unknown.
  - `max_context_window` - Largest text context the model supports, read from local metadata: downloaded GGUF models and installed FLM text models.
  - `context_length` - Tokens the model handles in one request: the loaded `ctx_size` while it runs, else the configured `ctx_size`, else `max_context_window`; omitted when none is known.
  - `max_output_tokens` - Cloud models only: the most tokens one completion can generate, when the provider reports it.
  - `max_completion_tokens` - Cloud models only: `max_output_tokens` under its OpenAI name.
  - `downloaded` - Whether the model is downloaded.
  - `update_available` - Whether the model's registry has a newer commit; `false` except for downloaded registry models.
  - `suggested` - Whether the model is recommended for general use.
  - `labels` - The model's capabilities and characteristics; see [Model Labels](./openai.md#model-labels).
  - `image_defaults` - Image models only: default generation parameters.
    - `steps` - Number of inference steps, such as `4` for turbo models or `20` for standard models.
    - `cfg_scale` - Classifier-free guidance scale, such as `1.0` for turbo models or `7.5` for standard models.
    - `width` - Default image width in pixels.
    - `height` - Default image height in pixels.
    - `sampling_method` - Default sampler, when the model sets one.
    - `flow_shift` - Default flow shift, when the model sets one.
    - `soft_pixel_cap` - Largest output area, in pixels, that `images/edits` infers from the first reference image when `size` is omitted.
  - `components` - Omni collections only (`recipe: "collection.omni"`): the names of its component models, in order.
  - `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
  - `checkpoints` - Every checkpoint by role, such as `main`, `mmproj`, `draft`, `text_encoder` or `vae`.
  - `source` - Where the model came from: a registry name, `local_upload`, `local_path` or `extra_models_dir`.
  - `registry_source` - Remote registry the checkpoint downloads from: `huggingface` or `modelscope`.
  - `recipe_options` - The model's own options: its image defaults and registry options, with what was saved in `recipe_options.json` on top.
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

**Deployment labels:** determine which backend endpoint the model is routed to.
Every model names exactly one deployment mode, and a model is never given two
labels that name different modes:

| Label | Endpoint | Description |
|-------|----------|-------------|
| `chat` | `/chat/completions`, `/completions`, `/responses` | Text-generating LLM. This label is what makes a model an LLM; it is not inferred from `reasoning`/`vision`/`tool-calling`/`chat-transcription`, which are characteristics rather than deployment modes. |
| `transcription` | `/audio/transcriptions` | Speech-to-text transcription model (e.g. Whisper). An omni LLM that accepts audio in a chat turn is not one of these: it carries `chat` and the `chat-transcription` capability below. |
| `embeddings` | `/embeddings` | Produces text embedding vectors. Also accepted as `embedding`. |
| `reranking` | `/rerank` | Scores and reranks a list of passages given a query. |
| `image` | `/images/generations`, `/images/edits`, `/images/variations` | Text-to-image generation model. |
| `tts` | `/audio/speech` | Text-to-speech synthesis model. |
| `audio-generation` | `/audio/generations` | Text-to-audio generation model (e.g. music, sound effects). |
| `classification` | `/classify` | Text classification model. Also accepted as `classifier`. |
| `3d` | `/3d/generations` | Text- or image-to-3D mesh generation model. |

When a model declares no deployment label at all, it inherits its recipe's
default: `chat` for `llamacpp`, `flm`, `ryzenai-llm`, `vllm` and `cloud`,
`transcription` for `whispercpp`, `image` for `sd-cpp`, `tts` for `kokoro`, and
so on.

Two label sets describe a model that cannot exist, and are refused rather than
repaired:

- **A mode the recipe's backend does not serve.** `/classify` is served only by
  `onnxruntime`, so `labels: ["classification"]` on a `llamacpp` model is an
  error; register it as the chat model it is.
- **Two different modes.** `labels: ["chat", "embeddings"]` on a `llamacpp` model
  is an error even though llama.cpp serves both: the subprocess is launched for
  one mode, so the second would name an endpoint it was never configured to
  answer. Register one model per mode. The legacy `embedding` and `reranking`
  booleans count as mode claims here, exactly as the labels do.

[`POST /v1/pull`](./lemonade.md#post-v1pull) answers `400` and registers nothing.
A stored `user_models.json` entry that breaks these rules is skipped at startup with
an error naming it; the file is left for you to correct.

**Input-modality labels:** the model accepts additional input types in `/chat/completions`:

| Label | Description |
|-------|-------------|
| `vision` | Accepts image attachments in chat messages. |
| `chat-transcription` | Accepts audio attachments in chat messages and transcribes them as part of its answer (e.g. Qwen2.5-Omni). Like `vision`, this is something a chat model can do, not a deployment mode of its own; a model carrying it also carries `chat`. It is distinct from `transcription`, which deploys a dedicated ASR model on `/audio/transcriptions`. |

**Streaming labels:** capability flags for real-time features:

| Label | Description |
|-------|-------------|
| `realtime-transcription` | Supports the WebSocket `/realtime` endpoint for live microphone transcription. |

**Runtime labels:** affect backend launch defaults:

| Label | Description |
|-------|-------------|
| `mtp` | Enables llama.cpp MTP draft decoding defaults (`--spec-type draft-mtp --spec-draft-n-max 3 --spec-draft-p-min 0.75`); users can override these with `llamacpp_args`. |

**Image capability labels:** carried alongside `image`; they refine what the model is offered for without changing its deployment mode:

| Label | Description |
|-------|-------------|
| `edit` | Tuned for editing an input image (`/images/edits`). Also selects the model for the `edit_image` role in an omni collection. |
| `upscaling` | Image upscaling model (e.g. Real-ESRGAN, `/images/upscale`). Used as a component in image pipelines rather than offered on its own. |

**Characteristic labels:** informational, do not affect routing:

| Label | Description |
|-------|-------------|
| `hot` | Featured or popular model, highlighted in the UI. |
| `reasoning` | Uses extended chain-of-thought reasoning (e.g. DeepSeek, Qwen3). |
| `tool-calling` | Supports function/tool calling in chat completions. |
| `coding` | Tuned for code generation and software tasks. |

<!-- BEGIN GENERATED: openai.models_id -->
## `GET /v1/models/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns one model, as the same model object [`GET /v1/models`](#get-v1models) lists.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

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

- `id` - Model identifier for loading and inference requests.
- `created` - Always `1234567890`, for OpenAI compatibility.
- `object` - Always `"model"`.
- `owned_by` - Always `"lemonade"`.
- `checkpoint` - The main checkpoint in the model's registry; see `registry_source`.
- `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
- `size` - Size in GB; omitted when unknown.
- `max_context_window` - Largest text context the model supports, read from local metadata: downloaded GGUF models and installed FLM text models.
- `context_length` - Tokens the model handles in one request: the loaded `ctx_size` while it runs, else the configured `ctx_size`, else `max_context_window`; omitted when none is known.
- `max_output_tokens` - Cloud models only: the most tokens one completion can generate, when the provider reports it.
- `max_completion_tokens` - Cloud models only: `max_output_tokens` under its OpenAI name.
- `downloaded` - Whether the model is downloaded.
- `update_available` - Whether the model's registry has a newer commit; `false` except for downloaded registry models.
- `suggested` - Whether the model is recommended for general use.
- `labels` - The model's capabilities and characteristics; see [Model Labels](./openai.md#model-labels).
- `image_defaults` - Image models only: default generation parameters.
  - `steps` - Number of inference steps, such as `4` for turbo models or `20` for standard models.
  - `cfg_scale` - Classifier-free guidance scale, such as `1.0` for turbo models or `7.5` for standard models.
  - `width` - Default image width in pixels.
  - `height` - Default image height in pixels.
  - `sampling_method` - Default sampler, when the model sets one.
  - `flow_shift` - Default flow shift, when the model sets one.
  - `soft_pixel_cap` - Largest output area, in pixels, that `images/edits` infers from the first reference image when `size` is omitted.
- `components` - Omni collections only (`recipe: "collection.omni"`): the names of its component models, in order.
- `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
- `checkpoints` - Every checkpoint by role, such as `main`, `mmproj`, `draft`, `text_encoder` or `vae`.
- `source` - Where the model came from: a registry name, `local_upload`, `local_path` or `extra_models_dir`.
- `registry_source` - Remote registry the checkpoint downloads from: `huggingface` or `modelscope`.
- `recipe_options` - The model's own options: its image defaults and registry options, with what was saved in `recipe_options.json` on top.
- `cloud_provider` - Cloud models only: the provider that serves the model.
- `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
- `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
- `system_prompt` - Omni collections only: the collection's system prompt override.
- `audio_defaults` - Audio models only: default generation parameters.
- `version` - Router collections only: the policy document's version.
- `routing` - Router collections only: the routing policy.
<!-- END GENERATED: openai.models_id -->

<!-- BEGIN GENERATED: openai.models_id_files -->
## `GET /v1/models/{id}/files`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the resolved local files of one model, for model-detail views such as the app's Files tab. It inventories one model, not drive storage.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `include_paths` (query) | No | `true` adds each file's absolute `path`, which can reveal local usernames and the cache layout. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown model answers `404` with `code` `model_not_found`.

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

- `model_id` - The model's public id.
- `files` - The model's resolved files.
  - `name` - Base filename from the resolved path.
  - `path` - Absolute path; present only with `include_paths=true`.
  - `role` - Checkpoint role, for example `main`, `mmproj`, or another recipe-specific role.
  - `size_bytes` - File size in bytes. Directories are summed recursively. Missing files report `0`.
  - `exists` - Whether the resolved path currently exists on disk.
<!-- END GENERATED: openai.models_id_files -->

<!-- BEGIN GENERATED: openai.models_id_options_get -->
## `GET /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reads a model's recipe options, separated by layer, without loading it. With `POST` and `DELETE` on the same path, this manages per-model options independently of [`/v1/load`](./lemonade.md#post-v1load).

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Per-architecture defaults are chosen by the architecture in the model's GGUF metadata, so a model that has not been downloaded yet shows every key with the value it has before those defaults apply.

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
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_get -->

<!-- BEGIN GENERATED: openai.models_id_options_post -->
## `POST /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Saves recipe options for a model without loading it. The body is a flat object of the recipe options [`/v1/load`](./lemonade.md#post-v1load) accepts.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Context size to save: a positive whole number, or `-1` to pin the model to automatic sizing even when the server-wide `ctx_size` is a specific number. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | `true` validates and resolves the request identically but persists nothing: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk. Use it to preview a change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | No | Ignored; the URL names the model. Accepted so `effective` can be posted back as is. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The request merges into the model's saved entry, so keys it leaves out keep their values. `null` removes an option, and the model falls back to the next layer of the [priority chain](./lemonade.md#post-v1load). [`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.

A `400` answers an empty or invalid body, an unrecognized option or one from another recipe, a value of the wrong type or an invalid `ctx_size`, a non-boolean `dry_run`, and nothing from that request is saved.

A running model keeps its current options until it is next loaded.

`pinned` answers `400` here and is omitted from `effective` and `defaults`; it belongs to [`/v1/load`](./lemonade.md#post-v1load) and [`/internal/pin`](./internal.md#post-internalpin).

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
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_post -->

<!-- BEGIN GENERATED: openai.models_id_options_delete -->
## `DELETE /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Resets a model to its defaults by erasing its `recipe_options.json` entry. Defaults from its registry entry and the server's configuration still apply.

Requires `LEMONADE_API_KEY` when it is set.

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
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`.
<!-- END GENERATED: openai.models_id_options_delete -->
