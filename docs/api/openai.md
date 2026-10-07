# OpenAI-Compatible API

This page documents Lemonade's implementation of the [OpenAI API](https://developers.openai.com/api/docs), and the Lemonade extensions to it.

<!-- BEGIN GENERATED: openai.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/chat/completions`](#post-v1chatcompletions) | Chat Completions |
| `POST` | [`/v1/completions`](#post-v1completions) | Text Completions |
| `POST` | [`/v1/responses`](#post-v1responses) | Responses API |
| `POST` | [`/v1/embeddings`](#post-v1embeddings) | Embeddings |
| `POST` | [`/v1/audio/transcriptions`](#post-v1audiotranscriptions) | Audio Transcription |
| `POST` | [`/v1/audio/speech`](#post-v1audiospeech) | Text to speech |
| `POST` | [`/v1/images/generations`](#post-v1imagesgenerations) | Image Generation |
| `POST` | [`/v1/images/edits`](#post-v1imagesedits) | Image Editing |
| `POST` | [`/v1/images/variations`](#post-v1imagesvariations) | Image Variations |
| `POST` | [`/v1/images/upscale`](#post-v1imagesupscale) | Image Upscaling |
| `GET` | [`/v1/models`](#get-v1models) | List models available locally |
| `GET` | [`/v1/models/{id}/files`](#get-v1modelsidfiles) | List resolved local file metadata for one model |
| `GET` | [`/v1/models/{id}/options`](#get-v1modelsidoptions) | Read a model's saved, effective, and default recipe options |
| `POST` | [`/v1/models/{id}/options`](#post-v1modelsidoptions) | Save recipe options for a model without loading it |
| `DELETE` | [`/v1/models/{id}/options`](#delete-v1modelsidoptions) | Reset a model's recipe options to defaults |
| `GET` | [`/v1/models/{id}`](#get-v1modelsid) | Retrieve a specific model by ID |
<!-- END GENERATED: openai.summary -->

The [Realtime API](#ws-realtime) streams audio transcription over WebSocket.

<!-- BEGIN GENERATED: openai.chat_completions -->
## `POST /v1/chat/completions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation, loading the model on first use.

