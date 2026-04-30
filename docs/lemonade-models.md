# Lemonade Models

A **Lemonade Model** is a preconfigured bundle of real models — typically one LLM plus an image model, an ASR model, and a TTS — distributed as a single shareable unit. Pulling a Lemonade Model downloads every component in one command, and loading it loads them all at once. They're the easiest way to get a multimodal agent running, and they're shareable: any Hugging Face repo with a `lemonade.json` manifest at the root is a valid Lemonade Model.

## Quick start

```bash
# Pull a Lemonade Model from Hugging Face
lemonade pull lemonade-sdk/lemonade-ultra

# Or install one from the desktop app: Lemonade > Models > Lemonade Models
```

Once downloaded, every component is loaded together when you select the Lemonade Model in the chat UI, and any agent pointed at Lemonade can call across the whole bundle.

## How a Lemonade Model is defined

A Lemonade Model is just a Hugging Face repo whose root contains a `lemonade.json` manifest. The manifest declares `recipe: "collection"` and lists the component models that make up the bundle. Components can be either references to models already curated in Lemonade's registry (string entries) or inline objects describing arbitrary HF checkpoints.

### Example: `lemonade-sdk/lemonade-ultra/lemonade.json`

```json
{
  "name": "Ultra Collection",
  "recipe": "collection",
  "suggested": true,
  "components": [
    "Qwen3.5-35B-A3B-GGUF",
    "Flux-2-Klein-9B-GGUF",
    "Whisper-Large-v3-Turbo",
    "kokoro-v1"
  ]
}
```

The four string entries reference models that already live in Lemonade's curated registry, so no per-component metadata is needed.

### Inline components

A community Lemonade Model can also include components by HF checkpoint and recipe directly:

```json
{
  "name": "My Bundle",
  "recipe": "collection",
  "components": [
    {
      "name": "MyTinyLLM-1B-GGUF",
      "checkpoint": "someuser/MyTinyLLM-1B-GGUF:Q4_K_M.gguf",
      "recipe": "llamacpp",
      "labels": ["tool-calling"],
      "size": 0.7
    },
    "kokoro-v1"
  ]
}
```

Inline components must declare a `recipe` from the allowed set (`llamacpp`, `sd-cpp`, `whispercpp`, `kokoro`, `flm`, `ryzenai-llm`) and a `checkpoint` (or `checkpoints` map). Inline names that collide with a curated registry entry are ignored — the curated entry always wins. Inline components are persisted as `user.`-namespaced models in `user_models.json`.

## Available tools

A Lemonade Model is the bundle; the **tools** it powers are the multimodal endpoints any OpenAI-compatible agent can call. The canonical tool definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json).

| Tool | Endpoint | Needs a model with label |
|------|----------|-------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` or `speech` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `audio` or `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Endpoints Spec](api/README.md).

## Using Lemonade Models from your agent

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](../examples/lemonade_tools.py) shows the full agentic loop — tool definitions, an LLM call with `tools=[...]`, executing each `tool_call` against the corresponding Lemonade endpoint, and feeding the result back. Fewer than 150 lines of Python.

To wire Lemonade Models into your own agent:

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list (or load the JSON directly).
3. When your agent receives a `tool_call` for one of these tools, POST to the corresponding endpoint and feed the response back to the LLM as a `tool` message.
4. If you want to pick component models programmatically rather than rely on a Lemonade Model being loaded, query `GET /v1/models?show_all=true` and match the `labels` array against the "Needs a model with label" column above.

## Publishing your own Lemonade Model

1. Create a Hugging Face repo (model or dataset).
2. Add a `lemonade.json` at the repo root following the schema above.
3. Reference curated Lemonade models by name where possible; use inline objects for anything not yet curated.
4. Test locally by passing the manifest contents in a `lemonade_manifest` field on `POST /api/v1/pull` (see `test/server_collections.py`).
5. Once it's working, share the repo. Anyone can pull it with `lemonade pull <owner>/<repo>`.

Lemonade Models are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "Ultra Collection" as if it were a real model. They surface with `?show_all=true` and appear in the desktop app's model list.
