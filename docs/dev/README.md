# Developing Lemonade

Lemonade is a community-driven project organized around the [Lemonade Discord server](https://discord.gg/5xXzkMu8Zk). That should be your first stop to meet the developers, get support, and propose contributions.

This documentation covers two audiences: developers building apps that consume the Lemonade API, and contributors who want to modify Lemonade itself.

### Building Apps with Lemonade

Start here if you want to build a client app, agent, or service that talks to a running Lemonade server: [Building Apps](./building-apps.md).

For the multimodal OmniRouter pattern and Lemonade Omni Models, see [Lemonade Omni Models](./lemonade-omni.md).

### Code Examples

The [`examples/`](https://github.com/lemonade-sdk/lemonade/tree/main/examples) directory contains runnable demos:

| Example | What it shows |
|---------|--------------|
| `lemonade_tools.py` | Full OmniRouter agentic loop (tool definitions, LLM call, tool dispatch) |
| `realtime_transcription.py` | WebSocket realtime audio transcription |
| `api_image_generation.py` | Image generation via `/v1/images/generations` |
| `api_image_edits.py` | Image editing via `/v1/images/edits` |
| `api_image_variations.py` | Image variations via `/v1/images/variations` |
| `api_text_to_speech.py` | TTS via `/v1/audio/speech` |
| `multi-model-tester.html` | Browser demo: test prompts across multiple models |
| `llm-debate.html` | Browser demo: LLM debate arena |

### Contributor Setup

To build and modify Lemonade itself, start here: [getting started](./getting-started.md).

You can also reference the [app](./app.md) and [web-ui](./web-ui.md) guides to learn more about the GUI side of the project.

### Philosophy

Understand Lemonade's mission and design tenets before contributing by reading the [philosophy](./philosophy.md).

### Roadmap

Lemonade's roadmap is defined by a set of [working groups](./working-groups/README.md), and most substantial contributions should be within the scope of one of these groups.

### Contributing

The Lemonade project welcomes contributions! Learn about the project's mission, maintainers, and contribution process [here](./contribute.md).

### Documentation

Writing or improving docs? Read the [documentation guide](./documentation.md) for style, structure, and guidance on AI-assisted contributions.

### CI System

Lemonade has a CI system that tests pull requests on real AI PC hardware targets. The [self-hosted runners](./self-hosted-runners.md) guide documents how those are set up.