Naming an Omni collection (`recipe: "collection.omni"`) runs a server-side tool-calling loop instead; see [Server-Side Tools](#server-side-tools). Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

Also served at `/api/v0/chat/completions`, `/api/v1/chat/completions` and `/v0/chat/completions`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | Yes | Conversation so far. Each message has a `role` (`system`, `user`, `assistant` or `tool`) and `content`; see [Image Input](#image-input) for images. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream tokens as server-sent events as they are generated. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | A string or an array of up to 4 strings where generation stops. The returned text does not contain the stop sequence. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Return the log probability of each output token. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0; 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens considered during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability, between 0.0 and 1.0, of the top tokens considered during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | Tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. Deprecated by OpenAI in favor of `max_completion_tokens`; the two are mutually exclusive. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | Upper bound on generated tokens. Mutually exclusive with `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_thinking` | No | Lemonade extension: `false` turns off reasoning on models that think. The OpenAI-compatible `thinking: false` is accepted too. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France?", "role": "user"}],
          "enable_thinking": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France?", "role": "user"}],
          "enable_thinking": false
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "choices": [
        {
          "finish_reason": "stop",
          "index": 0,
          "message": {"content": "The capital of France is **Paris**.", "role": "assistant"}
        }
      ],
      "created": 1791392196,
      "id": "chatcmpl-c42RVaWGJWcJv8fCzyJ9fsBzfjxtQ6Ra",
      "model": "Qwen3-0.6B-GGUF",
      "object": "chat.completion",
      "system_fingerprint": "b11393-dbe4c3ed4",
      "timings": {
        "cache_n": 0,
        "predicted_ms": 29.43,
        "predicted_n": 10,
        "predicted_per_second": 305.81039755351685,
        "predicted_per_token_ms": 3.27,
        "prompt_ms": 10.14,
        "prompt_n": 23,
        "prompt_per_second": 2268.2445759368834,
        "prompt_per_token_ms": 0.44086956521739135
      },
      "usage": {
        "completion_tokens": 10,
        "prompt_tokens": 23,
        "prompt_tokens_details": {"cached_tokens": 0},
        "total_tokens": 33
      }
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "object", "created", "model", "choices"],
      "properties": {
        "choices": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["index", "message", "finish_reason"],
            "properties": {
              "finish_reason": {"type": "string"},
              "index": {"type": "integer"},
              "message": {
                "type": "object",
                "required": ["role"],
                "properties": {
                  "content": {"type": ["string", "null"]},
                  "reasoning_content": {
                    "description": "Reasoning models only: the model's thinking.",
                    "type": "string"
                  },
                  "role": {"const": "assistant"},
                  "tool_calls": {"description": "Calls to the request's tools.", "type": "array"}
                }
              }
            }
          }
        },
        "created": {"type": "integer"},
        "id": {"type": "string"},
        "model": {"type": "string"},
        "object": {"const": "chat.completion"},
        "usage": {
          "type": "object",
          "required": ["prompt_tokens", "completion_tokens", "total_tokens"],
          "properties": {
            "completion_tokens": {"type": "integer"},
            "prompt_tokens": {"type": "integer"},
            "total_tokens": {"type": "integer"}
          }
        },
        "x_lemonade_route": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace. Present ONLY when the request set route_trace=true; minimal/omitted by default so policy-internal signals do not leak to end users.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        }
      }
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/chat/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France?", "role": "user"}],
          "stream": true,
          "enable_thinking": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/chat/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France?", "role": "user"}],
          "stream": true,
          "enable_thinking": false
        }'
    ```

=== "Response"

    `200`

    ```text
    data: {"choices": [{"finish_reason": null, "index": 0, "delta": {"role": "assistant", "content": null}}], "created": 1791396305, "id": "chatcmpl-oD69li5P5xakMLDbNnQQWLwNaYugoz9j", "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "chat.completion.chunk"}
    data: {"choices": [{"finish_reason": null, "index": 0, "delta": {"content": "The"}}], "created": 1791396305, "id": "chatcmpl-oD69li5P5xakMLDbNnQQWLwNaYugoz9j", "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "chat.completion.chunk"}
    data: {"choices": [{"finish_reason": null, "index": 0, "delta": {"content": " capital"}}], "created": 1791396305, "id": "chatcmpl-oD69li5P5xakMLDbNnQQWLwNaYugoz9j", "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "chat.completion.chunk"}
    ...
    data: {"choices": [{"finish_reason": "stop", "index": 0, "delta": {}}], "created": 1791396305, "id": "chatcmpl-oD69li5P5xakMLDbNnQQWLwNaYugoz9j", "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "chat.completion.chunk", "timings": {"cache_n": 0, "prompt_n": 23, "prompt_ms": 70.621, "prompt_per_token_ms": 3.070478260869565, "prompt_per_second": 325.6821625295592, "predicted_n": 10, "predicted_ms": 35.399, "predicted_per_token_ms": 3.933222222222222, "predicted_per_second": 254.2444701827735}}
    data: [DONE]
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "object", "created", "model", "choices"],
      "properties": {
        "choices": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["index", "delta"],
            "properties": {
              "delta": {
                "type": "object",
                "properties": {
                  "content": {"type": ["string", "null"]},
                  "reasoning_content": {"type": ["string", "null"]},
                  "role": {"const": "assistant"}
                }
              },
              "finish_reason": {"type": ["string", "null"]},
              "index": {"type": "integer"}
            }
          }
        },
        "created": {"type": "integer"},
        "id": {"type": "string"},
        "model": {"type": "string"},
        "object": {"const": "chat.completion.chunk"},
        "usage": {"description": "Final chunk only: token usage.", "type": "object"},
        "x_lemonade_route": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace. Present ONLY when the request set route_trace=true; minimal/omitted by default so policy-internal signals do not leak to end users.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.chat_completions -->

### Image Input

To send images to `chat/completions`, pass a `messages[*].content` array that mixes `text` and `image_url` items. The image can be provided as a base64 data URL (for example, from `FileReader.readAsDataURL(...)` in web apps). The model must carry the `vision` label.

```bash
curl http://localhost:13305/v1/chat/completions \
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
        ]
      }'
```

The answer arrives in `choices[0].message.content`, as for a text-only request:

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

### Server-Side Tools

> **Note:** Omni collection orchestration is a Lemonade-specific extension to the Chat Completions API; it is not part of the OpenAI specification. Requests that target ordinary (non-collection) models are unaffected and are fully OpenAI-compatible.

When `model` names an Omni **collection** model (`recipe: "collection.omni"`, e.g., `LMX-Omni-52B-Halo`), this endpoint runs an internal tool-calling loop instead of a plain completion. The server injects the reference system prompt and tools, routes to the collection's chat component, executes the omni tools (image generation/editing, text-to-speech) against the matching components, and returns one OpenAI-compatible response. Generated media is embedded in the assistant `content`:

- **images**: markdown `![generated image](data:image/png;base64,…)`
- **speech**: `<audio>data:audio/mpeg;base64,…</audio>`

Both non-streaming and `stream: true` are supported. In streaming mode the media arrives as a content delta on a `chat.completion.chunk` frame the moment its tool finishes.

**Merge semantics.** A client-provided system prompt is prepended by the built-in omni system prompt. Client-provided `tools` are merged with the built-in omni tools. The server resolves omni tool calls internally; calls to client-provided tools are returned in a `finish_reason: "tool_calls"` response for the client to execute and resume. Targeting a collection name invokes the server-side loop; targeting a component LLM name bypasses it and returns a plain completion. See [Lemonade Omni Models](../dev/lemonade-omni.md) for details.

```bash
curl http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "LMX-Omni-52B-Halo",
        "messages": [{"role": "user", "content": "Draw a red apple on a table."}]
      }'
```

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

<!-- BEGIN GENERATED: openai.completions -->
## `POST /v1/completions`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Continues a prompt, loading the model on first use.

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

Also served at `/api/v0/completions`, `/api/v1/completions` and `/v0/completions`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text to continue, or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream tokens as server-sent events as they are generated. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | A string or an array of up to 4 strings where generation stops. The returned text does not contain the stop sequence. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | Return the prompt in addition to the completion. Non-streaming only. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Return the log probability of each output token. Non-streaming only. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0; 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens considered during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability, between 0.0 and 1.0, of the top tokens considered during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "Qwen3-0.6B-GGUF", "prompt": "The capital of France is", "max_tokens": 16}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/completions \
      -H "Content-Type: application/json" \
      -d '{"model": "Qwen3-0.6B-GGUF", "prompt": "The capital of France is", "max_tokens": 16}'
    ```

=== "Response"

    `200`

    ```json
    {
      "choices": [
        {
          "finish_reason": "length",
          "index": 0,
          "logprobs": null,
          "text": " Paris. The capital of France is located in the city of Paris. The capital"
        }
      ],
      "created": 1791392196,
      "id": "chatcmpl-OarKDHHMyaKkOWVrMOMjlYjPwLcNcjjG",
      "model": "Qwen3-0.6B-GGUF",
      "object": "text_completion",
      "system_fingerprint": "b11393-dbe4c3ed4",
      "timings": {
        "cache_n": 0,
        "predicted_ms": 46.695,
        "predicted_n": 16,
        "predicted_per_second": 321.23353678124,
        "predicted_per_token_ms": 3.113,
        "prompt_ms": 5.261,
        "prompt_n": 5,
        "prompt_per_second": 950.3896597605018,
        "prompt_per_token_ms": 1.0522
      },
      "usage": {
        "completion_tokens": 16,
        "prompt_tokens": 5,
        "prompt_tokens_details": {"cached_tokens": 0},
        "total_tokens": 21
      }
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "object", "created", "model", "choices"],
      "properties": {
        "choices": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["index", "text"],
            "properties": {
              "finish_reason": {"type": ["string", "null"]},
              "index": {"type": "integer"},
              "text": {"type": "string"}
            }
          }
        },
        "created": {"type": "integer"},
        "id": {"type": "string"},
        "model": {"type": "string"},
        "object": {"const": "text_completion"},
        "usage": {"type": "object"},
        "x_lemonade_route": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace. Present ONLY when the request set route_trace=true; minimal/omitted by default so policy-internal signals do not leak to end users.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        }
      }
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": true,
          "max_tokens": 16
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/completions \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": true,
          "max_tokens": 16
        }'
    ```

=== "Response"

    `200`

    ```text
    data: {"choices": [{"text": " Paris", "index": 0, "logprobs": null, "finish_reason": null}], "created": 1791396305, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "text_completion", "id": "chatcmpl-2VMB52VrUkSqkUqJmoZxPUfXOjR884st"}
    data: {"choices": [{"text": ",", "index": 0, "logprobs": null, "finish_reason": null}], "created": 1791396305, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "text_completion", "id": "chatcmpl-2VMB52VrUkSqkUqJmoZxPUfXOjR884st"}
    data: {"choices": [{"text": " and", "index": 0, "logprobs": null, "finish_reason": null}], "created": 1791396305, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "text_completion", "id": "chatcmpl-2VMB52VrUkSqkUqJmoZxPUfXOjR884st"}
    ...
    data: {"choices": [{"text": "", "index": 0, "logprobs": null, "finish_reason": "length"}], "created": 1791396305, "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "system_fingerprint": "b11393-dbe4c3ed4", "object": "text_completion", "usage": {"completion_tokens": 16, "prompt_tokens": 5, "total_tokens": 21, "prompt_tokens_details": {"cached_tokens": 0}}, "id": "chatcmpl-2VMB52VrUkSqkUqJmoZxPUfXOjR884st", "timings": {"cache_n": 0, "prompt_n": 5, "prompt_ms": 114.001, "prompt_per_token_ms": 22.8002, "prompt_per_second": 43.85926439241761, "predicted_n": 16, "predicted_ms": 45.312, "predicted_per_token_ms": 3.0208, "predicted_per_second": 331.0381355932204}}
    data: [DONE]
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "object", "created", "model", "choices"],
      "properties": {
        "choices": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["index", "text"],
            "properties": {
              "finish_reason": {"type": ["string", "null"]},
              "index": {"type": "integer"},
              "text": {"type": "string"}
            }
          }
        },
        "created": {"type": "integer"},
        "id": {"type": "string"},
        "model": {"type": "string"},
        "object": {"const": "text_completion"},
        "usage": {"description": "Final chunk only: token usage.", "type": "object"},
        "x_lemonade_route": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace. Present ONLY when the request set route_trace=true; minimal/omitted by default so policy-internal signals do not leak to end users.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.completions -->

