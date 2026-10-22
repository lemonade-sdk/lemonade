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
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation, downloading and loading the model if it is not loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `messages` | Yes | The conversation so far, in OpenAI's message format: each message has a `role` and `content`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends the response as server-sent events as it progresses; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Sequences that stop generation, as a string or an array of strings. The returned text leaves them out. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | `true` returns the log probability of each output token. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `frequency_penalty` | No | From `-2.0` to `2.0`: positive values discourage tokens in proportion to how often they have appeared. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tool_choice` | No | Which tool the model must call, in OpenAI's format: `auto`, `none` or `required`. vLLM and cloud models also take an object naming a function, which llama.cpp models treat as `auto`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | The format the model must output, in OpenAI's format, such as `{"type": "json_object"}`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | OpenAI's name for `max_tokens`. Send one of the two: when both are sent, the backend picks, and llama.cpp uses `max_tokens` while vLLM uses `max_completion_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_thinking` | No | Lemonade extension: `false` turns off reasoning on models that think. `thinking: false` does the same, and `enable_thinking` wins when both are sent. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the per-condition `trace` to `x_lemonade_route`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Server-Side Tools

Omni collection orchestration is a Lemonade extension to the Chat Completions API; requests to other models are unaffected.

When `model` names an Omni **collection** model (`recipe: "collection.omni"`, e.g., `LMX-Omni-52B-Halo`), the route runs an internal tool-calling loop instead of a plain completion. The server injects the reference system prompt and tools, routes to the collection's chat component, executes the omni tools (image generation/editing, text-to-speech) against the matching components, and returns one OpenAI-compatible response. Generated media is embedded in the assistant `content`:

