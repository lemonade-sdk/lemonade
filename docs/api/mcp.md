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

All tools auto-load (and download, if missing) the requested model on first call, exactly like `POST /v1/chat/completions`. Errors are returned as MCP results with `"isError": true` rather than JSON-RPC errors, matching the spec's guidance for tool failures.

### `lemonade_list_models`

Discover what's loaded, what's downloaded, and what's recommended. Call this first if you don't already know the exact model name to pass to the other tools — passing a wrong name may trigger a multi-GB download.

```json
{
  "name": "lemonade_list_models",
  "arguments": {
    "include_available": true,
    "include_suggested": true
  }
}
```

Returns a summary text block plus a JSON-stringified text block with `{loaded, available, suggested_to_pull, recommended_chat_model}`.

### `lemonade_chat`

Chat completion against any LLM in the registry.

```json
{
  "name": "lemonade_chat",
  "arguments": {
    "model": "Qwen3-1.7B-GGUF",
    "messages": [
      {"role": "system", "content": "You are concise."},
      {"role": "user", "content": "Summarize MCP in one line."}
    ],
    "max_tokens": 64,
    "temperature": 0.2
  }
}
```

Returns one text block with the assistant content. If the model emits tool calls, a second text block containing `tool_calls: <json>` is appended.

Reasoning models (Qwen3, DeepSeek-R1, ...) have the `<think>` block disabled by default to keep small `max_tokens` budgets from being consumed by reasoning. Pass `"chat_template_kwargs": {"enable_thinking": true}` to opt back in.

> **Picking a portable model.** The example above uses `Qwen3-1.7B-GGUF` because GGUF (llama.cpp) runs everywhere lemonade does — Windows, Linux/Docker, macOS, CPU and Vulkan/ROCm/Metal GPUs. Hybrid/NPU variants such as `*-Hybrid` (recipe `ryzenai-llm`, **Windows + AMD RyzenAI** only) or `*-FLM` (recipe `flm`, **AMD Ryzen AI NPU** only) are faster on supported hardware but unavailable on others. If your client picks one that isn't supported, the tool returns a structured error suggesting a portable alternative — prefer `lemonade_list_models` to discover what's actually available on the running server.

### `lemonade_transcribe_audio`

Transcribe a local audio file with a Whisper-class model. Prefer `audio_path` (the server runs on the same machine as the caller); use `audio_base64` only when you genuinely have bytes in memory.

```json
{
  "name": "lemonade_transcribe_audio",
  "arguments": {
    "model": "Whisper-Large-v3-Turbo",
    "audio_path": "C:/clips/meeting.wav",
    "response_format": "verbose_json"
  }
}
```

Returns two text blocks: the bare transcript, followed by the full OpenAI-shaped response (for callers that need timestamps or segments).

### `lemonade_generate_image`

Generate one or more PNGs from a prompt. **Prefer writing to disk** via `output_path` (single image) or `output_dir` (one or more) — base64 image content blocks cost tens of thousands of tokens per image and some clients surface them as opaque resource URIs.

```json
{
  "name": "lemonade_generate_image",
  "arguments": {
    "model": "SDXL-Turbo",
    "prompt": "a lemon-shaped car driving across the moon",
    "size": "512x512",
    "output_path": "lemon-car.png"
  }
}
```

When disk paths are provided, returns text block(s) with the absolute path(s). Otherwise, returns one inline image content block per image (`{"type":"image", "data":"<base64>", "mimeType":"image/png"}`).

**Sandboxed disk writes.** To prevent a cross-origin or unauthenticated caller from overwriting arbitrary files, `output_path` and `output_dir` are confined to a sandbox directory:

- Default: `<cache_dir>/mcp-images`.
- Override with the `LEMONADE_MCP_IMAGE_DIR` environment variable (absolute path).
- Relative paths resolve against the sandbox root; absolute paths must stay within it. Paths that escape the sandbox (via `..` or symlinks) are rejected.
- `output_dir` writes use auto-generated, unique filenames (`image_<token>_<i>.png`), so concurrent callers never clobber one another's images — the returned `paths` tell you the exact names. Use `output_path` when you need an exact, caller-chosen filename (it is written as named, replacing any existing file at that path).

### `lemonade_omni`

One-shot multimodal turn against a **Lemonade Omni collection** (a model bundle that pairs a planner LLM with an image model, an image-edit model, and a TTS voice under a single `collection.omni` recipe — see [the Omni docs](../dev/lemonade-omni.md)). The server runs the orchestrator's internal tool-calling loop, executes the collection's `generate_image` / `edit_image` / `text_to_speech` tools by routing to the bundled components, and returns the result as a text block plus native MCP `image` / `audio` content blocks — one per artifact, in the order they were produced.

`model` is **optional** and defaults to `LMX-Omni-5.5B-Lite` (smaller and faster). Pass `model` explicitly to opt into a larger collection (e.g. `LMX-Omni-52B-Halo` on capable hardware) or any other `collection.omni` model surfaced by `lemonade_list_models`. The collection is downloaded on first use and may be multi-GB.

Use `lemonade_chat` instead when you only need plain-text LLM output and don't want the planner-loop overhead.

```json
{
  "name": "lemonade_omni",
  "arguments": {
    "messages": [
      {"role": "user", "content": "Generate an image of a lemon car, then read out a one-line description."}
    ],
    "output_dir": "omni"
  }
}
```

**Disk vs. inline output.** A single Omni turn can produce both images and audio in arbitrary order. Pass an `output_dir` to write each artifact to disk under a unique auto-generated name (`omni_<token>_<i>.<ext>`) — the tool returns one text block per artifact with its absolute path, plus a JSON-stringified `paths` array. This is strongly preferred over inline base64 for the same reasons documented under `lemonade_generate_image` — and is the **only** way to get audio out on clients that don't render `audio` content blocks. Like `lemonade_generate_image`, `output_dir` is confined to the MCP image sandbox (see **Sandboxed disk writes** above): relative paths resolve against the sandbox root, paths escaping it are rejected, and unique filenames mean concurrent callers never clobber each other.

When `output_dir` is omitted, artifacts are inlined as MCP content blocks: `{"type":"image", "data":"<base64>", "mimeType":"image/png"}` and `{"type":"audio", "data":"<base64>", "mimeType":"audio/mpeg"}`.

If the planner emits app-defined tool calls (those you passed in via `tools`/`tool_choice`), an extra text block `tool_calls: <json>` is appended, matching `lemonade_chat`'s passthrough semantics.

Passing a non-collection model (e.g. a plain LLM) returns `isError: true` with a hint to use `lemonade_chat`.

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
