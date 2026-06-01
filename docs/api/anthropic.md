# Anthropic-Compatible API

Lemonade supports an Anthropic Messages compatibility endpoint for applications that call Claude-style APIs. Use this to point an existing Anthropic SDK client at a locally-running model without changing your code.

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/messages`](#post-v1messages) | Generate a message response, streaming or non-streaming |

Current scope covers message generation parity for the most common fields. Unsupported Anthropic-specific fields are silently ignored and surfaced via warning log entries.

## `POST /v1/messages`
<sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Generate a response given a list of messages. Mirrors the [Anthropic Messages API](https://docs.anthropic.com/en/api/messages).

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The Lemonade model name to use (e.g. `Qwen3-0.6B-GGUF`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | Yes | Array of message objects. Each must have `role` (`"user"` or `"assistant"`) and `content` (string or content array). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | Yes | Maximum number of tokens to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `system` | No | System prompt string prepended before the conversation. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | Sampling temperature (0.0–1.0). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If `true`, returns Server-Sent Events. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | Array of tool definitions in Anthropic format. Basic tool use is supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop_sequences` | No | Array of strings where generation stops. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_p` | No | Nucleus sampling probability. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Top-k sampling. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `metadata` | No | Ignored. Accepted silently. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `thinking` | No | Ignored. Accepted silently. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

Query parameters such as `?beta=true` are accepted and ignored.

### Example request

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/messages \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Qwen3-0.6B-GGUF",
            "max_tokens": 256,
            "messages": [
              {"role": "user", "content": "What is the capital of France?"}
            ]
          }'
    ```

=== "Python (Anthropic SDK)"

    ```python
    import anthropic

    client = anthropic.Anthropic(
        api_key="lemonade",
        base_url="http://localhost:13305",
    )

    message = client.messages.create(
        model="Qwen3-0.6B-GGUF",
        max_tokens=256,
        messages=[{"role": "user", "content": "What is the capital of France?"}],
    )
    print(message.content[0].text)
    ```

### Response format

```json
{
  "id": "msg_01XFDUDYJgAACzvnptvVoYEL",
  "type": "message",
  "role": "assistant",
  "content": [
    {
      "type": "text",
      "text": "The capital of France is Paris."
    }
  ],
  "model": "Qwen3-0.6B-GGUF",
  "stop_reason": "end_turn",
  "stop_sequence": null,
  "usage": {
    "input_tokens": 14,
    "output_tokens": 9
  }
}
```

### Streaming response

When `stream: true`, the server returns Anthropic-format Server-Sent Events:

```
event: message_start
data: {"type":"message_start","message":{"id":"msg_01","type":"message","role":"assistant","content":[],"model":"Qwen3-0.6B-GGUF","stop_reason":null,"usage":{"input_tokens":14,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"The capital"}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" of France is Paris."}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":9}}

event: message_stop
data: {"type":"message_stop"}
```

### Error responses

| Status | Cause |
|--------|-------|
| 400 | Malformed request body or missing required fields |
| 404 | Model not found |
| 500 | Backend inference error |

Errors are returned as JSON with an `error` object:

```json
{"error": {"type": "invalid_request_error", "message": "model is required"}}