- **images** → markdown `![generated image](data:image/png;base64,…)`
- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`

Both non-streaming and `stream: true` are supported. In streaming mode the media arrives in `content` deltas of at most 16,000 characters each, the moment its tool finishes, and an error arrives as `Error: <message>` text in `content` followed by `finish_reason: "stop"`.

**Merge semantics.** The built-in omni system prompt goes before a client-provided one. Client-provided `tools` are merged with the built-in omni tools. The server resolves omni tool calls internally; calls to client-provided tools are returned in a `finish_reason: "tool_calls"` response for the client to execute and resume. Targeting a collection name invokes the server-side loop; targeting a component LLM name bypasses it and returns a plain completion. See [Lemonade Omni Models](../dev/lemonade-omni.md) for details.

### Examples

#### Non-streaming

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

**Response:** `200 OK`

```json
{
  "id": "chatcmpl-Ur8WXBSFmCy4xSW1gr9J6imPMcSBsmG7",
  "object": "chat.completion",
  "created": 1791990411,
  "model": "Qwen3-0.6B-GGUF",
  "system_fingerprint": "b11393-dbe4c3ed4",
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

#### Streaming

=== "PowerShell"

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

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the population of Paris?"}],
          "stream": true
        }'
    ```

**Response:** `200 OK`

```text
data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "delta": {"role": "assistant", "content": null}, "finish_reason": null}]}
data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "delta": {"reasoning_content": "Okay"}, "finish_reason": null}]}
data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990411, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "delta": {"reasoning_content": ","}, "finish_reason": null}]}
...
data: {"id": "chatcmpl-VvTqNsZLc33H6Q9uQlBqRQ4sk3dvCSX5", "object": "chat.completion.chunk", "created": 1791990412, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "timings": {"cache_n": 14, "prompt_n": 1, "prompt_ms": 4.081, "prompt_per_token_ms": 4.081, "prompt_per_second": 245.03798088703746, "predicted_n": 392, "predicted_ms": 1176.378, "predicted_per_token_ms": 3.008639386189258, "predicted_per_second": 332.37615800363494}}
data: [DONE]
```

#### Image input

Asks a vision model what `image.png` shows. A `messages[*].content` array mixes `text` and `image_url` items, with each image as a base64 data URL, such as one from `FileReader.readAsDataURL(...)` in a web app.

=== "PowerShell"

    ```powershell
    $image = [Convert]::ToBase64String([IO.File]::ReadAllBytes("image.png"))
    $body = @"
    {
          "model": "Qwen3.5-0.8B-GGUF",
          "messages": [
            {
              "role": "user",
              "content": [
                {"type": "text", "text": "What is in this image?"},
                {
                  "type": "image_url",
                  "image_url": {"url": "data:image/png;base64,$image"}
                }
              ]
            }
          ],
          "stream": false
        }
    "@
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body $body
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d "{
          \"model\": \"Qwen3.5-0.8B-GGUF\",
          \"messages\": [
            {
              \"role\": \"user\",
              \"content\": [
                {\"type\": \"text\", \"text\": \"What is in this image?\"},
                {
                  \"type\": \"image_url\",
                  \"image_url\": {\"url\": \"data:image/png;base64,$(base64 -w0 image.png)\"}
                }
              ]
            }
          ],
          \"stream\": false
        }"
    ```

**Response:** `200 OK`

```json
{
  "id": "chatcmpl-CXUhlv8GPMS7biiWtkuu27a3Gy3eNEUS",
  "object": "chat.completion",
  "created": 1792076819,
  "model": "Qwen3.5-0.8B-GGUF",
  "system_fingerprint": "b11393-dbe4c3ed4",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "This image is a **minimalist graphic design**.\n\nHere are the specific details:\n\n*   **Central Element:** There is a solid **red circle** (often described as a \"dot\" or \"target\" symbol) centered on the image.\n*   **Background:** The background is a colorful gradient (transitioning from blue on the left, to grey in the middle, and to purple/grey on the right).\n*   **Style:** It is a simple, flat vector design—no shading, no textures—often used in interfaces or branding to draw immediate attention to a specific focus point.",
        "reasoning_content": "The user is asking about the content of the image.\n\n1.  **Analyze the image:** T... (2,072 characters)"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 82,
    "completion_tokens": 619,
    "total_tokens": 701,
    "prompt_tokens_details": {"cached_tokens": 0}
  },
  "timings": {
    "cache_n": 0,
    "predicted_ms": 2706.181,
    "predicted_n": 619,
    "predicted_per_second": 228.36609968069394,
    "predicted_per_token_ms": 4.378933656957929,
    "prompt_ms": 52.671,
    "prompt_n": 82,
    "prompt_per_second": 1556.8339313853924,
    "prompt_per_token_ms": 0.6423292682926829
  }
}
```

#### Server-side tools

Runs the [server-side tools](#server-side-tools) loop on `LMX-Omni-5.5B-Lite`, whose image tool draws the requested picture.

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "LMX-Omni-5.5B-Lite",
          "messages": [{"role": "user", "content": "Draw a red apple on a table."}],
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "LMX-Omni-5.5B-Lite",
          "messages": [{"role": "user", "content": "Draw a red apple on a table."}],
          "stream": false
        }'
    ```

**Response:** `200 OK`

```json
{
  "id": "chatcmpl-i37OtXcuKiFJ82bZjOEJmIDgbbrSLIWk",
  "object": "chat.completion",
  "created": 1792076833,
  "model": "LMX-Omni-5.5B-Lite",
  "system_fingerprint": "b11393-dbe4c3ed4",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "I've generated an image of a red apple on a table for you! The image shows a cla... (393,258 characters)"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 1102,
    "completion_tokens": 63,
    "total_tokens": 1165,
    "prompt_tokens_details": {"cached_tokens": 1082}
  },
  "timings": {
    "cache_n": 1082,
    "draft_n": 69,
    "draft_n_accepted": 39,
    "predicted_ms": 706.57,
    "predicted_n": 63,
    "predicted_per_second": 87.74785230055055,
    "predicted_per_token_ms": 11.396290322580645,
    "prompt_ms": 97.681,
    "prompt_n": 20,
    "prompt_per_second": 204.74810863934647,
    "prompt_per_token_ms": 4.88405
  }
}
```

### Response format

#### JSON