<!-- BEGIN GENERATED: openai.responses -->
## `POST /v1/responses`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates a response to an input, loading the model on first use.

Streaming relays the backend's semantic events as they arrive. llama.cpp sends OpenAI's event types, including `response.created`, `response.in_progress`, `response.output_item.added`, `response.reasoning_text.delta` for reasoning models, `response.output_text.delta`, `response.output_item.done` and `response.completed`. See OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming) for each type.

Naming a `collection.router` model routes the request to one of its candidates; see [Router API](./router.md).

Also served at `/api/v0/responses`, `/api/v1/responses` and `/v0/responses`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | A string, or a list of input items, for the model to respond to. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `repeat_penalty` | No | Number between 1.0 and 2.0; 1.0 means no penalty. Higher values discourage repetition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens considered during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Cumulative probability, between 0.0 and 1.0, of the top tokens considered during nucleus sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream semantic events as they are generated. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `route_trace` | No | Lemonade extension for `collection.router` models: `true` adds the routing decision to the response as `x_lemonade_route`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "input": "What is the capital of France? Answer in one word.",
          "max_output_tokens": 256
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "input": "What is the capital of France? Answer in one word.",
          "max_output_tokens": 256
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "completed_at": 1791392197,
      "created_at": 1791392197,
      "id": "resp_woKVIHTKNqr7dSxlecLSw7CBhVfyIECP",
      "model": "Qwen3-0.6B-GGUF",
      "object": "response",
      "output": [
        {
          "content": [
            {
              "text": "Okay, so the user is asking, \"What is the capital of France?\" and wants the answer in one word. Let me think about this.\n\nFirst, I need to recall the capital of France. I remember that France's capital is Paris. But wait, the user is asking for it in one word. So, maybe the answer is Paris. But wait, sometimes people might think of other capitals, like Brussels, but that's in Belgium. So Paris is definitely the capital. The user might be testing if I know the correct capital, and the answer is just Paris. But maybe I should confirm if there's another capital. Let me think again. France's capital is indeed Paris. So the answer is Paris. But the user wants it in one word. So the answer is Paris. That's correct. No other capitals are in use. So the answer is Paris.\n",
              "type": "reasoning_text"
            }
          ],
          "encrypted_content": "",
          "id": "rs_NVWj4XjE93GYvpsHHM6DjhKPat6t3iaS",
          "status": "completed",
          "summary": [],
          "type": "reasoning"
        },
        {
          "content": [{"annotations": [], "logprobs": [], "text": "Paris", "type": "output_text"}],
          "id": "msg_vv9SH0nWM89iqCAKFCthNPQgqVR4qb7s",
          "role": "assistant",
          "status": "completed",
          "type": "message"
        }
      ],
      "status": "completed",
      "usage": {
        "input_tokens": 20,
        "input_tokens_details": {"cached_tokens": 0},
        "output_tokens": 184,
        "total_tokens": 204
      }
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "object", "created_at", "model", "output"],
      "properties": {
        "created_at": {"type": "number"},
        "id": {"type": "string"},
        "model": {"type": "string"},
        "object": {"const": "response"},
        "output": {
          "description": "Output items; a message item carries its text in content[].text.",
          "type": "array",
          "items": {"type": "object"}
        },
        "status": {"type": "string"},
        "usage": {"type": "object"},
        "x_lemonade_route": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace. Present ONLY when the request set route_trace=true; minimal/omitted by default so policy-internal signals do not leak to end users.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        }
      }
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "input": "What is the capital of France? Answer in one word.",
          "max_output_tokens": 256,
          "stream": true
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "input": "What is the capital of France? Answer in one word.",
          "max_output_tokens": 256,
          "stream": true
        }'
    ```

=== "Response"

    `200`

    ```text
    event: response.created
    data: {"type": "response.created", "response": {"id": "resp_jmcW4byv4WFMKP8TIOvuFDR3jYCfkgtj", "object": "response", "status": "in_progress"}}
    event: response.in_progress
    data: {"type": "response.in_progress", "response": {"id": "resp_jmcW4byv4WFMKP8TIOvuFDR3jYCfkgtj", "object": "response", "status": "in_progress"}}
    event: response.output_item.added
    data: {"type": "response.output_item.added", "item": {"id": "rs_GaqUgRMOKlLoMWfNPtTtAtJE4dtZYnza", "summary": [], "type": "reasoning", "content": [], "encrypted_content": "", "status": "in_progress"}}
    ...
    event: response.completed
    data: {"type": "response.completed", "response": {"id": "resp_jmcW4byv4WFMKP8TIOvuFDR3jYCfkgtj", "object": "response", "created_at": 1791396306, "status": "completed", "model": "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "output": [{"id": "rs_GaqUgRMOKlLoMWfNPtTtAtJE4dtZYnza", "summary": [], "type": "reasoning", "content": [{"text": "Okay, so the user is asking, \"What is the capital of France?\" and wants the answ... (1,039 characters)", "type": "reasoning_text"}], "encrypted_content": ""}], "usage": {"input_tokens": 20, "output_tokens": 256, "total_tokens": 276, "input_tokens_details": {"cached_tokens": 0}}}, "timings": {"cache_n": 0, "prompt_n": 20, "prompt_ms": 8.398, "prompt_per_token_ms": 0.4199, "prompt_per_second": 2381.5194093831865, "predicted_n": 256, "predicted_ms": 769.261, "predicted_per_token_ms": 3.0167098039215685, "predicted_per_second": 331.4869725619783}}
    data: [DONE]
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["type"],
      "properties": {
        "delta": {"description": "response.output_text.delta: the next text.", "type": "string"},
        "response": {
          "description": "response.created and response.completed: the response so far.",
          "type": "object"
        },
        "type": {
          "description": "Event type, one of the backend's OpenAI event types such as response.created, response.output_text.delta or response.completed.",
          "type": "string"
        }
      }
    }
    ```
<!-- END GENERATED: openai.responses -->

<!-- BEGIN GENERATED: openai.embeddings -->
## `POST /v1/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns vector representations of input text for semantic search, clustering and similarity comparisons, loading the model on first use.

Only embedding models (the `embeddings` label) using the `llamacpp` or `flm` recipe serve this endpoint.

