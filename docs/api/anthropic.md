# Anthropic-Compatible API

Lemonade serves the Anthropic Messages API for applications that call Claude-style APIs.

<!-- BEGIN GENERATED: anthropic.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/messages`](#post-v1messages) | Generate the next assistant message |
<!-- END GENERATED: anthropic.summary -->

<!-- BEGIN GENERATED: anthropic.messages -->
## `POST /v1/messages`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation in the Anthropic Messages format.

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

Lemonade runs the request as an OpenAI chat completion and converts both ways. Some values it cannot convert, such as an unsupported content block type or `metadata`, are ignored and reported in the `X-Lemonade-Warning` header (joined with ` | `) and in a `warnings` array. Unknown fields are ignored without a warning. A cloud provider registered with `--wire-format anthropic` is relayed unconverted, so nothing is dropped; see [Cloud Offload](../guide/configuration/cloud.md#providers-that-speak-the-anthropic-messages-format).

Errors use Anthropic's shape, `{"type": "error", "error": {"type", "message"}}`. A model that fails to load, whether unknown or not, answers `404` with a `not_found_error`, except a load blocked by a `collection.router` residency constraint, which answers `409`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/messages" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "max_tokens": 64,
          "temperature": 0,
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}]
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/messages \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "max_tokens": 64,
          "temperature": 0,
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}]
        }'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/messages" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "max_tokens": 64,
          "temperature": 0,
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": true
        }'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/messages \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "max_tokens": 64,
          "temperature": 0,
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": true
        }'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "id": "chatcmpl-uhdmXVdTjsgmRNikJFqvtD007BhH22p1",
      "type": "message",
      "role": "assistant",
      "model": "Qwen3-0.6B-GGUF",
      "content": [{"type": "text", "text": "The capital of France is **Paris**."}],
      "stop_reason": "end_turn",
      "stop_sequence": null,
      "usage": {"input_tokens": 19, "output_tokens": 14}
    }
    ```

    **Field Descriptions:**

    - `id` - The message's ID.
    - `type` - Always `message`.
    - `role` - Always `assistant`.
    - `model` - The model that generated the message.
    - `content` - The message's content blocks.
      - `type` - `text` or `tool_use`. A relayed provider can add its own block types, such as `thinking`.
      - `text` - In a `text` block: the text.
      - `id` - In a `tool_use` block: the tool call's ID.
      - `name` - In a `tool_use` block: the tool's name.
      - `input` - In a `tool_use` block: the call's arguments.
    - `stop_reason` - `end_turn`, `max_tokens` or `tool_use`.
    - `stop_sequence` - The stop sequence that ended the message; always `null` for local models.
    - `usage` - Token counts.
      - `input_tokens` - Prompt tokens.
      - `output_tokens` - Generated tokens.
    - `warnings` - What Lemonade ignored or changed while converting; present only when there is something to report.

=== "Streaming responses"

    `200`

    ```text
    event: message_start
    data: {"type": "message_start", "message": {"content": [], "id": "chatcmpl-W8xmVYifwOmFLduA4zOyS4skhoHrbmJi", "model": "Qwen3-0.6B-GGUF", "role": "assistant", "stop_reason": null, "stop_sequence": null, "type": "message", "usage": {"input_tokens": 0, "output_tokens": 0}}}
    event: content_block_start
    data: {"type": "content_block_start", "index": 0, "content_block": {"text": "", "type": "text"}}
    event: content_block_delta
    data: {"type": "content_block_delta", "index": 0, "delta": {"text": "The", "type": "text_delta"}}
    ...
    event: message_delta
    data: {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": null}, "usage": {"input_tokens": 0, "output_tokens": 0}}
    event: message_stop
    data: {"type": "message_stop"}
    ```

    **Field Descriptions:**

    - `type` - Matches the event's name: `message_start`, `content_block_start`, `content_block_delta`, `content_block_stop`, `message_delta`, `message_stop` or `error`. A relayed provider can also send `ping`.
    - `message` - In `message_start`: the message, with empty `content` and zero `usage`.
    - `index` - In `content_block_start`, `content_block_delta` and `content_block_stop`: the block's position in the message's `content`.
    - `content_block` - In `content_block_start`: the block, with empty `text` or `input`.
    - `delta` - In `content_block_delta`: the block's next `text_delta` text or `input_json_delta` partial JSON. In `message_delta`: the `stop_reason` and `stop_sequence`.
    - `usage` - In `message_delta`: token counts. Both are `0` when the backend's stream carries no usage, as with llama.cpp.
    - `warnings` - In `message_delta`: what Lemonade ignored while converting; present only when there is something to report.
<!-- END GENERATED: anthropic.messages -->