- `id` - Identifier of the completion.
- `object` - Always `"chat.completion"`.
- `created` - Unix timestamp of when the completion was created.
- `model` - The model that generated the response.
- `system_fingerprint` - Build identifier of the backend, when it reports one.
- `choices` - The generated messages, one per choice.
  - `index` - Position of the choice in `choices`.
  - `message` - The generated assistant message.
    - `role` - Always `"assistant"`.
    - `content` - The message text.
    - `reasoning_content` - Reasoning models only: the model's thinking.
    - `tool_calls` - Calls to the request's tools.
  - `finish_reason` - Why generation stopped, such as `stop`, `length` or `tool_calls`.
- `usage` - Token counts for the request.
  - `prompt_tokens` - Tokens in the prompt.
  - `completion_tokens` - Tokens generated.
  - `total_tokens` - Sum of `prompt_tokens` and `completion_tokens`.
  - `prompt_tokens_details` - Breakdown of the prompt tokens, such as `cached_tokens`, when the backend reports it.
- `timings` - Prompt processing and generation timings, passed through from backends that report them, such as llama.cpp.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

#### Event stream

One event per `data:` line. The stream ends with `data: [DONE]`.

##### Chunk

A chunk of the assistant's message.

- `id` - Identifier of the completion, the same on every chunk.
- `object` - Always `"chat.completion.chunk"`.
- `created` - Unix timestamp of when the completion was created.
- `model` - The backend's name for the model, such as the GGUF file path for llama.cpp models.
- `system_fingerprint` - Build identifier of the backend, when it reports one.
- `choices` - The next part of each choice's message.
  - `index` - Position of the choice in `choices`.
  - `delta` - The part of the assistant message generated since the previous chunk.
    - `role` - First chunk only: always `"assistant"`.
    - `content` - The next message text.
    - `reasoning_content` - Reasoning models only: the next part of the model's thinking.
    - `tool_calls` - Parts of calls to the request's tools, matched by `index`: a call's first part carries its `id` and `function.name`, and later parts carry the next piece of `function.arguments`.
  - `finish_reason` - Why generation stopped, such as `stop`, `length` or `tool_calls`, on the choice's last chunk; `null` before it.
- `usage` - Token counts for the request, on an extra last chunk with an empty `choices`. Sent when the request sets `stream_options.include_usage`, and always by vLLM models.
- `timings` - Final chunk only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

##### Error event

Sent in place of the rest of the stream when the backend fails after the stream has started. No `[DONE]` follows it.

- `error` - The backend's error, as an object or, when the backend sent one, a string.
  - `message` - What failed.
  - `type` - The error's type, such as `backend_error`.
  - `status_code` - The status the backend answered with, or `502` when it closed the stream without a response.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` is not an LLM, which is checked after it loads |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: openai.chat_completions -->

<!-- BEGIN GENERATED: openai.completions -->
## `POST /v1/completions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Continues a text prompt, downloading and loading the model if it is not loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The text to continue, as a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends the response as server-sent events as it progresses; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Sequences that stop generation, as a string or an array of strings. The returned text leaves them out. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | `true` asks the backend to return the prompt before the completion. llama.cpp models ignore it. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `logprobs` | No | `true`, or a number of top alternatives per token, returns the log probability of each output token. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `frequency_penalty` | No | From `-2.0` to `2.0`: positive values discourage tokens in proportion to how often they have appeared. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the per-condition `trace` to `x_lemonade_route`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Examples

#### Non-streaming

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

**Response:** `200 OK`

