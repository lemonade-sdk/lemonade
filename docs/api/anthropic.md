# Anthropic-Compatible API

Lemonade supports the Anthropic Messages API for applications that call Claude-style APIs.

<!-- BEGIN GENERATED: anthropic.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/messages`](#post-v1messages) | Messages, streaming and non-streaming |
<!-- END GENERATED: anthropic.summary -->

<!-- BEGIN GENERATED: anthropic.messages -->
## `POST /v1/messages`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation in the Anthropic Messages format, loading the model on first use, for applications that call Claude-style APIs.

Lemonade runs the request as an OpenAI chat completion and converts both ways. Fields it cannot convert are ignored, and each one is reported in the `X-Lemonade-Warning` header (joined with ` | `) and, on a non-streaming response, in a `warnings` array.

A model from a cloud provider registered with `--wire-format anthropic` is relayed to the provider unconverted, so no field is dropped; see [Cloud Offload](../guide/configuration/cloud.md#providers-that-speak-the-anthropic-messages-format).

Errors use Anthropic's shape, `{"type": "error", "error": {"type", "message"}}`. An unknown model answers `404` with a `not_found_error`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | Yes | Conversation so far, as `user` and `assistant` messages. `content` is a string or an array of `text`, `image` (base64 source), `tool_use` and `tool_result` blocks. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `system` | No | System prompt, as a string or an array of `text` blocks. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | Upper bound on generated tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Nucleus sampling probability. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Number of top tokens considered during sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop_sequences` | No | Sequences where generation stops. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream server-sent events as tokens are generated. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | Tools the model may call, each with a `name`, `description` and `input_schema`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tool_choice` | No | `{"type": "auto"}`, `any`, `none`, or `tool` with a `name`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `output_config` | No | `format` of type `json_schema` (with a `schema`) or `json_object` constrains the reply. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `thinking` | No | Not applied to local models: reasoning models think regardless. Qwen3 models skip their reasoning when the prompt ends with `/no_think`. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `metadata` | No | Ignored for local models, with a warning. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `context_management` | No | Ignored for local models, with a warning. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `beta` (query) | No | Accepted for Anthropic SDK compatibility. Values other than `true` add a warning; `true` is passed on to providers that are relayed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/messages" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "max_tokens": 64,
          "temperature": 0
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/messages \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "max_tokens": 64,
          "temperature": 0
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "content": [{"text": "The capital of France is **Paris**.", "type": "text"}],
      "id": "chatcmpl-7nHuJwc1c2OHLcsAfvSnubux6kPMFCPH",
      "model": "Qwen3-0.6B-GGUF",
      "role": "assistant",
      "stop_reason": "end_turn",
      "stop_sequence": null,
      "type": "message",
      "usage": {"input_tokens": 19, "output_tokens": 14}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "type", "role", "model", "content", "stop_reason", "usage"],
      "properties": {
        "content": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["type"],
            "properties": {
              "id": {"type": "string"},
              "input": {"type": "object"},
              "name": {"type": "string"},
              "text": {"type": "string"},
              "type": {
                "description": "text or tool_use. A relayed provider can add its own block types, such as thinking.",
                "type": "string"
              }
            }
          }
        },
        "id": {"type": "string"},
        "model": {"type": "string"},
        "role": {"const": "assistant"},
        "stop_reason": {"description": "end_turn, max_tokens or tool_use.", "type": "string"},
        "stop_sequence": {"type": ["string", "null"]},
        "type": {"const": "message"},
        "usage": {
          "type": "object",
          "properties": {"input_tokens": {"type": "integer"}, "output_tokens": {"type": "integer"}}
        },
        "warnings": {
          "description": "Fields Lemonade ignored while converting.",
          "type": "array",
          "items": {"type": "string"}
        }
      }
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/messages" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "max_tokens": 64,
          "temperature": 0,
          "stream": true
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/messages \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "max_tokens": 64,
          "temperature": 0,
          "stream": true
        }'
    ```

=== "Response"

    `200`

    ```text
    event: message_start
    data: {"message": {"content": [], "id": "chatcmpl-Gj4QYW9KTblYxvvmII9QEEZ1KEp6Vz5e", "model": "Qwen3-0.6B-GGUF", "role": "assistant", "stop_reason": null, "stop_sequence": null, "type": "message", "usage": {"input_tokens": 0, "output_tokens": 0}}, "type": "message_start"}
    event: content_block_start
    data: {"content_block": {"text": "", "type": "text"}, "index": 0, "type": "content_block_start"}
    event: content_block_delta
    data: {"delta": {"text": "The", "type": "text_delta"}, "index": 0, "type": "content_block_delta"}
    ...
    event: message_delta
    data: {"delta": {"stop_reason": "end_turn", "stop_sequence": null}, "type": "message_delta", "usage": {"input_tokens": 0, "output_tokens": 0}}
    event: message_stop
    data: {"type": "message_stop"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["type"],
      "properties": {
        "type": {
          "description": "Matches the event's name: message_start, content_block_start, content_block_delta, content_block_stop, message_delta, message_stop, or error. A relayed provider can also send ping.",
          "type": "string"
        }
      }
    }
    ```
<!-- END GENERATED: anthropic.messages -->
