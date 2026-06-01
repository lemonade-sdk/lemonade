# Lemonade Omni Models

**Lemonade Omni Models** provide true all-to-all omni-modality to users and apps. They accomplish this by unifying the capabilities of a collection of an LLM, an image model, an ASR model, and a TTS model — everything a multimodal agent needs to chat, generate images, transcribe audio, and speak responses out loud.

Under the hood, Lemonade Omni Models are powered by **OmniRouter** — Lemonade's pattern for exposing each modality as an OpenAI-compatible tool. OmniRouter is the engine; you choose who drives the loop.

## Two ways to use Omni models

| | **Client-side OmniRouter** (bring your own loop) | **Server-side orchestration** (`/chat/completions`) |
|---|---|---|
| Who runs the tool loop | Your app | Lemonade |
| You send | Tool calls to `/v1/images/generations`, `/v1/audio/speech`, … | A normal chat to the **collection model name** |
| You write | The system prompt + agentic loop | Nothing — the server injects the reference prompt and tools |
| Media comes back as | Raw endpoint responses you render yourself | Embedded in the assistant message (markdown image / `<audio>` data-URIs) |
| Best for | Apps that already have an agent and want full control | Any OpenAI-compatible frontend (e.g. **Open WebUI**) |

**Which should I pick?** If you already run an agent loop, use the client-side path for maximum control. If you want it to "just work" in an existing chat UI, point that UI at Lemonade and chat with the collection model — the server does the rest.

The two paths are distinguished by the model name you address: target a **collection** name for server-side orchestration, or a **component LLM** name to drive your own loop.

## How client-side OmniRouter works

1. Describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client executes each `tool_call` against the corresponding Lemonade endpoint, such as `/v1/images/generations` or `/v1/audio/speech`.
4. The client sends the tool result back to the LLM as a `tool` message.
5. The LLM continues until it either calls another tool or returns a final response.

The tool schemas OmniRouter provides are plain JSON. They do not require a Lemonade-specific client library, and the endpoints they target use OpenAI-compatible request and response shapes.

## Server-side orchestration

Send a normal `POST /v1/chat/completions` whose `model` is the **collection name** (e.g. `LMX-Omni-5.5B-Lite`). Lemonade injects the reference system prompt and tools, runs an internal tool-calling loop against the chat component, executes the omni tools (image gen/edit, text-to-speech) by routing to the matching component, and returns one OpenAI-compatible response. Generated media is embedded directly in the assistant message:

- **images** → markdown `![generated image](data:image/png;base64,…)`
- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`

Both streaming (`stream: true`, SSE `chat.completion.chunk` frames — media arrives as a content delta the moment its tool finishes) and non-streaming are supported.

**System-prompt merging.** If your request includes a system message, it is preserved — the omni tool instructions are *prepended* to it. A generic persona ("you are a helpful legal assistant") composes cleanly. Don't hand-author omni tool descriptions when targeting the collection name; the server already does.

**Tool merging (middleware).** Any `tools` you send are merged with the omni tools. The server resolves omni tool calls itself and returns any of *your* tool calls to you as a normal `finish_reason: "tool_calls"` response to execute and resume — so a frontend's own tools (Open WebUI's native function-calling, custom business tools) keep working alongside omni media. In a turn that mixes both, omni media is folded into the assistant `content` while your calls are returned in the same message's `tool_calls`.

**Scope.** Server-side orchestration covers `generate_image`, `edit_image`, and `text_to_speech`. The `transcribe_audio` and `analyze_image` tools remain client-side (path 1) tools — most chat frontends transcribe audio themselves before sending and pass images straight through to the model.

## The omni models

An **omni model** is a virtual model made up of components, registered with `recipe: "collection.omni"`. Lemonade ships these:

| Omni model | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **LMX-Omni-52B-Halo** | Qwen3.6-35B-A3B-MTP-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **LMX-Omni-5.5B-Lite** | Qwen3.5-4B-MTP-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Once all of an omni model's components are downloaded, it appears in the default `/v1/models` listing (and Ollama `/api/tags`) — because the server orchestrates `/chat/completions` for it, it behaves as a genuine OpenAI-compatible chat model. Not-yet-downloaded omni models surface with `?show_all=true`, and all of them appear in the Lemonade desktop app's Model Manager under the **Lemonade** category.

### Naming scheme

Omni model names follow the pattern `LMX-Omni-<xB>-<class>`:

| Component | Value | Meaning |
|-----------|-------|---------|
| Org prefix | `LMX` | Lemonade Mix. |
| Modality | `Omni` | True all-to-all omni-modal bundle. |
| Size | `xB` | Total parameter count across all component models. |
| Class | `Halo` | Based on a large MoE LLM (e.g., targeted at Strix Halo). |
|  | `Lite` | Based on small models targeted at 32 GB APUs. |
|  | `Dense` | Based on a dense LLM targeted at 32 GB dGPUs (none shipped yet). |

### Use an omni model

Every part of this doc assumes one is loaded — the desktop app, [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py), and the tools themselves were all validated against the two omni models above.

If you're the developer wiring OmniRouter into your own agent and you want to substitute models, you can, but you take on the compatibility work: any LLM you swap in must carry the `tool-calling` label, and each tool you want to call needs one downloaded model whose `labels` include the row's "Needs a model with label" entry from the tools table below. That's a developer-path discovery step, not a user configuration; the simple answer for everyone else is "install an omni model."

## Custom Omni Models

You can build your own omni model from registered models — see [Register a custom Omni Model from the desktop app](../guide/configuration/custom-models.md#register-a-custom-omni-model-from-the-desktop-app) in the custom models guide.

## Available tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) — a single source of truth used by the desktop app and this documentation.

| Tool | Endpoint | Needs a model with label |
|------|----------|--------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Endpoints Spec](../api/README.md).

## Quick start

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py) shows the full agentic loop — tool definitions, LLM call with `tools=[...]`, executing each `tool_call`, and feeding the result back. Fewer than 150 lines of Python.

## Using your own agent (client-side)

Integrate OmniRouter into an existing agent by following the pattern in [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py):

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list (or load the JSON directly).
3. When your agent receives a `tool_call` for one of these tools, POST to the corresponding endpoint from the table above and feed the response back to the LLM as a `tool` message.
4. If you want to pick models programmatically rather than rely on an omni model being loaded, query `GET /v1/models?show_all=true` and match the `labels` array against the "Needs a model with label" column above.

The example script implements all four steps end-to-end against the `generate_image` and `text_to_speech` tools.