Also served at `/api/v0/embeddings`, `/api/v1/embeddings` and `/v0/embeddings`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Embedding model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to embed, or an array of texts. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `encoding_format` | No | Format of the returned embeddings: `float` (default) or `base64`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
      "data": [
        {
          "embedding": [0.03513138368725777, 0.0010724958265200257, 0.0043548960238695145, -0.024502824991941452, -0.02874191850423813, 0.029377534985542297, -0.01320538017898798, 0.01702793687582016, ... 760 more],
          "index": 0,
          "object": "embedding"
        },
        {
          "embedding": [-0.024310413748025894, -0.03385873883962631, -0.003393277060240507, 0.0042533064261078835, -0.015559273771941662, 0.026714053004980087, -0.009407692588865757, 0.033160898834466934, ... 760 more],
          "index": 1,
          "object": "embedding"
        }
      ],
      "model": "nomic-embed-text-v1-GGUF",
      "object": "list",
      "usage": {"prompt_tokens": 12, "total_tokens": 12}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["object", "data", "model"],
      "properties": {
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["object", "index", "embedding"],
            "properties": {
              "embedding": {
                "description": "The vector, as floats or a base64 string.",
                "type": ["array", "string"]
              },
              "index": {
                "description": "Position of the input text in the request.",
                "type": "integer"
              },
              "object": {"const": "embedding"}
            }
          }
        },
        "model": {"type": "string"},
        "object": {"const": "list"},
        "usage": {
          "type": "object",
          "properties": {"prompt_tokens": {"type": "integer"}, "total_tokens": {"type": "integer"}}
        }
      }
    }
    ```
<!-- END GENERATED: openai.embeddings -->

<!-- BEGIN GENERATED: openai.audio_transcriptions -->
## `POST /v1/audio/transcriptions`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Transcribes an audio file to text, loading the model on first use. The request is `multipart/form-data`.

Transcription models such as the Whisper family are downloaded automatically when first used.

**Limitations:** Only `wav` audio input is supported. On the FastFlowLM (FLM) backend, `srt` and `vtt` are rejected with a `400` because FLM returns no segment timestamps, and `verbose_json` returns the compact shape without a `segments` field.

Also served at `/api/v0/audio/transcriptions`, `/api/v1/audio/transcriptions` and `/v0/audio/transcriptions`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `file` | Yes | The audio file to transcribe. Only `wav` is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `model` | Yes | Transcription model, e.g. `Whisper-Tiny`, `Whisper-Base` or `Whisper-Small`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `language` | No | Language of the audio as an ISO 639-1 code, e.g. `en`, `es` or `fr`. Defaults to `auto`, which has whisper.cpp detect the language instead of assuming English. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Text to guide the transcription's style or continue a previous segment. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | `json` (default), `verbose_json`, `text`, `srt` or `vtt`. `text`, `srt` and `vtt` answer with plain text; `srt` and `vtt` need a backend that reports segment timestamps, such as whisper.cpp. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {"text": " Just seeing if this is working.\n"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["text"],
      "properties": {
        "segments": {"description": "verbose_json only: timestamped segments.", "type": "array"},
        "text": {"description": "The transcribed text.", "type": "string"}
      }
    }
    ```

### Response: `Text`

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

=== "Response"

    `200`

    ```text
     Just seeing if this is working.
    ```
<!-- END GENERATED: openai.audio_transcriptions -->

<!-- BEGIN GENERATED: openai.audio_speech -->
## `POST /v1/audio/speech`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Speaks the input text and returns the audio, loading the model on first use. Which engine serves the request depends on the model.

