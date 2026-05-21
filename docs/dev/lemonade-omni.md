# Lemonade Omni Models

**Lemonade Omni Models** provide true all-to-all omni-modality to users and apps. They accomplish this by unifying the capabilities of a collection of an LLM, an image model, an ASR model, and a TTS model — everything a multimodal agent needs to chat, generate images, transcribe audio, and speak responses out loud.

Under the hood, Lemonade Omni Models are powered by **OmniRouter** — Lemonade's pattern for exposing each modality as an OpenAI-compatible tool that an existing LLM agent can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the local tools.

## How OmniRouter works

1. Describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client executes each `tool_call` against the corresponding Lemonade endpoint, such as `/v1/images/generations` or `/v1/audio/speech`.
4. The client sends the tool result back to the LLM as a `tool` message.
5. The LLM continues until it either calls another tool or returns a final response.

The tool schemas OmniRouter provides are plain JSON. They do not require a Lemonade-specific client library, and the endpoints they target use OpenAI-compatible request and response shapes.

## The omni models

An **omni model** is a virtual model made up of components, registered with `recipe: "collection.omni"`. Lemonade ships these:

| Omni model | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **LMX-Omni-52B-Halo** | Qwen3.6-35B-A3B-MTP-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **LMX-Omni-5.5B-Lite** | Qwen3.5-4B-MTP-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Omni models are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "LMX-Omni-52B-Halo" as if it were an LLM. They surface with `?show_all=true` and appear in the Lemonade desktop app's Model Manager under the **Lemonade** category.

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

## Using your own agent

Integrate OmniRouter into an existing agent by following the pattern in [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py):

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list (or load the JSON directly).
3. When your agent receives a `tool_call` for one of these tools, POST to the corresponding endpoint from the table above and feed the response back to the LLM as a `tool` message.
4. If you want to pick models programmatically rather than rely on an omni model being loaded, query `GET /v1/models?show_all=true` and match the `labels` array against the "Needs a model with label" column above.

The example script implements all four steps end-to-end against the `generate_image` and `text_to_speech` tools.
