# Anthropic-Compatible API

Lemonade supports an initial Anthropic Messages compatibility endpoint for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |

Current scope focuses on message generation parity for common fields (`model`, `messages`, `system`, `max_tokens`, `temperature`, `stream`, and basic `tools`). Unsupported or unimplemented Anthropic-specific fields are ignored and surfaced via warning logs/headers.

Those limits apply to models Lemonade serves by converting to and from the OpenAI shape — every local model, and cloud providers registered with the default `openai` wire format. A cloud provider registered with `--wire-format anthropic` is instead relayed byte-for-byte to its `/messages` endpoint, so no field is dropped and nothing is converted. See [Cloud Offload](../guide/configuration/cloud.md#providers-that-speak-the-anthropic-messages-format).