Supported models are `kokoro-v1` (fixed voices, [Kokoros](https://github.com/lucasjinreal/Kokoros) backend) and the OpenMOSS family: `OpenMOSS-TTS` and `MOSS-TTS-Local` support cloning and integrated voice design. `MOSS-VoiceGen` remains available as a legacy compatibility model.

**Limitations:** Which `response_format` values are accepted depends on the model's backend: `kokoro-v1` encodes `mp3`, `wav`, `opus` and `pcm`; OpenMOSS encodes buffered `wav` or `pcm`. Streaming is narrower for both backends and uses `pcm` only, so an explicit non-PCM `response_format` on a streaming request is rejected rather than mislabeled or silently transcoded. OpenMOSS raw PCM is returned as `audio/pcm` with `X-MOSS-Sample-Rate` and `X-MOSS-Channels` headers, because its native format is model-dependent (24 kHz mono for OpenMOSS-TTS, 48 kHz stereo for MOSS-TTS-Local).

A request for a model that is not a text-to-speech model answers `400` with `code` `model_not_applicable`, before any model loads.

Also served at `/api/v0/audio/speech`, `/api/v1/audio/speech` and `/v0/audio/speech`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Text-to-speech model, e.g. `kokoro-v1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | The text to speak. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice` | No | For `kokoro-v1`, a voice name: every OpenAI voice (`alloy`, `ash`, ...) and the Kokoro voices (`af_sky`, `am_echo`, ...). Defaults to `shimmer`. For OpenMOSS models, a free-text voice or style instruction, e.g. `a calm, deep male narrator voice`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `speed` | No | Speaking speed. Defaults to `1.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reference_wav_b64` | No | Lemonade extension for OpenMOSS voice cloning: a base64-encoded WAV sample of the voice to clone. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `voice_design_description` | No | Lemonade extension for OpenMOSS voice design: a description of a voice to invent, e.g. `a warm low female voice with a British accent`. Lemonade renders a short sample in that voice and uses it as the reference, with the same effect as supplying `reference_wav_b64`. Ignored when `reference_wav_b64` is also present. Only this field triggers design; `voice` never does. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Container for the returned audio; which values a model accepts depends on its backend. Defaults to `mp3` when buffered and `pcm` when streaming, falling back to the backend's first supported format when it cannot encode that default. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream_format` | No | Set to `audio` to stream the response; no other value is supported. This selects the transport only: the container still comes from `response_format`, and an explicit one is honored on both transports. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `stream` | No | `true` streams the response, like `stream_format: audio`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Binary`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/speech" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "kokoro-v1", "input": "Lemonade can speak!", "response_format": "mp3"}' `
      -OutFile output.mp3
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/speech \
      -H "Content-Type: application/json" \
      -d '{"model": "kokoro-v1", "input": "Lemonade can speak!", "response_format": "mp3"}' \
      --output output.mp3
    ```

=== "Response"

    `200`, `Content-Type: audio/mpeg`, 40,516 bytes

### Response: `BinaryStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/speech" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "kokoro-v1", "input": "Lemonade can speak!", "stream_format": "audio"}' `
      -OutFile output.l16
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/speech \
      -H "Content-Type: application/json" \
      -d '{"model": "kokoro-v1", "input": "Lemonade can speak!", "stream_format": "audio"}' \
      --output output.l16
    ```

=== "Response"

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

Generates an image from a text prompt, loading the model on first use.

**Performance:** CPU inference takes about 4 to 5 minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/generations`, `/api/v1/images/generations` and `/v0/images/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Diffusion model, e.g. `SD-Turbo` or `Krea-2-Turbo`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text description of the image to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Image size as `WIDTHxHEIGHT`, e.g. `512x512` or `256x256`. Defaults to `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate. Only `1` is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Only `b64_json` (a base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. SD-Turbo works well with 4. The default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. SD-Turbo uses low values (about 1.0). The default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. A random seed is used when omitted. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/images/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "SD-Turbo",
          "prompt": "A serene mountain landscape at sunset",
          "size": "256x256",
          "response_format": "b64_json",
          "steps": 4
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/generations \
      -H "Content-Type: application/json" \
      -d '{
          "model": "SD-Turbo",
          "prompt": "A serene mountain landscape at sunset",
          "size": "256x256",
          "response_format": "b64_json",
          "steps": 4
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "created": 1791392207,
      "data": [
        {
          "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAEdXRFWHRwYXJhbWV0ZXJzAEEgc2VyZW5l... (169,204 characters)"
        }
      ],
      "output_format": "png"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["created", "data"],
      "properties": {
        "created": {
          "description": "Unix timestamp of when the image was generated.",
          "type": "integer"
        },
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["b64_json"],
            "properties": {"b64_json": {"description": "Base64-encoded PNG.", "type": "string"}}
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.images_generations -->

<!-- BEGIN GENERATED: openai.images_edits -->
## `POST /v1/images/edits`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Edits a source image as a text prompt describes, loading the model on first use. The request is `multipart/form-data`.

Use an editing-capable model such as `Flux-2-Klein-4B` or `SD-Turbo`.

**Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/edits`, `/api/v1/images/edits` and `/v0/images/edits`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Diffusion model, e.g. `Flux-2-Klein-4B` or `SD-Turbo`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Source image to edit (PNG). The field may also be named `image[]`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text description of the desired edit. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mask` | No | Mask image (PNG). White areas are edited; black areas are preserved. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`, e.g. `512x512`. Defaults to `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of images to generate, from `1` to `10`. Defaults to `1`; values outside the range answer `400`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Only `b64_json` (a base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `steps` | No | Number of inference steps. The default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Classifier-free guidance scale. The default varies by model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `user` | No | OpenAI compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `background` | No | OpenAI compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `quality` | No | OpenAI compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `input_fidelity` | No | OpenAI compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `output_compression` | No | OpenAI compatibility field. Accepted and ignored by the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/images/edits `
      -F "model=SD-Turbo" `
      -F "prompt=Add a red barn in the background, photorealistic" `
      -F "size=256x256" `
      -F "steps=4" `
      -F image=@image.png
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/edits \
      -F "model=SD-Turbo" \
      -F "prompt=Add a red barn in the background, photorealistic" \
      -F "size=256x256" \
      -F "steps=4" \
      -F image=@image.png
    ```

=== "Response"

    `200`

    ```json
    {
      "created": 1791392208,
      "data": [
        {
          "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAEi3RFWHRwYXJhbWV0ZXJzAEFkZCBhIHJl... (129,408 characters)"
        }
      ],
      "output_format": "png"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["created", "data"],
      "properties": {
        "created": {
          "description": "Unix timestamp of when the image was generated.",
          "type": "integer"
        },
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["b64_json"],
            "properties": {"b64_json": {"description": "Base64-encoded PNG.", "type": "string"}}
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.images_edits -->

### Edit with the OpenAI Python Client

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

<!-- BEGIN GENERATED: openai.images_variations -->
## `POST /v1/images/variations`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates a variation of a source image, loading the model on first use. The request is `multipart/form-data`.

Unlike [`/v1/images/edits`](#post-v1imagesedits), this takes no `prompt`; a prompt field is ignored and the variation follows the input image alone.

**Performance:** CPU inference takes several minutes per image. GPU (ROCm) is significantly faster.

Also served at `/api/v0/images/variations`, `/api/v1/images/variations` and `/v0/images/variations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Diffusion model, e.g. `Flux-2-Klein-4B` or `SD-Turbo`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Source image (PNG). The field may also be named `image[]`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `size` | No | Output size as `WIDTHxHEIGHT`, e.g. `512x512`. Defaults to `512x512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `n` | No | Number of variations to generate, from `1` to `10`. Defaults to `1`; values outside the range answer `400`. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `response_format` | No | Only `b64_json` (a base64-encoded image) is supported. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `user` | No | OpenAI compatibility field. Accepted but not forwarded to the backend. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    curl.exe http://localhost:13305/v1/images/variations `
      -F "model=SD-Turbo" `
      -F "size=256x256" `
      -F image=@image.png
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/images/variations \
      -F "model=SD-Turbo" \
      -F "size=256x256" \
      -F image=@image.png
    ```

=== "Response"

    `200`

    ```json
    {
      "created": 1791392209,
      "data": [
        {
          "b64_json": "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAEPXRFWHRwYXJhbWV0ZXJzAHZhcmlhdGlv... (146,288 characters)"
        }
      ],
      "output_format": "png"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["created", "data"],
      "properties": {
        "created": {
          "description": "Unix timestamp of when the image was generated.",
          "type": "integer"
        },
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["b64_json"],
            "properties": {"b64_json": {"description": "Base64-encoded PNG.", "type": "string"}}
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.images_variations -->

### Create Variations with the OpenAI Python Client

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

<!-- BEGIN GENERATED: openai.images_upscale -->
## `POST /v1/images/upscale`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Upscales a base64-encoded image with a Real-ESRGAN model. The upscale factor depends on the model and is usually in its name.

Unlike [`/v1/images/edits`](#post-v1imagesedits) and [`/v1/images/variations`](#post-v1imagesvariations), this endpoint takes a JSON body, with the image as a base64 string.

The model must carry the `upscaling` label and is downloaded on first use. A missing `image` or `model`, or a model without the label, answers `400`; an unknown model answers `404`; a failed upscale answers `500`. Each error body is `{"error": {"message": ..., "type": ...}}`.

Also served at `/api/v0/images/upscale`, `/api/v1/images/upscale` and `/v0/images/upscale`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `image` | Yes | Base64-encoded PNG image to upscale. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | Upscaling model, e.g. `RealESRGAN-x4plus` or `Remacri-4x-TheNoise`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
      "created": 1791392214,
      "data": [
        {
          "b64_json": "iVBORw0KGgoAAAANSUhEUgAABAAAAAQACAIAAADwf7zUAAADV3RFWHRwYXJhbWV0ZXJzAApTdGVwczog... (1,819,044 characters)"
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["created", "data"],
      "properties": {
        "created": {
          "description": "Unix timestamp of when the image was generated.",
          "type": "integer"
        },
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["b64_json"],
            "properties": {"b64_json": {"description": "Base64-encoded PNG.", "type": "string"}}
          }
        }
      }
    }
    ```
<!-- END GENERATED: openai.images_upscale -->

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

<!-- BEGIN GENERATED: openai.models -->
## `GET /v1/models`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the models on the server in OpenAI's format, extended with Lemonade fields such as `checkpoint`, `recipe`, `size`, `downloaded`, `labels`, `context_length` and, when known, `max_context_window`. By default only downloaded models are listed, as OpenAI does.

When `lemond` is configured with cloud providers, cloud-routed models appear here alongside local ones with `recipe: "cloud"` and a `cloud_provider` field. They are dot-namespaced by provider (e.g. `fireworks.kimi-k2p5`) and accept the standard chat completions and completions requests; see [Cloud Offload](../guide/configuration/cloud.md).

Model aliases are listed too, each as a copy of its target model with the alias as its `id`.

`HEAD` answers `200` with no body.

Also served at `/api/v0/models`, `/api/v1/models` and `/v0/models`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `show_all` (query) | No | `true` lists every model in the catalog, including ones not downloaded yet. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models
    ```

=== "Response"

    `200`

    ```json
    {
      "data": [
        {
          "checkpoint": "lemonade-sdk/phishing-email-detection-distilbert-ONNX",
          "checkpoints": {"main": "lemonade-sdk/phishing-email-detection-distilbert-ONNX"},
          "components": [],
          "created": 1234567890,
          "downloaded": true,
          "id": "Phishing-Email-Detection-ONNX",
          "labels": ["classification"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "onnxruntime",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.25,
          "source": "huggingface",
          "suggested": false,
          "update_available": false
        },
        {
          "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
          "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Q4_0"},
          "components": [],
          "context_length": 40960,
          "created": 1234567890,
          "downloaded": true,
          "id": "Qwen3-0.6B-GGUF",
          "labels": ["chat", "reasoning", "tool-calling"],
          "max_context_window": 40960,
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "llamacpp",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.356,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf",
          "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf"},
          "components": [],
          "context_length": 40960,
          "created": 1234567890,
          "downloaded": true,
          "id": "Qwen3-0.6B-HRX",
          "labels": ["chat", "reasoning", "tool-calling"],
          "max_context_window": 40960,
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "llamacpp-hrx",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.356,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "amd/realesrgan-x4plus:RealESRGAN_x4plus.pth",
          "checkpoints": {"main": "amd/realesrgan-x4plus:RealESRGAN_x4plus.pth"},
          "components": [],
          "created": 1234567890,
          "downloaded": true,
          "id": "RealESRGAN-x4plus",
          "labels": ["upscaling", "image"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "sd-cpp",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.062,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "stabilityai/sd-turbo:sd_turbo.safetensors",
          "checkpoints": {"main": "stabilityai/sd-turbo:sd_turbo.safetensors"},
          "components": [],
          "context_length": 22691,
          "created": 1234567890,
          "downloaded": true,
          "id": "SD-Turbo",
          "image_defaults": {"cfg_scale": 1.0, "height": 512, "steps": 4, "width": 512},
          "labels": ["image"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "sd-cpp",
          "recipe_options": {"cfg_scale": 1.0, "height": 512, "steps": 4, "width": 512},
          "registry_source": "huggingface",
          "size": 4.86,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "ilintar/trellis2-gguf",
          "checkpoints": {"main": "ilintar/trellis2-gguf"},
          "components": [],
          "created": 1234567890,
          "downloaded": true,
          "id": "TRELLIS-3D",
          "labels": ["3d"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "trellis",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 30.7,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "ilintar/thinksound-gguf",
          "checkpoints": {"main": "ilintar/thinksound-gguf"},
          "components": [],
          "created": 1234567890,
          "downloaded": true,
          "id": "ThinkSound-SFX",
          "labels": ["audio-generation"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "thinksound",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 6.4,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "ggerganov/whisper.cpp:ggml-tiny.bin",
          "checkpoints": {
            "main": "ggerganov/whisper.cpp:ggml-tiny.bin",
            "npu_cache": "amd/whisper-tiny-onnx-npu:ggml-tiny-encoder-vitisai.rai"
          },
          "components": [],
          "context_length": 32768,
          "created": 1234567890,
          "downloaded": true,
          "id": "Whisper-Tiny",
          "labels": ["transcription", "realtime-transcription"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "whispercpp",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.072,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "mradermacher/jina-reranker-v1-tiny-en-GGUF:Q8_0",
          "checkpoints": {"main": "mradermacher/jina-reranker-v1-tiny-en-GGUF:Q8_0"},
          "components": [],
          "context_length": 8192,
          "created": 1234567890,
          "downloaded": true,
          "id": "jina-reranker-v1-tiny-en-GGUF",
          "labels": ["reranking"],
          "max_context_window": 8192,
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "llamacpp",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.034,
          "source": "huggingface",
          "suggested": false,
          "update_available": false
        },
        {
          "checkpoint": "mikkoph/kokoro-onnx",
          "checkpoints": {"main": "mikkoph/kokoro-onnx"},
          "components": [],
          "context_length": 32768,
          "created": 1234567890,
          "downloaded": true,
          "id": "kokoro-v1",
          "labels": ["tts"],
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "kokoro",
          "recipe_options": {},
          "registry_source": "huggingface",
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        },
        {
          "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
          "checkpoints": {"main": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S"},
          "components": [],
          "context_length": 8192,
          "created": 1234567890,
          "downloaded": true,
          "id": "nomic-embed-text-v1-GGUF",
          "labels": ["embeddings"],
          "max_context_window": 2048,
          "object": "model",
          "owned_by": "lemonade",
          "recipe": "llamacpp",
          "recipe_options": {},
          "registry_source": "huggingface",
          "size": 0.073,
          "source": "huggingface",
          "suggested": true,
          "update_available": false
        }
      ],
      "object": "list"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["object", "data"],
      "properties": {
        "data": {
          "type": "array",
          "items": {
            "type": "object",
            "required": [
              "id",
              "object",
              "created",
              "owned_by",
              "checkpoint",
              "checkpoints",
              "recipe",
              "downloaded",
              "update_available",
              "suggested",
              "source",
              "registry_source",
              "labels",
              "components",
              "recipe_options"
            ],
            "properties": {
              "audio_defaults": {
                "description": "Audio models only: default generation parameters.",
                "type": "object"
              },
              "checkpoint": {
                "description": "Main checkpoint: a Hugging Face or ModelScope repository and file, or a local path.",
                "type": "string"
              },
              "checkpoints": {
                "description": "Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.",
                "type": "object",
                "additionalProperties": {"type": "string"}
              },
              "cloud_provider": {
                "description": "Cloud models only: the provider that serves the model.",
                "type": "string"
              },
              "components": {
                "description": "Collections only: ordered component model names. Empty for other models.",
                "type": "array",
                "items": {"type": "string"}
              },
              "context_length": {
                "description": "Tokens one request can use: the loaded value when the model is running, else the configured ctx_size or the cloud provider's reported length.",
                "type": "integer"
              },
              "cost_input_per_million": {
                "description": "Cloud models only: USD per million input tokens, when the provider reports it.",
                "type": "number"
              },
              "cost_output_per_million": {
                "description": "Cloud models only: USD per million output tokens, when the provider reports it.",
                "type": "number"
              },
              "created": {
                "description": "Unix timestamp of when the model entry was created.",
                "type": "integer"
              },
              "downloaded": {
                "description": "Whether the model's files are on disk.",
                "type": "boolean"
              },
              "id": {
                "description": "Model identifier, used for loading and inference requests.",
                "type": "string"
              },
              "image_defaults": {
                "description": "Image models only: default generation parameters.",
                "type": "object",
                "properties": {
                  "cfg_scale": {
                    "description": "Classifier-free guidance scale, e.g. 1.0 for turbo models and 7.5 for standard models.",
                    "type": "number"
                  },
                  "flow_shift": {"type": "number"},
                  "height": {"description": "Default image height in pixels.", "type": "integer"},
                  "sampling_method": {"type": "string"},
                  "steps": {
                    "description": "Inference steps, e.g. 4 for turbo models and 20 for standard models.",
                    "type": "integer"
                  },
                  "width": {"description": "Default image width in pixels.", "type": "integer"}
                }
              },
              "labels": {
                "description": "Capabilities and characteristics; see Model Labels.",
                "type": "array",
                "items": {"type": "string"}
              },
              "max_completion_tokens": {
                "description": "OpenAI's name for max_output_tokens.",
                "type": "integer"
              },
              "max_context_window": {
                "description": "Largest context the model supports, from local metadata. Set for downloaded GGUF models and installed FLM models.",
                "type": "integer"
              },
              "max_output_tokens": {
                "description": "Cloud models only: the most tokens one completion can generate, when the provider reports it.",
                "type": "integer"
              },
              "models": {
                "description": "Collections only: each component's full model object, parallel to components, so an exported collection imports through /v1/pull.",
                "type": "array",
                "items": {"type": "object"}
              },
              "object": {"const": "model"},
              "owned_by": {"const": "lemonade"},
              "recipe": {
                "description": "Backend recipe that loads the model, such as llamacpp, flm or ryzenai-llm.",
                "type": "string"
              },
              "recipe_options": {
                "description": "Options saved for this model in recipe_options.json.",
                "type": "object"
              },
              "registry_source": {
                "description": "Remote registry the checkpoint downloads from: huggingface or modelscope.",
                "type": "string"
              },
              "routing": {
                "description": "Router collections only: the routing policy.",
                "type": "object"
              },
              "size": {"description": "Model size in GB, when known.", "type": "number"},
              "source": {
                "description": "Where the model came from: a registry name, local_upload, local_path or extra_models_dir.",
                "type": "string"
              },
              "suggested": {
                "description": "Whether the model is recommended for general use.",
                "type": "boolean"
              },
              "system_prompt": {
                "description": "Omni collections only: the collection's system prompt override.",
                "type": "string"
              },
              "update_available": {
                "description": "Whether a newer upstream commit exists. Set only for downloaded registry-backed models.",
                "type": "boolean"
              },
              "version": {"description": "Router collections only: the policy document's version."}
            }
          }
        },
        "object": {"const": "list"}
      }
    }
    ```
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
| `experimental` | Not yet validated for production use. |

<!-- BEGIN GENERATED: openai.models_id_files -->
## `GET /v1/models/{id}/files`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lemonade extension: lists the files one model resolves to on disk, for model-detail UIs such as a Files tab. It is per-model inventory, not drive storage accounting.

Absolute paths are left out by default, since they can reveal local usernames and the cache layout. Trusted local clients that need them for native UI actions can ask with `?include_paths=true`.

An unknown model answers `404` with an `error` object whose `code` is `model_not_found`.

Also served at `/api/v0/models/{id}/files`, `/api/v1/models/{id}/files` and `/v0/models/{id}/files`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `include_paths` (query) | No | `true` adds each file's absolute `path`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/files
    ```

=== "Response"

    `200`

    ```json
    {
      "files": [
        {"exists": true, "name": "Qwen3-0.6B-Q4_0.gguf", "role": "main", "size_bytes": 382156480}
      ],
      "model_id": "Qwen3-0.6B-GGUF"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model_id", "files"],
      "properties": {
        "files": {
          "description": "Resolved model files known to the registry.",
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name", "role", "size_bytes", "exists"],
            "properties": {
              "exists": {
                "description": "Whether the resolved path currently exists on disk.",
                "type": "boolean"
              },
              "name": {"description": "Base filename from the resolved path.", "type": "string"},
              "path": {
                "description": "Absolute resolved path on the local system. Only with include_paths=true; privacy-sensitive.",
                "type": "string"
              },
              "role": {
                "description": "Checkpoint role, for example main, mmproj, or another recipe-specific role.",
                "type": "string"
              },
              "size_bytes": {
                "description": "File size in bytes. Directories are summed recursively; missing files report 0.",
                "type": "integer"
              }
            }
          }
        },
        "model_id": {"description": "Public model id of the requested model.", "type": "string"}
      }
    }
    ```
<!-- END GENERATED: openai.models_id_files -->

<!-- BEGIN GENERATED: openai.models_id_options_get -->
## `GET /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lemonade extension: reads a model's recipe options, separated by layer, without loading it. With `POST` and `DELETE` on the same path, this manages per-model options independently of [`/v1/load`](./lemonade.md#post-v1load).

`effective` is the exact request body a [`POST /v1/load`](./lemonade.md#post-v1load) for this model uses right now, with every option the recipe accepts resolved through the full priority chain. `defaults` is what a reset model would get.

Per-architecture defaults come from the model's GGUF metadata. For a model that has not been downloaded yet, every key is still present but carries the value it has before those defaults apply.

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-4B-GGUF/options"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-4B-GGUF/options
    ```

=== "Response"

    `200`

    ```json
    {
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
      "model_name": "Qwen3-4B-GGUF",
      "recipe": "llamacpp",
      "resolved_ctx_size": 8192,
      "saved": {"ctx_size": 8192}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model_name", "recipe", "saved", "effective", "defaults", "resolved_ctx_size"],
      "properties": {
        "defaults": {
          "description": "What effective becomes if saved is erased, in the same shape. A ctx_size of -1 means the server picks the context size automatically.",
          "type": "object"
        },
        "effective": {
          "description": "The exact /v1/load body for this model right now, with every option the recipe accepts resolved through the priority chain. Posting it back whole saves every resolved value as an override, so send only the options the user changed.",
          "type": "object"
        },
        "model_name": {
          "description": "The id from the URL. It appears again inside effective and defaults, so each is a complete /v1/load body.",
          "type": "string"
        },
        "recipe": {"description": "The recipe the option names belong to.", "type": "string"},
        "resolved_ctx_size": {
          "description": "The context size a load right now would use: the effective ctx_size, or the automatically computed size when that is -1.",
          "type": "integer"
        },
        "saved": {
          "description": "The model's own entry in recipe_options.json: only what was explicitly saved, or {} when nothing is. It can hold keys this endpoint does not accept, such as pinned written by /v1/load, so replay effective rather than saved.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: openai.models_id_options_get -->

<!-- BEGIN GENERATED: openai.models_id_options_post -->
## `POST /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lemonade extension: saves recipe options for a model without loading it. The body is a flat object of the recipe options [`/v1/load`](./lemonade.md#post-v1load) accepts, and the response is the same as [`GET /v1/models/{id}/options`](#get-v1modelsidoptions), after the write.

The request merges into the model's saved entry, so options it does not mention keep their saved values. `null` removes an option, and the model falls back to the next layer of the [priority chain](./lemonade.md#post-v1load). [`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.

A `400` reports an unrecognized option name, an option from a different recipe, a value of the wrong type, or an invalid `ctx_size`, and nothing from that request is saved.

Saving never loads or reloads the model, so a model that is already running keeps its current options until it is next loaded.

`pinned` is not settable here and is omitted from `effective` and `defaults`. It belongs to [`/v1/load`](./lemonade.md#post-v1load) and [`/internal/pin`](./internal.md#post-internalpin).

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | A positive whole number, or `-1` to pin the model to automatic sizing even when the server-wide `ctx_size` is a specific number. Any other option the model's recipe accepts is set the same way. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | `true` validates and resolves the request identically but persists nothing: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | No | Ignored; the URL names the model. Accepted so `effective` can be posted back as is. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
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
      "model_name": "Qwen3-4B-GGUF",
      "recipe": "llamacpp",
      "resolved_ctx_size": 8192,
      "saved": {"ctx_size": 8192}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model_name", "recipe", "saved", "effective", "defaults", "resolved_ctx_size"],
      "properties": {
        "defaults": {
          "description": "What effective becomes if saved is erased, in the same shape. A ctx_size of -1 means the server picks the context size automatically.",
          "type": "object"
        },
        "effective": {
          "description": "The exact /v1/load body for this model right now, with every option the recipe accepts resolved through the priority chain. Posting it back whole saves every resolved value as an override, so send only the options the user changed.",
          "type": "object"
        },
        "model_name": {
          "description": "The id from the URL. It appears again inside effective and defaults, so each is a complete /v1/load body.",
          "type": "string"
        },
        "recipe": {"description": "The recipe the option names belong to.", "type": "string"},
        "resolved_ctx_size": {
          "description": "The context size a load right now would use: the effective ctx_size, or the automatically computed size when that is -1.",
          "type": "integer"
        },
        "saved": {
          "description": "The model's own entry in recipe_options.json: only what was explicitly saved, or {} when nothing is. It can hold keys this endpoint does not accept, such as pinned written by /v1/load, so replay effective rather than saved.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: openai.models_id_options_post -->

To save automatic context sizing for a model, post `-1`. It is saved even when the server-wide `ctx_size` is an explicit number, and wins over it:

```bash
curl -X POST http://localhost:13305/v1/models/Qwen3-4B-GGUF/options \
  -H "Content-Type: application/json" \
  -d '{"ctx_size": -1}'
```

<!-- BEGIN GENERATED: openai.models_id_options_delete -->
## `DELETE /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lemonade extension: resets a model to its defaults by erasing its `recipe_options.json` entry. The response is the same as [`GET /v1/models/{id}/options`](#get-v1modelsidoptions), with `saved` now `{}`.

The model keeps the defaults that come from its registry entry and from the server's global configuration; only the saved overrides are removed.

Also served at `/api/v0/models/{id}/options`, `/api/v1/models/{id}/options` and `/v0/models/{id}/options`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
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
      "model_name": "Qwen3-4B-GGUF",
      "recipe": "llamacpp",
      "resolved_ctx_size": 32768,
      "saved": {}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model_name", "recipe", "saved", "effective", "defaults", "resolved_ctx_size"],
      "properties": {
        "defaults": {
          "description": "What effective becomes if saved is erased, in the same shape. A ctx_size of -1 means the server picks the context size automatically.",
          "type": "object"
        },
        "effective": {
          "description": "The exact /v1/load body for this model right now, with every option the recipe accepts resolved through the priority chain. Posting it back whole saves every resolved value as an override, so send only the options the user changed.",
          "type": "object"
        },
        "model_name": {
          "description": "The id from the URL. It appears again inside effective and defaults, so each is a complete /v1/load body.",
          "type": "string"
        },
        "recipe": {"description": "The recipe the option names belong to.", "type": "string"},
        "resolved_ctx_size": {
          "description": "The context size a load right now would use: the effective ctx_size, or the automatically computed size when that is -1.",
          "type": "integer"
        },
        "saved": {
          "description": "The model's own entry in recipe_options.json: only what was explicitly saved, or {} when nothing is. It can hold keys this endpoint does not accept, such as pinned written by /v1/load, so replay effective rather than saved.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: openai.models_id_options_delete -->

<!-- BEGIN GENERATED: openai.models_id -->
## `GET /v1/models/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns one model, in the same shape as the entries of [`GET /v1/models`](#get-v1models).

An Omni collection (`recipe: "collection.omni"`) additionally carries `components` (its ordered component names) and `models` (each component's full model object), so its response is a complete collection file; see [Share a Collection](../guide/configuration/custom-models.md#share-a-collection-between-machines).

An unknown model answers `404` with an `error` object whose `code` is `model_not_found`, or `model_not_supported` for a model this system cannot run.

Also served at `/api/v0/models/{id}`, `/api/v1/models/{id}` and `/v0/models/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias. See the [model list](https://lemonade-server.ai/models.html). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/models/Qwen3-0.6B-GGUF"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF
    ```

=== "Response"

    `200`

    ```json
    {
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "checkpoints": {"main": "unsloth/Qwen3-0.6B-GGUF:Q4_0"},
      "components": [],
      "context_length": 40960,
      "created": 1234567890,
      "downloaded": true,
      "id": "Qwen3-0.6B-GGUF",
      "labels": ["chat", "reasoning", "tool-calling"],
      "max_context_window": 40960,
      "object": "model",
      "owned_by": "lemonade",
      "recipe": "llamacpp",
      "recipe_options": {},
      "registry_source": "huggingface",
      "size": 0.356,
      "source": "huggingface",
      "suggested": true,
      "update_available": false
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": [
        "id",
        "object",
        "created",
        "owned_by",
        "checkpoint",
        "checkpoints",
        "recipe",
        "downloaded",
        "update_available",
        "suggested",
        "source",
        "registry_source",
        "labels",
        "components",
        "recipe_options"
      ],
      "properties": {
        "audio_defaults": {
          "description": "Audio models only: default generation parameters.",
          "type": "object"
        },
        "checkpoint": {
          "description": "Main checkpoint: a Hugging Face or ModelScope repository and file, or a local path.",
          "type": "string"
        },
        "checkpoints": {
          "description": "Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.",
          "type": "object",
          "additionalProperties": {"type": "string"}
        },
        "cloud_provider": {
          "description": "Cloud models only: the provider that serves the model.",
          "type": "string"
        },
        "components": {
          "description": "Collections only: ordered component model names. Empty for other models.",
          "type": "array",
          "items": {"type": "string"}
        },
        "context_length": {
          "description": "Tokens one request can use: the loaded value when the model is running, else the configured ctx_size or the cloud provider's reported length.",
          "type": "integer"
        },
        "cost_input_per_million": {
          "description": "Cloud models only: USD per million input tokens, when the provider reports it.",
          "type": "number"
        },
        "cost_output_per_million": {
          "description": "Cloud models only: USD per million output tokens, when the provider reports it.",
          "type": "number"
        },
        "created": {
          "description": "Unix timestamp of when the model entry was created.",
          "type": "integer"
        },
        "downloaded": {"description": "Whether the model's files are on disk.", "type": "boolean"},
        "id": {
          "description": "Model identifier, used for loading and inference requests.",
          "type": "string"
        },
        "image_defaults": {
          "description": "Image models only: default generation parameters.",
          "type": "object",
          "properties": {
            "cfg_scale": {
              "description": "Classifier-free guidance scale, e.g. 1.0 for turbo models and 7.5 for standard models.",
              "type": "number"
            },
            "flow_shift": {"type": "number"},
            "height": {"description": "Default image height in pixels.", "type": "integer"},
            "sampling_method": {"type": "string"},
            "steps": {
              "description": "Inference steps, e.g. 4 for turbo models and 20 for standard models.",
              "type": "integer"
            },
            "width": {"description": "Default image width in pixels.", "type": "integer"}
          }
        },
        "labels": {
          "description": "Capabilities and characteristics; see Model Labels.",
          "type": "array",
          "items": {"type": "string"}
        },
        "max_completion_tokens": {
          "description": "OpenAI's name for max_output_tokens.",
          "type": "integer"
        },
        "max_context_window": {
          "description": "Largest context the model supports, from local metadata. Set for downloaded GGUF models and installed FLM models.",
          "type": "integer"
        },
        "max_output_tokens": {
          "description": "Cloud models only: the most tokens one completion can generate, when the provider reports it.",
          "type": "integer"
        },
        "models": {
          "description": "Collections only: each component's full model object, parallel to components, so an exported collection imports through /v1/pull.",
          "type": "array",
          "items": {"type": "object"}
        },
        "object": {"const": "model"},
        "owned_by": {"const": "lemonade"},
        "recipe": {
          "description": "Backend recipe that loads the model, such as llamacpp, flm or ryzenai-llm.",
          "type": "string"
        },
        "recipe_options": {
          "description": "Options saved for this model in recipe_options.json.",
          "type": "object"
        },
        "registry_source": {
          "description": "Remote registry the checkpoint downloads from: huggingface or modelscope.",
          "type": "string"
        },
        "routing": {"description": "Router collections only: the routing policy.", "type": "object"},
        "size": {"description": "Model size in GB, when known.", "type": "number"},
        "source": {
          "description": "Where the model came from: a registry name, local_upload, local_path or extra_models_dir.",
          "type": "string"
        },
        "suggested": {
          "description": "Whether the model is recommended for general use.",
          "type": "boolean"
        },
        "system_prompt": {
          "description": "Omni collections only: the collection's system prompt override.",
          "type": "string"
        },
        "update_available": {
          "description": "Whether a newer upstream commit exists. Set only for downloaded registry-backed models.",
          "type": "boolean"
        },
        "version": {"description": "Router collections only: the policy document's version."}
      }
    }
    ```
<!-- END GENERATED: openai.models_id -->
