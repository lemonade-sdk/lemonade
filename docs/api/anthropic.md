# Anthropic-Compatible API

Lemonade supports an initial Anthropic Messages compatibility endpoint for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |
| `POST /v1/messages/count_tokens` | Supported | Uses the loaded model tokenizer over Lemonade's Anthropic-to-chat conversion. |

Current scope focuses on message generation parity for common fields (`model`, `messages`, `system`, `max_tokens`, `temperature`, `stream`, and basic `tools`). Anthropic `thinking` blocks are preserved as reasoning context on input, and model reasoning output is returned as Anthropic `thinking` content blocks or streaming `thinking_delta` events instead of regular text output. Unsupported or unimplemented Anthropic-specific fields are ignored and surfaced via warning logs/headers.

When `LEMONADE_API_KEY` is set, Anthropic clients may authenticate with either `Authorization: Bearer ...` or `X-Api-Key`.