```json
{
  "id": "chatcmpl-f4ExiyuFYahpItfZ9kgOsndtQcaHrBjz",
  "object": "text_completion",
  "created": 1791943134,
  "model": "Qwen3-0.6B-GGUF",
  "system_fingerprint": "b11393-dbe4c3ed4",
  "choices": [
    {
      "index": 0,
      "text": " And what does the Parisian metro system include?\n\nThe population of Paris is approximately 2 million people. The Parisian metro system includes the Paris Métro, the Paris Underground, and the Paris TGV, which is a train system.\n\nNow, the question is: What is the population of the city of Paris?\n\n",
      "logprobs": null,
      "finish_reason": "length"
    }
  ],
  "usage": {
    "prompt_tokens": 7,
    "completion_tokens": 64,
    "total_tokens": 71,
    "prompt_tokens_details": {"cached_tokens": 0}
  },
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

#### Streaming

=== "PowerShell"

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

=== "Bash"

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

**Response:** `200 OK`

```text
data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "text": " What", "logprobs": null, "finish_reason": null}]}
data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "text": " is", "logprobs": null, "finish_reason": null}]}
data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "text": " the", "logprobs": null, "finish_reason": null}]}
...
data: {"id": "chatcmpl-QP1hqogXI4lxTag36ZFzMXdKhpHcqVKv", "object": "text_completion", "created": 1791943134, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "choices": [{"index": 0, "text": "", "logprobs": null, "finish_reason": "length"}], "usage": {"prompt_tokens": 7, "completion_tokens": 64, "total_tokens": 71, "prompt_tokens_details": {"cached_tokens": 6}}, "timings": {"cache_n": 6, "prompt_n": 1, "prompt_ms": 3.04, "prompt_per_token_ms": 3.04, "prompt_per_second": 328.9473684210526, "predicted_n": 64, "predicted_ms": 184.45, "predicted_per_token_ms": 2.9277777777777776, "predicted_per_second": 341.5559772296015}}
data: [DONE]
```

### Response format

#### JSON

- `id` - Identifier of the completion.
- `object` - Always `"text_completion"`.
- `created` - Unix timestamp of when the completion was created.
- `model` - The model that generated the response.
- `system_fingerprint` - Build identifier of the backend, when it reports one.
- `choices` - The generated text, one entry per choice.
  - `index` - Position of the choice in `choices`.
  - `text` - The generated text.
  - `logprobs` - Log probabilities of the output tokens when the request sets `logprobs`, else `null`.
  - `finish_reason` - Why generation stopped, such as `stop` or `length`.
- `usage` - Token counts for the request.
  - `prompt_tokens` - Tokens in the prompt.
  - `completion_tokens` - Tokens generated.
  - `total_tokens` - Sum of `prompt_tokens` and `completion_tokens`.
  - `prompt_tokens_details` - Breakdown of the prompt tokens, such as `cached_tokens`, when the backend reports it.
- `timings` - Prompt processing and generation timings, passed through from backends that report them, such as llama.cpp.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

#### Event stream

One event per `data:` line. The stream ends with `data: [DONE]`.

##### Chunk

A chunk of the completion text.

- `id` - Identifier of the completion, the same on every chunk.
- `object` - Always `"text_completion"`.
- `created` - Unix timestamp of when the completion was created.
- `model` - The backend's name for the model, such as the GGUF file path for llama.cpp models.
- `system_fingerprint` - Build identifier of the backend, when it reports one.
- `choices` - The next part of each choice's text.
  - `index` - Position of the choice in `choices`.
  - `text` - The text generated since the previous chunk.
  - `logprobs` - Log probabilities of the output tokens when the request sets `logprobs`, else `null`.
  - `finish_reason` - Why generation stopped, such as `stop` or `length`, on the choice's last chunk; `null` before it.
- `usage` - Final chunk only: token counts for the request.
  - `prompt_tokens` - Tokens in the prompt.
  - `completion_tokens` - Tokens generated.
  - `total_tokens` - Sum of `prompt_tokens` and `completion_tokens`.
  - `prompt_tokens_details` - Breakdown of the prompt tokens, such as `cached_tokens`, when the backend reports it.
- `timings` - Final chunk only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

##### Error event

Sent in place of the rest of the stream when the backend fails after the stream has started. No `[DONE]` follows it.

- `error` - The backend's error, as an object or, when the backend sent one, a string.
  - `message` - What failed.
  - `type` - The error's type, such as `backend_error`.
  - `status_code` - The status the backend answered with, or `502` when it closed the stream without a response.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` is not an LLM, which is checked after it loads |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The backend's response lacks a field the route returns, such as `choices` or `labels` |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: openai.completions -->

