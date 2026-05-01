# OmniRouter

OmniRouter is Lemonade's approach to multimodal agentic workflows. Instead of building a proprietary agent runtime into Lemonade, each supported modality is exposed as an **OpenAI-compatible tool** that an existing LLM agent (Continue, OpenHands, Claude Code, your own app) can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the local tools.

## How it works

1. Describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client executes each `tool_call` against the corresponding Lemonade endpoint, such as `/v1/images/generations` or `/v1/audio/speech`.
4. The client sends the tool result back to the LLM as a `tool` message.
5. The LLM continues until it either calls another tool or returns a final response.

The tool schemas used by OmniRouter are plain JSON. They do not require a Lemonade-specific client library, and the endpoints they target use OpenAI-compatible request and response shapes.

## Collections

A **Collection** is a preconfigured bundle of models sized for a hardware tier. Selecting a collection in the Lemonade desktop app loads one LLM plus the tool models needed for the OmniRouter workflow.

| Collection | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **Ultra Collection** | Qwen3.5-35B-A3B-GGUF | Flux-2-Klein-9B-GGUF (generation + editing) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **Lite Collection** | Qwen3.5-4B-GGUF | SD-Turbo (generation only) | Whisper-Tiny | kokoro-v1 |

Collections are hidden from the default `/v1/models` listing so OpenAI-compatible clients do not see a collection as if it were a single concrete model. They are returned by `GET /v1/models?show_all=true` and are shown in the Lemonade desktop app's model list.

Use a collection when you want a recommended default bundle. The desktop app, the built-in tool definitions, and the example client can all use these predefined bundles.

## Custom workflows in the desktop app

Custom workflows let you create the same OmniRouter-style experience with your own downloaded model mix.

1. Download the concrete models you want to use in **Model Manager**.
2. In **Model Manager**, expand **OmniRouter**.
3. Click **Create custom workflow**.
4. Pick one LLM and any optional models for image generation, image editing, vision analysis, speech-to-text, and text-to-speech.
5. Save the workflow.
6. Select the new `workflow.<name>` entry in the chat model picker.

Custom workflows behave like lightweight local collections. They are stored by the desktop app, layered on top of `GET /v1/models?show_all=true`, and shown in the chat picker with a `workflow.` prefix.

Custom workflows do **not** register a synthetic server-side model and do **not** change the OpenAI-compatible `/v1/models` response. The selected LLM remains the planner that decides when to call tools. Optional role models are only used when their corresponding tool is called.

The workflow editor only offers already-downloaded compatible models for each role:

| Workflow role | Tool unlocked | Required model capability |
|---------------|---------------|---------------------------|
| LLM | Chat loop and tool calls | Concrete chat model, preferably tool-calling capable |
| Vision / image analysis | `analyze_image` | `vision` label |
| Image generation | `generate_image` | `image` label |
| Image editing | `edit_image` | `edit` label |
| Speech-to-text | `transcribe_audio` | `audio` or `transcription` label |
| Text-to-speech | `text_to_speech` | `tts` or `speech` label |

If a component model is deleted or renamed later, the workflow remains saved in local storage but is hidden from the model picker until all referenced component models are available again.

## Available tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json). This keeps the desktop app and this documentation aligned on the same tool names, descriptions, parameters, and endpoints.

| Tool | Endpoint | Needs a model with label |
|------|----------|--------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` or `speech` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `audio` or `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | `vision` |

Endpoint request and response shapes are documented in the [API docs](api/README.md).

## Quick start

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](../examples/lemonade_tools.py) shows the full agentic loop: tool definitions, an LLM request with `tools=[...]`, execution of each `tool_call`, and feeding each result back to the LLM.

The example script is a standalone OpenAI-compatible client. It does not read desktop `localStorage`, so it will not automatically see desktop-only `workflow.*` entries. For Python tests, use one of the built-in collections or adapt the script's tool-to-model mapping explicitly.

## Using your own agent

To integrate OmniRouter into an existing agent:

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list, or load the JSON directly.
3. When your agent receives a `tool_call` for one of these tools, call the corresponding Lemonade endpoint from the table above.
4. Feed the endpoint response back to the LLM as a `tool` message.
5. Continue the loop until the LLM returns a final response.

If your client needs to choose models programmatically, query `GET /v1/models?show_all=true` and match the `labels` array against the required model capabilities in the table above.

## Testing custom workflows

### Automated unit test

The desktop app includes a focused Node-based smoke test for the custom workflow utility layer:

```bash
cd src/app
npm run test:custom-workflows
```

This test runs without starting Tauri or the Lemonade server. It uses a fake `localStorage` and verifies that custom workflows can be saved, edited, deleted, imported, exported, merged into model metadata, hidden when a component is stale, and filtered by compatible workflow role.

A successful run ends with:

```text
All custom workflow tests passed (4/4).
```

To confirm the test can catch a regression, temporarily change `CUSTOM_WORKFLOW_PREFIX` in `src/renderer/utils/customWorkflows.ts` from `workflow.` to another value, rerun `npm run test:custom-workflows`, and verify that the test fails. Revert the change afterwards.

### Manual desktop smoke test

Use the desktop app to verify the user-facing flow end to end:

1. Start the Lemonade desktop app.
2. Download at least one chat-capable LLM in **Model Manager**.
3. Optionally download one image model, one edit-capable image model, one vision model, one transcription model, and one speech model.
4. Open **Model Manager > OmniRouter**.
5. Click **Create custom workflow**.
6. Save a workflow with only an LLM and verify it appears as `workflow.<name>` in the chat model picker.
7. Edit the workflow to add optional role models and save again.
8. Select the workflow in chat and run prompts that trigger the configured tools, such as image generation, speech synthesis, audio transcription, or image analysis.
9. Export the workflow JSON, delete the workflow, import the JSON, and verify that the workflow reappears.
10. Delete or rename one component model and verify that the stale workflow is hidden from the picker until the component model is available again.
