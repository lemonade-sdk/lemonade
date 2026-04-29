# OmniRouter

OmniRouter is Lemonade's approach to multimodal agentic workflows. Instead of building a proprietary agent runtime into Lemonade, we expose each modality as an **OpenAI-compatible tool** that any existing LLM agent (Continue, OpenHands, Claude Code, your own app) can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the tools.

## How it works

1. You describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client code executes each `tool_call` against the corresponding Lemonade endpoint (`/v1/images/generations`, `/v1/audio/speech`, etc.) and feeds the result back as a `tool` message.
4. The LLM continues, calling more tools or producing a final response.

This is the standard OpenAI tool-calling loop. The tool schemas OmniRouter provides are plain JSON (no Lemonade-specific client library required), the endpoints they target are OpenAI-compatible, and the server returns standard response shapes.

## Collections

A **Collection** is a preconfigured bundle of models sized for a hardware tier. Selecting a collection in the Lemonade desktop app loads one LLM + one image model + one ASR + one TTS — all the pieces OmniRouter's tools need in a single click.

| Collection | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **Ultra Collection** | Qwen3.5-35B-A3B-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **Lite Collection** | Qwen3.5-4B-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Collections are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "Ultra Collection" as if it were a real model. They surface with `?show_all=true` and appear in the desktop app's model list.

Use a Collection. Every part of this doc assumes one is loaded — the desktop app, [`examples/lemonade_tools.py`](../../examples/lemonade_tools.py), and the tools themselves were all validated against the Ultra and Lite Collections above.

If you're the developer wiring OmniRouter into your own agent and you want to substitute models, you can, but you take on the compatibility work: any LLM you swap in must carry the `tool-calling` label, and each tool you want to call needs one downloaded model whose `labels` include the row's "Needs a model with label" entry from the tools table below. That's a developer-path discovery step, not a user configuration; the simple answer for everyone else is "install a Collection."

## Available tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](../../src/app/src/renderer/utils/toolDefinitions.json) — a single source of truth used by the desktop app and this documentation.

| Tool | Endpoint | Needs a model with label |
|------|----------|-------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` or `speech` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `audio` or `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Endpoints Spec](../api/README.md).

## Quick start

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](../../examples/lemonade_tools.py) shows the full agentic loop — tool definitions, LLM call with `tools=[...]`, executing each `tool_call`, and feeding the result back. Fewer than 150 lines of Python.

## Using your own agent

Integrate OmniRouter into an existing agent by following the pattern in [`examples/lemonade_tools.py`](../../examples/lemonade_tools.py):

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`src/app/src/renderer/utils/toolDefinitions.json`](../../src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list (or load the JSON directly).
3. When your agent receives a `tool_call` for one of these tools, POST to the corresponding endpoint from the table above and feed the response back to the LLM as a `tool` message.
4. If you want to pick models programmatically rather than rely on a Collection being loaded, query `GET /v1/models?show_all=true` and match the `labels` array against the "Needs a model with label" column above.

The example script implements all four steps end-to-end against the `generate_image` and `text_to_speech` tools.
