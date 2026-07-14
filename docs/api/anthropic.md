# Anthropic-Compatible API

Lemonade provides Anthropic Messages compatibility for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |
| `POST /v1/messages/count_tokens` | Supported for llama.cpp models | Applies the model chat template and tokenizer without running generation. |

The adapter supports text and image content, system prompts, sampling and stop controls, streaming, structured tool use and tool results, and local-model thinking output. Tool definitions are converted to the OpenAI-compatible format used by Lemonade backends. Structured backend `tool_calls` are converted back to Anthropic `tool_use` blocks; raw model text that resembles a tool call is not reparsed by this adapter, so the selected model and backend chat template must support tool calling.

`stop_sequence` is populated when the backend identifies the matched stop string; otherwise it remains `null`.

Requests with malformed required fields or unsupported content-block types return `invalid_request_error`. Lossy optional compatibility behavior is reported through `X-Lemonade-Warning` and server logs rather than non-standard fields in successful response bodies.

Local models cannot create or verify Anthropic's cryptographic thinking signatures. Lemonade emits empty signatures for local thinking blocks, accepts empty signatures on subsequent turns, and rejects signed or `redacted_thinking` blocks from remote Anthropic responses instead of silently discarding them. `thinking.type: adaptive` enables the backend's local thinking mode, but model-specific adaptive behavior is not emulated.

Token counting uses the same converted messages, tools, and model chat template as generation. A model must be loaded to access its tokenizer and template, so this endpoint may auto-load the requested model and is unavailable for backends that do not implement chat token counting.

Streaming uses chat token counting when the backend supports it so `message_start.usage.input_tokens` is accurate before generation deltas are emitted. Other backends report `input_tokens: 0` and return an `X-Lemonade-Warning` instead of rejecting the stream.

When `LEMONADE_API_KEY` is set, Anthropic clients may authenticate with either `Authorization: Bearer ...` or `X-Api-Key`.
