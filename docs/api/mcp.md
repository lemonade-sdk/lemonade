# MCP Gateway

Lemonade exposes its inference capabilities as a Model Context Protocol (MCP) server, so any MCP-compatible client (GitHub Copilot, Claude Desktop, MCP Inspector, Cursor, the `mcp` Python client, etc.) can call your locally running models as tools.

The gateway implements the **MCP "Streamable HTTP" transport** (spec version `2025-06-18`) with the `tools` capability only. All traffic flows through a single endpoint:

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /mcp` | Supported | JSON-RPC 2.0 envelope. Accepts a single message or a batch array. |
| `GET /mcp` | `405 Method Not Allowed` | Server-initiated SSE channel is not supported. |

> **Why a single path?** The MCP specification mandates one endpoint URL per server, so `/mcp` is an intentional exception to Lemonade's quad-prefix convention.

## Authentication

`/mcp` is treated as a regular API route, so it honors `LEMONADE_API_KEY` exactly like `/api/v1/chat/completions`:

```bash
curl -s http://localhost:13305/mcp \
    -H "Authorization: Bearer $LEMONADE_API_KEY" \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

## Supported methods

| Method | Purpose |
|--------|---------|
| `initialize` | Negotiate protocol version, return server identity and capabilities. |
| `notifications/initialized` | Client acknowledgement; silently accepted. |
| `tools/list` | Return the catalogue of callable tools (with JSON Schemas). |
| `tools/call` | Invoke one of the tools below. |
| `ping` | Liveness probe; returns `{}`. |

## Tools

<!--
The tool reference below is generated from a live Lemonade server's `tools/list`
response. Tool names, descriptions, schemas, and MCP annotations therefore have a
single source of truth in the server registry instead of being copied into the docs.
-->

<!-- BEGIN GENERATED: mcp-tools -->
<!-- Generated from live /mcp tools/list. Do not edit this region by hand. -->

### `lemonade_list_models`

List models known to the Lemonade server. ALWAYS call this first if you don't already know the exact model name to use for chat/transcribe/image — passing a wrong name may trigger a multi-GB download. Returns a summary text block plus a JSON block with `{loaded, available, suggested_to_pull, recommended_chat_model}`.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `include_available` | no | `{"type":"boolean"}` | Include downloaded models in the `available` list. |
| `include_suggested` | no | `{"type":"boolean"}` | Include suggested, not-yet-downloaded models in `suggested_to_pull`. |

### `lemonade_chat`

Chat completion against a locally hosted LLM. Pass a `messages` array (OpenAI chat format). `model` is OPTIONAL: when omitted, the server reuses an already-loaded LLM, else an already-downloaded one; if neither exists it asks you to either pass a `model` or `allow_download: true` (which downloads the default, Qwen3.5-4B-MTP-GGUF). Call `lemonade_list_models` first if you want to choose explicitly — a wrong name may trigger a multi-GB download.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default model when none is loaded or downloaded. Defaults to false. |
| `chat_template_kwargs` | no | `{"type":"object"}` | e.g. {"enable_thinking": true} to enable reasoning blocks; disabled by default |
| `max_tokens` | no | `{"type":"integer"}` | Maximum number of output tokens. |
| `messages` | yes | `{"items":{"type":"object"},"type":"array"}` | Conversation messages in OpenAI chat format. |
| `model` | no | `{"type":"string"}` | Optional. Omit to auto-select a loaded/downloaded LLM; defaults to Qwen3.5-4B-MTP-GGUF only with allow_download=true. |
| `response_format` | no | `{"type":"object"}` | OpenAI-compatible response format configuration. |
| `seed` | no | `{"type":"integer"}` | Optional random seed for reproducible sampling. |
| `stop` | no | `{}` | stop sequences (string or array) |
| `temperature` | no | `{"type":"number"}` | Sampling temperature. |
| `tool_choice` | no | `{}` | auto \| none \| required \| {type: function, ...} |
| `tools` | no | `{"items":{"type":"object"},"type":"array"}` | OpenAI-compatible tool definitions available to the model. |
| `top_p` | no | `{"type":"number"}` | Nucleus sampling probability. |

### `lemonade_transcribe_audio`