<!-- BEGIN GENERATED: openai.embeddings -->
## `POST /v1/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns vector representations (embeddings) of the input text for semantic search, clustering and similarity comparisons, downloading and loading the model if it is not loaded. Embedding models use the `llamacpp` or `flm` recipe.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The text to embed, as a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | `float` or `base64`. Defaults to `float`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{
  "object": "list",
  "data": [
    {
      "object": "embedding",
      "index": 0,
      "embedding": [0.03513138368725777, 0.0010724958265200257, 0.0043548960238695145, -0.024502824991941452, -0.02874191850423813, 0.029377534985542297, -0.01320538017898798, 0.01702793687582016, ... 760 more]
    },
    ... 1 more
  ],
  "model": "nomic-embed-text-v1-GGUF",
  "usage": {"prompt_tokens": 12, "total_tokens": 12}
}
```

### Response format

- `object` - Always `"list"`.
- `data` - One embedding per input.
  - `object` - Always `"embedding"`.
  - `index` - Position of the input text in the request.
  - `embedding` - The vector, as an array of floats, or a base64 string when `encoding_format` is `base64`.
- `model` - The model that produced the embeddings.
- `usage` - Token usage.
  - `prompt_tokens` - Tokens in the input.
  - `total_tokens` - Tokens processed in total.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model`'s backend cannot embed, such as a text-to-speech model |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
| `501` |  | `model` is a llama.cpp model that is not an embedding model |
<!-- END GENERATED: openai.embeddings -->

<!-- BEGIN GENERATED: openai.responses -->
## `POST /v1/responses`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates a model response to an input, in OpenAI's Responses format, downloading and loading the model if it is not loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | The input, as a string or an array of input items. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens to consider during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability of the top tokens to consider during nucleus sampling, from `0.0` to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends the response as server-sent events as it progresses; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the per-condition `trace` to `x_lemonade_route`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

### Examples

#### Non-streaming

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

**Response:** `200 OK`

```json
{
  "id": "resp_UMCsVMSLV0H1cGKewMgTRY02v5PgKvr3",
  "object": "response",
  "created_at": 1791943139,
  "completed_at": 1791943139,
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
    "output_tokens": 49,
    "total_tokens": 91,
    "input_tokens_details": {"cached_tokens": 0}
  }
}
```

#### Streaming

=== "PowerShell"

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

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Llama-3.2-1B-Instruct-GGUF",
          "input": "What is the population of Paris?",
          "stream": true
        }'
    ```

**Response:** `200 OK`

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

### Response format

#### JSON

- `id` - Identifier of the response.
- `object` - Always `"response"`.
- `created_at` - Unix timestamp of when the response was created.
- `completed_at` - Unix timestamp of when the response completed, when the backend reports it.
- `model` - The model that generated the response.
- `status` - State of the response, such as `completed`.
- `output` - Output items; a message item carries its text in `content[].text`.
- `usage` - Token counts for the request.
  - `input_tokens` - Tokens in the input.
  - `output_tokens` - Tokens generated.
  - `total_tokens` - Sum of `input_tokens` and `output_tokens`.
  - `input_tokens_details` - Breakdown of the input tokens, such as `cached_tokens`, when the backend reports it.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

#### Event stream

One event per `data:` line. The stream ends with `data: [DONE]`.

##### Response event

An event of the response, passed through from the backend.

- `type` - Event type, passed through from the backend, such as `response.created`, `response.output_text.delta` or `response.completed`; see OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming).
- `response` - Lifecycle events, such as `response.created`, `response.in_progress` and `response.completed`, only: the response so far.
- `item` - Output item events, such as `response.output_item.added`, only: the output item.
- `item_id` - Delta events only: the `id` of the output item the text belongs to.
- `delta` - Delta events, such as `response.output_text.delta` and `response.reasoning_text.delta`, only: the next text.
- `timings` - `response.completed` only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp.
- `x_lemonade_route` - The routing decision, when `model` names a `collection.router` model. See [Routing](./router.md#routing-collectionrouter) for the decision object.

##### Error event

Sent in place of the rest of the stream when the backend fails after the stream has started. No `[DONE]` follows it.

- `error` - The backend's error, as an object or, when the backend sent one, a string.
  - `message` - What failed.
  - `type` - The error's type, such as `backend_error`.
  - `status_code` - The status the backend answered with, or `502` when it closed the stream without a response.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The request does not stream and `model` is a FastFlowLM or cloud model, which has no Responses API |
| `400` | `model_not_applicable` | The request does not stream and `model` is not an LLM |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
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
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | The language of the audio (ISO 639-1 code, e.g., `en`, `es`, `fr`). Defaults to `auto`, which detects it, for whisper.cpp models, and to the backend's own for the others. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | `json`, `verbose_json`, `text`, `srt` or `vtt`. Defaults to `json`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Text to guide the transcription's style or continue a previous segment. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. Defaults to `0` for whisper.cpp models, and to the backend's own for FastFlowLM models. Moonshine models ignore it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

On FastFlowLM, which reports no segment timestamps, `verbose_json` has no `segments`.

### Examples

#### Transcript

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

**Response:** `200 OK`

```json
{"text": " Just seeing if this is working.\n"}
```

#### Plain text

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/audio/transcriptions `
      -F "model=Whisper-Tiny" `
      -F "response_format=text" `
      -F file=@speech.wav
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/transcriptions \
      -F "model=Whisper-Tiny" \
      -F "response_format=text" \
      -F file=@speech.wav
    ```

**Response:** `200 OK`

```text
 Just seeing if this is working.
