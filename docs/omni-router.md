# OmniRouter

OmniRouter is Lemonade's approach to multimodal agentic workflows. Instead of building a proprietary agent runtime into Lemonade, we expose each modality as an **OpenAI-compatible tool** that any existing LLM agent (Continue, OpenHands, Claude Code, your own app) can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the tools.

## How it works

1. You describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client code executes each `tool_call` against the corresponding Lemonade endpoint (`/v1/images/generations`, `/v1/audio/speech`, etc.) and feeds the result back as a `tool` message.
4. The LLM continues, calling more tools or producing a final response.

This is the same pattern every OpenAI-compatible agent already uses. No custom SDK, no lock-in.

## Collections

A **Collection** is a preconfigured bundle of models sized for a hardware tier. Selecting a collection in the Lemonade desktop app loads one LLM + one image model + one ASR + one TTS — all the pieces OmniRouter's tools need in a single click.

| Collection | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **Ultra Collection** | Qwen3.5-35B-A3B-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **Lite Collection** | Qwen3.5-4B-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Collections are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "Ultra Collection" as if it were a real model. They surface with `?show_all=true` and appear in the desktop app's model list.

You don't have to use a Collection — any LLM with the `tool-calling` label plus any image / TTS / ASR model works.

## Available tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json) — a single source of truth used by the desktop app and this documentation.

| Tool | Endpoint | Needs a model with label |
|------|----------|-------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` or `speech` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `audio` or `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Server Spec](server/server_spec.md).

## Quick start

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](../examples/lemonade_tools.py) shows the full agentic loop — tool definitions, LLM call with `tools=[...]`, executing each `tool_call`, and feeding the result back. Fewer than 150 lines of Python.

## Using your own agent

Point your OpenAI-compatible client at `http://localhost:13305/v1`, list models, discover capabilities by label, and wire up the tools you care about. The discovery flow:

```bash
curl http://localhost:13305/v1/models
```

Each model entry includes a `labels` array. Map labels to tools using the table above.