Transcribe an audio clip with a Whisper-class model. The Lemonade MCP server always runs on the same machine as the caller, so prefer `audio_path` (an absolute path to a local audio file: wav, mp3, m4a, ogg, flac, webm). Use `audio_base64` only when you genuinely have audio bytes in memory. Exactly one of the two must be provided. `model` is OPTIONAL: when omitted, the server reuses an already-loaded transcription model, else an already-downloaded one; if neither exists it asks you to pass a `model` or `allow_download: true` (which downloads the default, Whisper-Tiny).

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default model when none is loaded or downloaded. Defaults to false. |
| `audio_base64` | no | `{"type":"string"}` | Base64-encoded audio data. Use instead of `audio_path`. |
| `audio_path` | no | `{"type":"string"}` | Absolute path to a local audio file. Preferred over audio_base64. |
| `filename` | no | `{"type":"string"}` | Original audio filename, used as an input-format hint. |
| `language` | no | `{"type":"string"}` | Optional source language hint. |
| `model` | no | `{"type":"string"}` | Optional. Omit to auto-select a loaded/downloaded model; defaults to Whisper-Tiny only with allow_download=true. |
| `prompt` | no | `{"type":"string"}` | Optional text prompt to guide transcription. |
| `response_format` | no | `{"enum":["json","text","srt","verbose_json","vtt"],"type":"string"}` | Transcription response format. |
| `temperature` | no | `{"type":"number"}` | Sampling temperature for transcription. |

### `lemonade_generate_image`

Generate one or more images from a text prompt. The Lemonade MCP server always runs on the same machine as the caller, so PREFER writing the result directly to disk by passing `output_path` (single image) or `output_dir` (one or more). When you do, the tool returns absolute file path(s) as text — no base64 round-trip and dramatically fewer tokens. Only omit both arguments when you genuinely need the image inline. For safety, disk writes are confined to a sandbox directory (<cache_dir>/mcp-images, or LEMONADE_MCP_IMAGE_DIR if set); paths outside it are rejected. `output_dir` writes get unique auto-generated filenames (so concurrent callers never clobber each other); use `output_path` when you need an exact name. `model` is OPTIONAL: when omitted, the server reuses an already-loaded image model, else an already-downloaded one; if neither exists it asks you to pass a `model` or `allow_download: true` (which downloads the default, SD-Turbo).

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default model when none is loaded or downloaded. Defaults to false. |
| `cfg_scale` | no | `{"type":"number"}` | Classifier-free guidance scale. |
| `model` | no | `{"type":"string"}` | Optional. Omit to auto-select a loaded/downloaded image model; defaults to SD-Turbo only with allow_download=true. |
| `n` | no | `{"minimum":1,"type":"integer"}` | Number of images to generate. |
| `negative_prompt` | no | `{"type":"string"}` | Text describing content to avoid in the generated image. |
| `output_dir` | no | `{"type":"string"}` | Directory to write generated images into, inside the MCP image sandbox. Filenames are auto-generated and unique (image_<token>_<i>.png); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it. |
| `output_path` | no | `{"type":"string"}` | Exact path of the PNG file to write, inside the MCP image sandbox (<cache_dir>/mcp-images or LEMONADE_MCP_IMAGE_DIR). Relative paths resolve against the sandbox root; absolute paths must stay within it. Written as named (overwrites if it already exists). Only valid when n == 1. |
| `prompt` | yes | `{"type":"string"}` | Text prompt describing the image to generate. |
| `seed` | no | `{"type":"integer"}` | Optional random seed for reproducible generation. |
| `size` | no | `{"type":"string"}` | Requested image size as WIDTHxHEIGHT, for example 512x512. |
| `steps` | no | `{"type":"integer"}` | Number of diffusion sampling steps. |

### `lemonade_omni`