```

### Response format

- `text` - The transcribed text.
- `segments` - `verbose_json` only: timestamped segments.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` |  | The request is not `multipart/form-data` |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` cannot transcribe, which is found after it loads; the error's `type` is `unsupported_operation` |
| `400` |  | `response_format` is `srt` or `vtt` and `model` is a FastFlowLM model, which reports no segment timestamps |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
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
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Image size as `WIDTHxHEIGHT`, such as `256x256`. Defaults to the model's own, else `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Only `b64_json`, a base64-encoded image, is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps; lower is faster, higher can improve quality. Defaults to the model's own setting, else the backend's. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. Defaults to the model's own, else `7.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output; a negative value picks a random seed. Defaults to the backend's fixed seed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

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

### Response format

- `created` - Unix timestamp of when the image was generated.
- `data` - The generated images, one entry per image.
  - `b64_json` - Base64-encoded PNG.
- `output_format` - File format of the images, such as `png`, when the backend reports it.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The backend's reply does not parse, or a request field not listed above has the wrong JSON type |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error, for example when `model` cannot serve this route, an error whose `type` is `unsupported_operation` |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: openai.images_generations -->

<!-- BEGIN GENERATED: openai.images_edits -->
## `POST /v1/images/edits`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Edits one or more reference images as a prompt describes. The request is `multipart/form-data`. CPU inference takes minutes per image; a GPU backend is much faster.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` or `image[]` | Yes | PNG or JPEG image upload. For multiple references, repeat `image[]` in order. Files are forwarded without re-encoding. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | A text description of the desired edit. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mask` | No | A PNG mask: white areas are edited and black areas are kept. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`, such as `512x512`. Defaults to the first reference image's size, scaled down to fit the model's `image_defaults.soft_pixel_cap` (2048x2048 pixels, about 4 MP, unless the model sets one). An explicit size skips that cap; backend limits still apply. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate, from `1` to `10`. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
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

### Example

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

**Response:** `200 OK`

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

### Response format

- `created` - Unix timestamp of when the image was generated.
- `data` - The generated images, one entry per image.
  - `b64_json` - Base64-encoded PNG.
- `output_format` - File format of the images, such as `png`, when the backend reports it.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` |  | The request is not `multipart/form-data` |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The form has files under both `image` and `image[]` |
| `400` |  | The backend's reply does not parse, or a request field not listed above has the wrong JSON type |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error, for example when `model` cannot serve this route, an error whose `type` is `unsupported_operation` |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: openai.images_edits -->

<!-- BEGIN GENERATED: openai.images_variations -->
## `POST /v1/images/variations`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates a variation of a source image, sent as `multipart/form-data`. CPU inference takes minutes per image; a GPU backend is much faster.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` or `image[]` | Yes | The source image (PNG). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Ignored; the variation depends only on the image. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`. Defaults to the model's own, else `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate, from `1` to `10`. Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
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

### Example

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

**Response:** `200 OK`

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

### Response format

