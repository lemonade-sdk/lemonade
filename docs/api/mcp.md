# MCP Gateway

Lemonade exposes its inference and model-management capabilities as a Model Context Protocol (MCP) server, so any MCP-compatible client (GitHub Copilot, Claude Desktop, MCP Inspector, Cursor, the `mcp` Python client, etc.) can call your locally running models as tools.

The gateway implements the **MCP "Streamable HTTP" transport** (spec version `2025-06-18`) with the `tools` capability only, so `resources/*` and `prompts/*` are not implemented. All traffic flows through a single endpoint:

<!-- BEGIN GENERATED: mcp.summary -->
| Method | Endpoint | Status | Description |
|--------|----------|--------|-------------|
| `POST` | [`/mcp`](#post-mcp) | Supported | Call MCP methods over JSON-RPC 2.0 |
| `GET` | [`/mcp`](#get-mcp) | Not supported | Open a server-initiated event stream |
<!-- END GENERATED: mcp.summary -->

<!-- BEGIN GENERATED: mcp.rpc -->
## `POST /mcp`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Serves the MCP methods listed under [Supported Methods](#supported-methods) over JSON-RPC 2.0.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `jsonrpc` | Yes | JSON-RPC version: `"2.0"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `id` | No | Request id, echoed in the reply. Omit it to send a notification, which gets no reply. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `method` | Yes | One of the [supported methods](#supported-methods), e.g. `initialize`, `tools/list` or `tools/call`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `params` | No | Method parameters; `tools/call` takes `name` and `arguments`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The body is one JSON-RPC message or a batch array of them. A JSON-RPC error is answered with `200` like any other JSON-RPC response; see [Error Model](#error-model). A request whose messages are all notifications answers `202` with no body.

### Supported Methods

| Method | Purpose |
|--------|---------|
| `initialize` | Return the server's protocol version, identity and capabilities. |
| `notifications/initialized` | Client acknowledgement; silently accepted. |
| `notifications/cancelled` | Client cancellation; silently accepted. |
| `tools/list` | Return the catalogue of callable tools (with JSON Schemas). |
| `tools/call` | Invoke one of the tools below. |
| `ping` | Liveness probe; returns `{}`. |

### Error Model

| Code | Meaning |
|------|---------|
| `-32700` | Body was not valid JSON. |
| `-32600` | Request was not a JSON-RPC object (or batch was empty / missing `method`). |
| `-32601` | Unknown JSON-RPC method (e.g. `resources/list`). |
| `-32602` | Invalid `params` for a known method. |
| `-32603` | Internal server error (an exception escaped a handler). |

Tool-level failures (bad arguments, model load errors, backend exceptions) are returned as **successful** JSON-RPC results with `"isError": true` and a text content block describing the failure, so MCP-aware models can self-correct.

### Limitations

- No session resumption (`Mcp-Session-Id` header is not issued).
- Tools return their whole result: `lemonade_chat` ignores `stream`. Use [`POST /v1/chat/completions`](./openai.md#post-v1chatcompletions) for streamed tokens.
- Embeddings have no MCP tool; use [`/v1/embeddings`](./openai.md#post-v1embeddings).

### Examples

#### Request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/mcp" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "jsonrpc": "2.0",
          "id": 1,
          "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {}}
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/mcp \
      -H "Content-Type: application/json" \
      -d '{
          "jsonrpc": "2.0",
          "id": 1,
          "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {}}
        }'
    ```

**Response:** `200 OK`

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "capabilities": {"tools": {}},
    "protocolVersion": "2025-06-18",
    "serverInfo": {"name": "lemonade-mcp", "version": "2026.43.0~1686.1553ede6"}
  }
}
```

#### Notification

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/mcp" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"jsonrpc": "2.0", "method": "notifications/initialized"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/mcp \
      -H "Content-Type: application/json" \
      -d '{"jsonrpc": "2.0", "method": "notifications/initialized"}'
    ```

**Response:** `202 Accepted`

### Response format

One JSON-RPC 2.0 response, or an array of them for a batch.

- `jsonrpc` - Always `2.0`.
- `id` - The `id` of the request this answers; `null` when it could not be read.
- `result` - The method's result.
- `error` - A JSON-RPC error, such as `-32601` for an unknown method.
  - `code` - The JSON-RPC error code.
  - `message` - What went wrong.

### Errors

| Status | Code | When |
|--------|------|------|
| `500` | `-32603` | An exception escapes the JSON-RPC handling, such as a number too large for a double or invalid UTF-8 in the body |
<!-- END GENERATED: mcp.rpc -->

<!-- BEGIN GENERATED: mcp.sse -->
## `GET /mcp`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Lemonade opens no server-initiated event stream, so tools return their full result in the [`POST /mcp`](#post-mcp) reply.

Requires `LEMONADE_API_KEY` when it is set.

### Errors

| Status | Code | When |
|--------|------|------|
| `405` | `-32600` | Always, since MCP calls go to `POST /mcp`; the reply carries `Allow: POST` |
<!-- END GENERATED: mcp.sse -->

## Tools

`lemonade_omni` runs the server's collection orchestrator; see the [Lemonade Omni documentation](../dev/lemonade-omni.md) for the collection model and component behavior.

<!--
The tool reference below is generated from a live Lemonade server's `tools/list`
response. Tool names, descriptions, input schemas, result contracts, and MCP
annotations therefore have a single source of truth in the server registry instead
of being copied into the docs.
-->

<!-- BEGIN GENERATED: mcp-tools -->
<!-- Generated from live /mcp tools/list. Do not edit this region by hand. -->

### `lemonade_get_model_info`

Get the server's detailed record for one exact model ID returned by lemonade_list_models. Returns the same model metadata used by the REST model APIs, including recipe, download state, components, recipe options, and Router metadata when present.

**Returns**

Returns a result from lemonade_get_model_info, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `model_name` | yes | `{"type":"string"}` | Exact model ID from lemonade_list_models. |

### `lemonade_load_model`

Explicitly load a Lemonade model using the same implementation as POST /v1/load. The model_name must be exact. A known registry model that is not local may be downloaded by the existing load path. collection.router models are virtual and are acknowledged without starting a Router backend process. `options` contains ephemeral recipe-specific load options such as llamacpp_backend or llamacpp_device; they are not persisted by this MCP tool.

**Returns**

Returns a result from lemonade_load_model, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": true,
  "readOnlyHint": false
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `ctx_size` | no | `{"type":"integer"}` | Optional context size. Use -1 for automatic sizing. |
| `model_name` | yes | `{"type":"string"}` | Exact model ID from lemonade_list_models. |
| `options` | no | `{"additionalProperties":true,"type":"object"}` | Optional recipe-specific load options. Applied only to this load; model/model_name/save_options/pinned are not allowed here. |
| `pinned` | no | `{"type":"boolean"}` | Optional residency pin state for this load. |

### `lemonade_unload_model`

Unload exactly one named model using the same implementation as POST /v1/unload. model_name is mandatory: this MCP tool never exposes the REST endpoint's empty-name unload-all behavior.

**Returns**

Returns a result from lemonade_unload_model, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": false
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `model_name` | yes | `{"type":"string"}` | Exact model ID to unload. |

### `lemonade_get_loaded_models`

List the models currently loaded in Lemonade, including runtime recipe/type/device information reported by the server. Use this when the question is specifically about current residency; use lemonade_list_models for broader discovery.

**Returns**

Returns a result from lemonade_get_loaded_models, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

This tool takes no arguments.

### `lemonade_get_server_health`

Return Lemonade server health and runtime limits using the same implementation as GET /api/v1/health.

**Returns**

Returns a result from lemonade_get_server_health, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

This tool takes no arguments.

### `lemonade_pull_model`

Download or register a model using the same implementation as POST /v1/pull. For a built-in/registered model, pass its exact model_name. For a custom remote model, pass a user.* model_name plus checkpoint and recipe. This call is synchronous and may take a long time for large model downloads.

**Returns**

Returns a result from lemonade_pull_model, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": true,
  "readOnlyHint": false
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `checkpoint` | no | `{"type":"string"}` | Optional remote repository/checkpoint selector accepted by /pull. |
| `do_not_upgrade` | no | `{"type":"boolean"}` | Skip upgrading an already-cached registered model. |
| `model_name` | yes | `{"type":"string"}` | Exact registered model ID, or user.* ID for a new custom model. |
| `recipe` | no | `{"type":"string"}` | Recipe required when registering a custom checkpoint. |
| `source` | no | `{"type":"string"}` | Optional remote registry source, for example huggingface or modelscope. |

### `lemonade_delete_model`

Delete exactly one model using the same implementation as POST /v1/delete. This is destructive. The model_name must identify an existing model exactly and confirm must be true. A loaded model is unloaded first, matching the REST endpoint.

**Returns**

Returns a result from lemonade_delete_model, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": true,
  "openWorldHint": false,
  "readOnlyHint": false
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `confirm` | yes | `{"enum":[true],"type":"boolean"}` | Must be true to execute deletion. |
| `model_name` | yes | `{"type":"string"}` | Exact existing model ID to delete. |

### `lemonade_get_system_info`

Return Lemonade hardware and recipe/backend capability information using the same implementation as GET /api/v1/system-info.

**Returns**

Returns a result from lemonade_get_system_info, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

This tool takes no arguments.

### `lemonade_list_backends`

List Lemonade recipes/backends and their installation/support state. This is the backend-focused view of the canonical /api/v1/system-info data.

**Returns**

Returns a result from lemonade_list_backends, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

This tool takes no arguments.

### `lemonade_install_backend`

Install or update one Lemonade recipe/backend through the same implementation as POST /api/v1/install. The MCP call is synchronous; use lemonade_list_backends first to inspect support and current state.

**Returns**

Returns a result from lemonade_install_backend, including structured content on success.

**MCP annotations**

```json
{
  "destructiveHint": true,
  "openWorldHint": true,
  "readOnlyHint": false
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `backend` | yes | `{"pattern":"\\S","type":"string"}` | Argument backend for the lemonade_install_backend tool. |
| `force` | no | `{"type":"boolean"}` | Override an unsupported-state guard when the underlying API permits it. |
| `recipe` | yes | `{"pattern":"\\S","type":"string"}` | Argument recipe for the lemonade_install_backend tool. |

### `lemonade_list_models`

List models known to the Lemonade server. ALWAYS call this first if you don't already know the exact model name to use for chat/transcribe/image — passing a wrong name may trigger a multi-GB download. Returns a summary text block plus a JSON block with `{loaded, available, suggested_to_pull, recommended_chat_model}`.

**Returns**

Success returns two `text` content blocks: a human-readable summary, then a JSON-stringified object with `loaded`, `available`, `suggested_to_pull`, and `recommended_chat_model`.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": false,
  "readOnlyHint": true
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `include_available` | no | `{"type":"boolean"}` | Include downloaded models in the `available` list. |
| `include_suggested` | no | `{"type":"boolean"}` | Include suggested, not-yet-downloaded models in `suggested_to_pull`. |

### `lemonade_chat`

Chat completion against a locally hosted LLM. Pass a `messages` array (OpenAI chat format). `model` is OPTIONAL: when omitted, the server reuses an already-loaded LLM, else an already-downloaded one; if neither exists it asks you to either pass a `model` or `allow_download: true` (which downloads the default, Qwen3.5-4B-MTP-GGUF). Call `lemonade_list_models` first if you want to choose explicitly — a wrong name may trigger a multi-GB download.

**Returns**

Success returns one `text` content block with the assistant output. If the model emits tool calls, a second `text` block prefixed with `tool_calls: ` contains the JSON tool-call array. If normal content is empty but reasoning content is present, the first block contains the reasoning fallback instead of an empty string.

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

Transcribe an audio clip with a Whisper-class model. The Lemonade MCP server always runs on the same machine as the caller, so prefer `audio_path` (an absolute path to a local audio file: wav, mp3, m4a, ogg, flac, webm). Use `audio_base64` only when you genuinely have audio bytes in memory. If both are provided, `audio_path` takes precedence. `model` is OPTIONAL: when omitted, the server reuses an already-loaded transcription model, else an already-downloaded one; if neither exists it asks you to pass a `model` or `allow_download: true` (which downloads the default, Whisper-Tiny).

**Returns**

Success returns two `text` content blocks: the transcript, then the JSON-stringified full transcription response, including timestamps or segments when provided by the backend.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default model when none is loaded or downloaded. Defaults to false. |
| `audio_base64` | no | `{"type":"string"}` | Base64-encoded audio data. Used when `audio_path` is not provided. |
| `audio_path` | no | `{"type":"string"}` | Path to a local audio file. Preferred over `audio_base64`; absolute paths avoid working-directory ambiguity. |
| `filename` | no | `{"type":"string"}` | Original audio filename, used as an input-format hint. |
| `language` | no | `{"type":"string"}` | Optional source language hint. |
| `model` | no | `{"type":"string"}` | Optional. Omit to auto-select a loaded/downloaded model; defaults to Whisper-Tiny only with allow_download=true. |
| `prompt` | no | `{"type":"string"}` | Optional text prompt to guide transcription. |
| `response_format` | no | `{"enum":["json","text","srt","verbose_json","vtt"],"type":"string"}` | Transcription response format. |
| `temperature` | no | `{"type":"number"}` | Sampling temperature for transcription. |

### `lemonade_generate_image`

Generate one or more images from a text prompt. The Lemonade MCP server always runs on the same machine as the caller, so PREFER writing the result directly to disk by passing `output_path` (single image) or `output_dir` (one or more). When you do, the tool returns absolute file path(s) as text — no base64 round-trip and dramatically fewer tokens. Only omit both arguments when you genuinely need the image inline. For safety, disk writes are confined to a sandbox directory (&lt;cache_dir&gt;/mcp-images, or LEMONADE_MCP_IMAGE_DIR if set); paths outside it are rejected. `output_dir` writes get unique auto-generated filenames (so concurrent callers never clobber each other); use `output_path` when you need an exact name. `model` is OPTIONAL: when omitted, the server reuses an already-loaded image model, else an already-downloaded one; if neither exists it asks you to pass a `model` or `allow_download: true` (which downloads the default, SD-Turbo).

**Returns**

Without `output_path` or `output_dir`, success returns one native MCP `image` content block per generated PNG. With disk output, success returns a summary `text` block, optional per-image path blocks when multiple files are written, and a final JSON-stringified `paths` object in a `text` block.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default model when none is loaded or downloaded. Defaults to false. |
| `cfg_scale` | no | `{"type":"number"}` | Classifier-free guidance scale. |
| `model` | no | `{"type":"string"}` | Optional. Omit to auto-select a loaded/downloaded image model; defaults to SD-Turbo only with allow_download=true. |
| `n` | no | `{"minimum":1,"type":"integer"}` | Number of images to generate. |
| `negative_prompt` | no | `{"type":"string"}` | Text describing content to avoid in the generated image. |
| `output_dir` | no | `{"type":"string"}` | Directory to write generated images into, inside the MCP image sandbox. Filenames are auto-generated and unique (image_{token}_{index}.png); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it. |
| `output_path` | no | `{"type":"string"}` | Exact path of the PNG file to write, inside the MCP image sandbox (&lt;cache_dir&gt;/mcp-images or LEMONADE_MCP_IMAGE_DIR). Relative paths resolve against the sandbox root; absolute paths must stay within it. Written as named (overwrites if it already exists). Only valid when n == 1. |
| `prompt` | yes | `{"type":"string"}` | Text prompt describing the image to generate. |
| `seed` | no | `{"type":"integer"}` | Optional random seed for reproducible generation. |
| `size` | no | `{"type":"string"}` | Requested image size as WIDTHxHEIGHT, for example 512x512. |
| `steps` | no | `{"type":"integer"}` | Number of diffusion sampling steps. |

### `lemonade_edit_image`

Edit an image using a model advertising the image-edit capability. The host chooses the source image and passes it as base64 or a data:image/...;base64 URL. An explicit model must be an image-edit capable IMAGE model. Without a model, Lemonade prefers an already loaded eligible model, then an already downloaded one. Generation-only image models are skipped and no model is downloaded automatically.

**Returns**

On success returns native MCP image content blocks containing the edited images as base64 PNG data. If an explicit model lacks image-edit or no local eligible model exists, returns one error text block with isError=true.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `cfg_scale` | no | `{"type":"number"}` | Classifier-free guidance scale. |
| `image` | yes | `{"type":"string"}` | Raw base64 image bytes or a data:image/...;base64 URL. |
| `model` | no | `{"type":"string"}` | Optional. An IMAGE model with image-edit capability; omit to select an eligible local model. |
| `n` | no | `{"maximum":10,"minimum":1,"type":"integer"}` | Number of edited images to return. |
| `prompt` | yes | `{"type":"string"}` | Instructions for editing the source image. |
| `seed` | no | `{"type":"integer"}` | Optional random seed. |
| `size` | no | `{"type":"string"}` | Output size as WIDTHxHEIGHT. |
| `steps` | no | `{"minimum":1,"type":"integer"}` | Number of image editing steps. |

### `lemonade_omni`

Multimodal turn against a Lemonade Omni collection (one tool call -&gt; text + images + speech in the same response). The server runs an internal tool-calling loop against the collection's planner LLM and executes its image / image-edit / TTS tools by routing to the bundled component models; the result comes back as a text block plus native MCP `image` / `audio` content blocks (one per artifact). `model` is OPTIONAL: when omitted, the server reuses an already-downloaded Omni collection; if none is downloaded it asks you to pass a `model` or `allow_download: true` (which downloads the default, `LMX-Omni-5.5B-Lite`). Pass `model='LMX-Omni-52B-Halo'` (or any other recipe='collection.omni' model from `lemonade_list_models`) to opt into a larger collection; that model may be multi-GB. Same-machine deployment: PREFER `output_dir` to write artifacts to disk and avoid expensive inline base64 blobs. For plain text chat against a regular LLM, use `lemonade_chat` instead.

**Returns**

Success always starts with one `text` block containing the final text. Artifacts follow in the order they were produced. Inline mode then appends one native MCP `image` or `audio` block per artifact. Disk mode instead appends one `text` block per artifact path in the same order, plus a final JSON-stringified `paths` object. If application tool calls are emitted, a final `text` block prefixed with `tool_calls: ` is appended.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `allow_download` | no | `{"type":"boolean"}` | Permit downloading the default collection when none is downloaded. Defaults to false. |
| `chat_template_kwargs` | no | `{"type":"object"}` | Additional chat-template arguments forwarded to the planner LLM. |
| `max_tokens` | no | `{"type":"integer"}` | Maximum number of planner output tokens. |
| `messages` | yes | `{"items":{"type":"object"},"type":"array"}` | Conversation messages in OpenAI chat format. |
| `model` | no | `{"type":"string"}` | Optional. Omni collection name (recipe='collection.omni'). Omit to reuse a downloaded collection; defaults to LMX-Omni-5.5B-Lite only with allow_download=true. |
| `output_dir` | no | `{"type":"string"}` | Directory to write produced artifacts into, inside the MCP image sandbox (&lt;cache_dir&gt;/mcp-images or LEMONADE_MCP_IMAGE_DIR). Filenames are auto-generated and unique (omni_{token}_{index}.{ext}); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it. PREFER this to inline base64 when caller and server share a filesystem. Omit to receive artifacts inline as MCP content blocks. |
| `response_format` | no | `{"type":"object"}` | OpenAI-compatible response format configuration for the planner. |
| `seed` | no | `{"type":"integer"}` | Optional random seed forwarded to the planner LLM. |
| `stop` | no | `{}` | stop sequences (string or array) |
| `temperature` | no | `{"type":"number"}` | Sampling temperature for the planner LLM. |
| `tool_choice` | no | `{}` | auto \| none \| required \| {type: function, ...} |
| `tools` | no | `{"items":{"type":"object"},"type":"array"}` | OpenAI-compatible application tool definitions passed to the planner. |
| `top_p` | no | `{"type":"number"}` | Nucleus sampling probability for the planner LLM. |

### `lemonade_web_search`

Search live web or news for current information, products, releases and research. Use search_type=news for recent headlines. Only the query goes to an external provider; linked pages are not fetched.

**Returns**

Success returns structuredContent with query, search_type, results (title, HTTPS url, snippet; optional source and published_at), and truncated, plus numbered text. Provider failures return isError: true.

**MCP annotations**

```json
{
  "destructiveHint": false,
  "openWorldHint": true,
  "readOnlyHint": true
}
```

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `max_results` | no | `{"minimum":1,"type":"integer"}` | Optional number of results, maximum 5 (default 5). |
| `query` | yes | `{"type":"string"}` | Search terms (1-256 UTF-8 bytes). |
| `search_type` | no | `{"enum":["web","news"],"type":"string"}` | Use news for current events/headlines; default web. |

### `lemonade_docs`

Read this server's own API reference. Call with no arguments to list the pages it ships, then pass `page` (an `id` from that list) to read one as markdown. The docs are bundled with the server, so they describe the version actually running and work offline. Use this before hand-writing requests against Lemonade endpoints.

**Returns**

With `page`, success returns one `text` block containing the raw bundled Markdown page. With no arguments, success normally returns a summary `text` block followed by a JSON-stringified `pages` object containing `id`, `title`, and `bytes` for each page; if no bundled documentation is installed, it returns one explanatory `text` block instead. Unknown pages return `isError: true` with one explanatory `text` block. The same bundled documentation is also available through `GET /v1/docs`.

| Argument | Required | Schema | Description |
|---|:---:|---|---|
| `page` | no | `{"type":"string"}` | Optional. Page id from the listing, e.g. 'api/lemonade'. Omit to list. |

<!-- END GENERATED: mcp-tools -->

### Additional server capabilities

`tools/list` is the canonical runtime contract. The following additional
tools reuse Lemonade's existing server implementations:

| Capability | MCP tool |
|-----------|----------|
| Model detail | `lemonade_get_model_info` |
| Load / unload | `lemonade_load_model`, `lemonade_unload_model` |
| Pull / delete | `lemonade_pull_model`, `lemonade_delete_model` |
| Model residency | `lemonade_get_loaded_models` |
| Health | `lemonade_get_server_health` |
| Hardware / backends | `lemonade_get_system_info`, `lemonade_list_backends` |
| Backend installation | `lemonade_install_backend` |

Lifecycle actions can change local model state and consume resources.
MCP annotations and `confirm: true` for model deletion help clients
present approvals, but are not authorization controls. Clients should
approve destructive actions and backend installation explicitly.

## Quick Test with curl

After the `initialize` request in [`POST /mcp`](#post-mcp), list the tools and call one:

```bash
curl -s http://localhost:13305/mcp -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

curl -s http://localhost:13305/mcp -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"lemonade_chat","arguments":{"model":"Qwen3-1.7B-GGUF","messages":[{"role":"user","content":"hi"}],"max_tokens":16}}}'
```