Multimodal turn against a Lemonade Omni collection (one tool call -> text + images + speech in the same response). The server runs an internal tool-calling loop against the collection's planner LLM and executes its image / image-edit / TTS tools by routing to the bundled component models; the result comes back as a text block plus native MCP `image` / `audio` content blocks (one per artifact). `model` is OPTIONAL: when omitted, the server reuses an already-downloaded Omni collection; if none is downloaded it asks you to pass a `model` or `allow_download: true` (which downloads the default, `LMX-Omni-5.5B-Lite`). Pass `model='LMX-Omni-52B-Halo'` (or any other recipe='collection.omni' model from `lemonade_list_models`) to opt into a larger collection; that model may be multi-GB. Same-machine deployment: PREFER `output_dir` to write artifacts to disk and avoid expensive inline base64 blobs. For plain text chat against a regular LLM, use `lemonade_chat` instead.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default collection when none is downloaded. Defaults to false. |
| `chat_template_kwargs` | no | `{"type":"object"}` | Additional chat-template arguments forwarded to the planner LLM. |
| `max_tokens` | no | `{"type":"integer"}` | Maximum number of planner output tokens. |
| `messages` | yes | `{"items":{"type":"object"},"type":"array"}` | Conversation messages in OpenAI chat format. |
| `model` | no | `{"type":"string"}` | Optional. Omni collection name (recipe='collection.omni'). Omit to reuse a downloaded collection; defaults to LMX-Omni-5.5B-Lite only with allow_download=true. |
| `output_dir` | no | `{"type":"string"}` | Directory to write produced artifacts into, inside the MCP image sandbox (<cache_dir>/mcp-images or LEMONADE_MCP_IMAGE_DIR). Filenames are auto-generated and unique (omni_<token>_<i>.<ext>); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it. PREFER this to inline base64 when caller and server share a filesystem. Omit to receive artifacts inline as MCP content blocks. |
| `response_format` | no | `{"type":"object"}` | OpenAI-compatible response format configuration for the planner. |
| `seed` | no | `{"type":"integer"}` | Optional random seed forwarded to the planner LLM. |
| `stop` | no | `{}` | stop sequences (string or array) |
| `temperature` | no | `{"type":"number"}` | Sampling temperature for the planner LLM. |
| `tool_choice` | no | `{}` | auto \| none \| required \| {type: function, ...} |
| `tools` | no | `{"items":{"type":"object"},"type":"array"}` | OpenAI-compatible application tool definitions passed to the planner. |
| `top_p` | no | `{"type":"number"}` | Nucleus sampling probability for the planner LLM. |

### `lemonade_docs`

Read this server's own API reference. Call with no arguments to list the pages it ships, then pass `page` (an `id` from that list) to read one as markdown. The docs are bundled with the server, so they describe the version actually running and work offline. Use this before hand-writing requests against Lemonade endpoints.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `page` | no | `{"type":"string"}` | Optional. Page id from the listing, e.g. 'api/lemonade'. Omit to list. |

<!-- END GENERATED: mcp-tools -->

## Error model

| Code | Meaning |
|------|---------|
| `-32700` | Body was not valid JSON. |
| `-32600` | Request was not a JSON-RPC object (or batch was empty / missing `method`). |
| `-32601` | Unknown JSON-RPC method (e.g. `resources/list`). |
| `-32602` | Invalid `params` for a known method. |
| `-32603` | Internal server error (an exception escaped a handler). |

Tool-level failures (bad arguments, model load errors, backend exceptions) are returned as **successful** JSON-RPC results with `"isError": true` and a text content block describing the failure, so MCP-aware models can self-correct.

## Limitations (MVP)

- No server-initiated SSE (GET /mcp returns 405). Tools return their full result in the POST response.
- No session resumption (`Mcp-Session-Id` header is not issued).
- `resources/*` and `prompts/*` capabilities are not implemented.
- Streaming chat output is not exposed via MCP — `stream=true` is ignored. Use `POST /v1/chat/completions` directly for streamed tokens.
- Embeddings and text-to-speech are not currently exposed as MCP tools; use the OpenAI-compatible endpoints (`/v1/embeddings`, `/v1/audio/speech`) for those.

## Quick test with curl

```bash
# 1. Initialize
curl -s http://localhost:13305/mcp -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}'

# 2. List tools
curl -s http://localhost:13305/mcp -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

# 3. Call lemonade_chat
curl -s http://localhost:13305/mcp -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"lemonade_chat","arguments":{"model":"Qwen3-1.7B-GGUF","messages":[{"role":"user","content":"hi"}],"max_tokens":16}}}'
```