- `created` - Unix timestamp of when the image was generated.
- `data` - The generated images, one entry per image.
  - `b64_json` - Base64-encoded PNG.
- `output_format` - File format of the images, such as `png`, when the backend reports it.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` |  | The request is not `multipart/form-data` |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The backend's reply does not parse, or a request field not listed above has the wrong JSON type |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error, for example when `model` cannot serve this route, an error whose `type` is `unsupported_operation` |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
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

### Upscale a Generated Image

To upscale an image from [`POST /v1/images/generations`](#post-v1imagesgenerations), pass its `b64_json` as `image`.

### Example

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

**Response:** `200 OK`

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

### Response format

- `created` - Unix timestamp of when the upscaled image was generated.
- `data` - The upscaled image.
  - `b64_json` - Base64-encoded PNG of the upscaled image.
- `output_format` - File format of the images, such as `png`, when the backend reports it.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` has no `upscaling` label |
| `400` |  | `model`'s recipe cannot upscale |
| `400` |  | The backend's reply does not parse, or a request field not listed above has the wrong JSON type |
| `404` |  | `model` names no model in the registry, or downloading it fails |
| `500` |  | The backend produces no upscaled image |
| `500` |  | The request fails unexpectedly |
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
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `speed` | No | Speaking speed. Defaults to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice` | No | For `kokoro-v1`, a voice name: OpenAI's voices (`alloy`, `ash`, ...) or Kokoro's own (`af_sky`, `am_echo`, ...), defaulting to `shimmer`. For OpenMOSS models, a free-text voice or style instruction, e.g. `a calm, deep male narrator voice`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `reference_wav_b64` | No | Lemonade extension (OpenMOSS voice cloning): base64-encoded WAV sample of the voice to clone. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice_design_description` | No | Lemonade extension (OpenMOSS voice design): a description of the voice to invent, e.g. `a warm low female voice with a British accent`. Lemonade renders a short sample in that voice and uses it as the reference, so the effect is the same as supplying `reference_wav_b64` yourself. Ignored when `reference_wav_b64` is also present. Only this field triggers design; `voice` never does. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Container for the returned audio. Defaults to `mp3` when buffered and `pcm` when streaming, or to the backend's first supported format when it cannot encode those. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream_format` | No | `audio` streams the response. It selects the transport only: the container still comes from `response_format`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream` | No | `true` streams the response, like `stream_format: audio`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Accepted `response_format` values depend on the backend: `kokoro-v1` encodes `mp3`, `wav`, `opus` and `pcm`, and OpenMOSS encodes `wav` or `pcm`. Streaming produces `pcm` only. OpenMOSS PCM is returned as `audio/pcm` with `X-MOSS-Sample-Rate` and `X-MOSS-Channels` headers, since its format depends on the model (24 kHz mono for `OpenMOSS-TTS`, 48 kHz stereo for `MOSS-TTS-Local`).

### Examples

#### Non-streaming

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

**Response:** `200 OK`, `audio/mpeg`, 40,516 bytes

#### Streaming

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
          "stream_format": "audio"
        }' `
      -OutFile output.l16
    ```

=== "Bash"

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

**Response:** `200 OK`, `audio/l16;rate=24000;endianness=little-endian`, 97,200 bytes

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` | `model_not_applicable` | `model` is not a text-to-speech model, which is checked before it loads |
| `400` |  | `response_format` is not a format `model`'s backend produces, or, when streaming, one it streams |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The request fails unexpectedly |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
| `502` |  | The backend produces no output, for example after it crashes or runs out of GPU memory |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models
    ```

**Response:** `200 OK`

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
    ... 10 more
  ]
}
```

### Response format

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
  - `components` - The names of a collection's component models, in order; empty for other models.
  - `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
  - `checkpoints` - Every checkpoint by role, such as `main`, `mmproj`, `draft`, `text_encoder` or `vae`.
    - The checkpoint that fills the role named by its key.
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

**Deployment labels**: determine which backend endpoint the model is routed to.
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

**Input-modality labels**: the model accepts additional input types in `/chat/completions`:

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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF
    ```

**Response:** `200 OK`

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

