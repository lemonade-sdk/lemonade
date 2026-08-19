# Anthropic-Compatible API

Lemonade supports an initial Anthropic Messages compatibility endpoint for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |

Current scope focuses on message generation parity for common fields (`model`, `messages`, `system`, `max_tokens`, `temperature`, `stream`, and basic `tools`). Unsupported or unimplemented Anthropic-specific fields are ignored and surfaced via warning logs/headers.

Those limits apply only where Lemonade converts to and from the OpenAI shape. A cloud provider registered with `--wire-format anthropic` is relayed unconverted, so no field is dropped — see [Cloud Offload](../guide/configuration/cloud.md#providers-that-speak-the-anthropic-messages-format).
