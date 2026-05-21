# Lemonade Omni Models

**Lemonade Omni Models** are one-click multimodal model bundles. A single selection in the Lemonade desktop app loads one LLM, one image model, one ASR model, and one TTS model — everything a multimodal agent needs to chat, generate images, transcribe audio, and speak responses out loud.

Under the hood, Lemonade Omni Models are powered by **OmniRouter** — Lemonade's pattern for exposing each modality as an OpenAI-compatible tool that an existing LLM agent (Continue, OpenHands, Claude Code, your own app) can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the local tools.

## How OmniRouter works

1. Describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client executes each `tool_call` against the corresponding Lemonade endpoint, such as `/v1/images/generations` or `/v1/audio/speech`.
4. The client sends the tool result back to the LLM as a `tool` message.
5. The LLM continues until it either calls another tool or returns a final response.

The tool schemas OmniRouter provides are plain JSON. They do not require a Lemonade-specific client library, and the endpoints they target use OpenAI-compatible request and response shapes.

## The omni models

An **omni model** is a meta-model made up of components, registered with `recipe: "collection.omni"`. Selecting one in the Lemonade desktop app loads every component in a single click. Lemonade ships these:

| Omni model | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **LMN-Halo-Omni-52B** | Qwen3.6-35B-A3B-MTP-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **LMN-Lite-Omni-5.5B** | Qwen3.5-4B-MTP-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Omni models are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "LMN-Halo-Omni-52B" as if it were a real model. They surface with `?show_all=true` and appear in the desktop app's model list under the **Lemonade Omni** category.

### Naming scheme decoder ring

Omni model names follow the pattern `LMN-<class>-Omni-<xB>`:

| Component | Value | Meaning |
|-----------|-------|---------|
| Org prefix | `LMN` | Lemonade org. |
| Class | `Halo` | Based on a large MoE LLM (e.g., targeted at Strix Halo). |
|  | `Lite` | Based on small models targeted at 32 GB APUs. |
|  | `Dense` | Based on a dense LLM targeted at 32 GB dGPUs (none shipped yet). |
| Modality | `Omni` | True all-to-all omni-modal bundle. |
| Size | `xB` | Total parameter count across all component models. |

### Use an omni model

Every part of this doc assumes one is loaded — the desktop app, [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py), and the tools themselves were all validated against the two omni models above.

If you're the developer wiring OmniRouter into your own agent and you want to substitute models, you can, but you take on the compatibility work: any LLM you swap in must carry the `tool-calling` label, and each tool you want to call needs one downloaded model whose `labels` include the row's "Needs a model with label" entry from the tools table below. That's a developer-path discovery step, not a user configuration; the simple answer for everyone else is "install an omni model."

## Custom Omni Models in the desktop app

Custom Omni Models let you create the same OmniRouter-style experience with your own mix of registered models — useful when you want to swap in a different planner LLM or a different image/ASR/TTS backbone without waiting for a new built-in omni model to ship.

1. Register or download the concrete models you want to use in **Model Manager**.
2. In the desktop app menu, open **Lemonade > New Omni Model > Manually** (or **From JSON** to import an exported one).
3. Pick one planner LLM and any optional models for image generation, image editing, vision analysis, speech-to-text, and text-to-speech.
4. Save the Omni Model.
5. Select the new `user.<name>` entry in the chat model picker — it appears alongside the built-in omni models under the **Lemonade Omni** category.

Custom Omni Models are registered through the same `POST /v1/pull` path with `recipe: "collection.omni"` that the built-ins use. They live under the server's `user.*` namespace, so a custom Omni Model named `MyKit` is addressable as `user.MyKit`. They behave like built-in omni models for routing purposes: the selected planner LLM remains the loop driver that decides when to call tools, and optional role models are only loaded/used when their corresponding tool is called.

The Omni Model editor only offers already-registered compatible models for each role:

| Omni Model role | Tool unlocked | Required model capability |
|---------------|---------------|---------------------------|
| LLM | Chat loop and tool calls | Concrete chat model, preferably tool-calling capable |
| Vision / image analysis | `analyze_image` | `vision` label |
| Image generation | `generate_image` | `image` label |
| Image editing | `edit_image` | `edit` label |
| Speech-to-text | `transcribe_audio` | `audio` or `transcription` label |
| Text-to-speech | `text_to_speech` | `tts` or `speech` label |

If a component model is deleted later, the Omni Model entry remains registered but is hidden from the chat picker until every referenced component is available again.

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

## Testing custom Omni Models

### Automated unit test

The desktop app includes a focused Node-based smoke test for the custom Omni Model utility layer:

```bash
cd src/app
npm run test:custom-collections
```

This test runs without starting Tauri or the Lemonade server. It exercises the helpers in [`src/app/src/renderer/utils/customCollections.ts`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/customCollections.ts) — saving, editing, importing, exporting, and filtering Omni Models by compatible component role.

### Manual desktop smoke test

Use the desktop app to verify the user-facing flow end to end:

1. Start the Lemonade desktop app.
2. Download at least one chat-capable LLM in **Model Manager**.
3. Optionally download one image model, one edit-capable image model, one vision model, one transcription model, and one speech model.
4. Open the **Lemonade** menu and choose **New Omni Model > Manually**.
5. Save an Omni Model with only an LLM and verify it appears as `user.<name>` in the chat model picker.
6. Edit the Omni Model to add optional role models and save again.
7. Select the Omni Model in chat and run prompts that trigger the configured tools, such as image generation, speech synthesis, audio transcription, or image analysis.
8. Export the Omni Model JSON, delete the Omni Model, import the JSON, and verify it reappears.
9. Delete one component model and verify the now-stale Omni Model is hidden from the picker until the component is registered again.