### Response format

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
- `components` - The names of a collection's component models, in order; empty for other models.
- `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
- `checkpoints` - Every checkpoint by role, such as `main`, `mmproj`, `draft`, `text_encoder` or `vae`.
  - The checkpoint that fills the role named by its key.
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

### Errors

| Status | Code | When |
|--------|------|------|
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
<!-- END GENERATED: openai.models_id -->

<!-- BEGIN GENERATED: openai.models_id_files -->
## `GET /v1/models/{id}/files`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the resolved local files of one model, for model-detail views such as the app's Files tab. It inventories one model, not drive storage.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models). An alias's id is not accepted here; use its target. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `include_paths` (query) | No | `true` adds each file's absolute `path`, which can reveal local usernames and the cache layout. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files
    ```

**Response:** `200 OK`

```json
{
  "model_id": "Qwen3-0.6B-GGUF",
  "files": [
    {"name": "Qwen3-0.6B-Q4_0.gguf", "role": "main", "size_bytes": 382156480, "exists": true}
  ]
}
```

### Response format

- `model_id` - The model's public id.
- `files` - The model's resolved files.
  - `name` - Base filename from the resolved path.
  - `path` - Absolute path; present only with `include_paths=true`.
  - `role` - Checkpoint role, for example `main`, `mmproj`, or another recipe-specific role.
  - `size_bytes` - File size in bytes. Directories are summed recursively. Missing files report `0`.
  - `exists` - Whether the resolved path currently exists on disk.

### Errors

| Status | Code | When |
|--------|------|------|
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `404` | `model_load_error` | Resolving or listing `id`'s files fails |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options
    ```

**Response:** `200 OK`

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

### Response format

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`. `null` means automatic sizing found insufficient memory and no valid context is available.

### Errors

| Status | Code | When |
|--------|------|------|
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `500` | `internal_error` | Reading or saving the model's options fails, for example on an unwritable options file |
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
| `ctx_size` | No | Context size: a positive whole number, or `-1` to size it automatically even when the server-wide `ctx_size` is a specific number. Only recipes that take a context size use it, such as `llamacpp`, `flm`, `ryzenai-llm` and `vllm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | `true` reports what the request would do without doing it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | No | Ignored; the URL names the model. Accepted so `effective` can be posted back as is. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The request merges into the model's saved entry, so keys it leaves out keep their values. `null` removes an option, as do `-1`, `""` and `"auto"` for any option but `ctx_size`, and the model falls back to the next layer of the [priority chain](./lemonade.md#post-v1load). [`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.

A request that fails validation saves nothing.

With `dry_run`, the request is validated and resolved as a save would be, but nothing is persisted: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk.

A running model keeps its current options until it is next loaded.

`pinned` is omitted from `effective` and `defaults`; it belongs to [`/v1/load`](./lemonade.md#post-v1load) and [`/internal/pin`](./internal.md#post-internalpin).

### Example

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

**Response:** `200 OK`

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

### Response format

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`. `null` means automatic sizing found insufficient memory and no valid context is available.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The body names an option that the model's recipe does not accept |
| `400` |  | The body sets `pinned`, which belongs to `/v1/load` and `/internal/pin` |
| `400` |  | A field not listed above has the wrong JSON type |
| `400` |  | `ctx_size` is not a positive whole number or `-1`, or a whole-number option has a fractional value |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `500` | `internal_error` | Reading or saving the model's options fails, for example on an unwritable options file |
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

### Example

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

**Response:** `200 OK`

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

### Response format

- `model_name` - The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body.
- `recipe` - The recipe the option names belong to.
- `saved` - The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`.
- `effective` - The body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses now: every option the recipe accepts, resolved through the full priority chain. Posting it back whole saves every resolved value as an override, so send only the options that changed.
- `defaults` - What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically.
- `resolved_ctx_size` - The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`. `null` means automatic sizing found insufficient memory and no valid context is available.

### Errors

| Status | Code | When |
|--------|------|------|
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `500` | `internal_error` | Reading or saving the model's options fails, for example on an unwritable options file |
<!-- END GENERATED: openai.models_id_options_delete -->
