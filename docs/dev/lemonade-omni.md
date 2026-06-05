# Lemonade Omni Models

**Lemonade Omni Models** provide true all-to-all omni-modality to users and apps. They accomplish this by unifying the capabilities of a collection of an LLM, an image model, an ASR model, and a TTS model. Under the hood, Lemonade Omni Models are powered by **OmniRouter**, Lemonade's pattern for exposing each modality as an OpenAI-compatible tool.

## Provided Omni Models

An **omni model** is a virtual model made up of components, registered with `recipe: "collection.omni"`. Lemonade ships these:

| Omni model | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **LMX-Omni-52B-Halo** | Qwen3.6-35B-A3B-MTP-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **LMX-Omni-5.5B-Lite** | Qwen3.5-4B-MTP-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Once all of an omni model's components are downloaded, it appears in the default `/v1/models` listing (and Ollama `/api/tags`) — because the server orchestrates `/chat/completions` for it, it behaves as a genuine OpenAI-compatible chat model. Not-yet-downloaded omni models surface with `?show_all=true`, and all of them appear in the Lemonade desktop app's Model Manager under the **Lemonade** category.

### Naming Scheme

Omni model names follow the pattern `LMX-Omni-<xB>-<class>`:

| Component | Value | Meaning |
|-----------|-------|---------|
| Org prefix | `LMX` | Lemonade Mix. |
| Modality | `Omni` | True all-to-all omni-modal bundle. |
| Size | `xB` | Total parameter count across all component models. |
| Class | `Halo` | Based on a large MoE LLM (e.g., targeted at Strix Halo). |
|  | `Lite` | Based on small models targeted at 32 GB APUs. |
|  | `Dense` | Based on a dense LLM targeted at 32 GB dGPUs (none shipped yet). |

## Available Tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) — a single source of truth used by the desktop app, the server-side orchestrator (the file is staged into the server's resources at build time), and this documentation.

| Tool | Endpoint | Needs a model with label |
|------|----------|--------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Endpoints Spec](../api/README.md).

## How to Use Omni Models

Any app can use an omni collection by simply requesting `/chat/completions` and receiving multi-media results in the response content. Apps that want a higher degree of customization can instead send their requests to the collection's planner LLM, with a custom system prompt and tool definitions, and receive tool calls in the response.

| | **Server-Side Orchestration** | **Client-Side Orchestration** |
|---|---|---|
| Best for | Any OpenAI-compatible frontend (e.g. **Open WebUI**). | Apps with an existing tool-calling loop that need full control. |
| Request | `/chat/completions` addressed to the **collection name**. | `/chat/completions` addressed to the **planner LLM** (component model name). |
| Omni tool execution | Server internally executes each omni tool call; client-supplied tools still return for the client to run. | Client executes each omni tool call against the component endpoints. |
| System prompt & tools | Injected by the server. | Supplied by the client. |
| Generated media | Embedded in the assistant message (markdown image / `<audio>` data-URI). | Each endpoint's native payload (`b64_json` image, audio bytes). |

## Server-Side Orchestration

Address a `POST /v1/chat/completions` request to the **collection name** (e.g. `LMX-Omni-5.5B-Lite`); the server runs the tool-calling loop and embeds generated media in the assistant message. The full request/response contract is specified in [`POST /v1/chat/completions` → Server-side tools](../api/openai.md#server-side-tools).

**Scope.** Server-side orchestration covers `generate_image`, `edit_image`, and `text_to_speech`. The `transcribe_audio` and `analyze_image` tools remain client-side tools — most chat frontends transcribe audio themselves before sending and pass images straight through to the model.

## Client-Side Orchestration

Point an OpenAI-compatible client at `http://localhost:13305/v1` and supply the OmniRouter tool schemas from [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) (load the file directly, or copy its entries into the client's tool list). The loop then runs entirely over OpenAI-compatible calls:

1. `POST /v1/chat/completions` to the planner LLM (the collection's component LLM name) with `tools` set to the OmniRouter tool schemas.
2. When the planner decides to act, it returns `finish_reason: "tool_calls"` with one or more `tool_calls`, each carrying a function name and a JSON `arguments` string.
3. For each `tool_call`, POST its arguments to the corresponding endpoint (`/v1/images/generations`, `/v1/audio/speech`, …) and capture the response.
4. Append each endpoint result to the message list as a `tool` message keyed by the originating `tool_call_id`, then re-issue the chat completion.
5. Repeat until the planner returns `finish_reason: "stop"` with a final assistant message.

To select components programmatically instead of relying on a loaded omni model, query `GET /v1/models?show_all=true` and match each model's `labels` against the [Available tools](#available-tools) table. No Lemonade-specific client library is required: the tool schemas are plain OpenAI-format JSON, and every target endpoint uses OpenAI-compatible request and response shapes.

[`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py) implements the full loop end-to-end:

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

## Custom Omni Models

You can build your own omni model from registered models — see [Register a custom Omni Model from the desktop app](../guide/configuration/custom-models.md#register-a-custom-omni-model-from-the-desktop-app) in the custom models guide. The planner LLM must carry the `tool-calling` label, and each modality must have a downloaded model whose `labels` include the matching entry from the [tools table](#available-tools).
