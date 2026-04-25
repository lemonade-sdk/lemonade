# Lemonade Endpoints Spec

The Lemonade HTTP service provides a wide array of standards-compliant and custom endpoints.

Our design philosophy is:

1. Ensure that Lemonade works out-of-box with all popular local AI apps.
2. Prioritize using a pre-existing standard for functionality whenever possible.
3. Add sufficient custom functionality to enable developers to build highly polished experiences.

This spec details all supported endpoints. It is organized into pages that correspond to  which organization (OpenAI, Ollama, Lemonade, etc.) defined the endpoints.

| API | Description |
|-----|-------------|
| [OpenAI-Compatible API](./openai.md) | Start here for the main API surface used by most SDKs and clients. |
| [Ollama-Compatible API](./ollama.md) | Use this if your client expects Ollama-style behavior and routes. |
| [Anthropic-Compatible API](./anthropic.md) | Use this for clients built around Anthropic's message format. |
| [llama.cpp-Specific API](./llamacpp.md) | Reference for llama.cpp-specific compatibility and conventions. |
| [Lemonade-Specific API](./lemonade.md) | Local-first API for managing lifecycle, configuration, backends, etc. |
