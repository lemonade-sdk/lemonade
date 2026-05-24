# Project Wiki: 1bit-lemonade (Lemonade)

## Mission
To provide a refreshingly fast local AI server that gives users cloud-like capabilities (chat, coding, speech, image generation) while remaining 100% free and private. It aims to auto-optimize for the user's hardware, with a focus on AMD Ryzen AI, Radeon, and Strix Halo PCs.

## Architecture
- **Inference Stack:** C++ server core with a React-based frontend.
- **Engines:** Integrates multiple state-of-the-art inference engines:
  - `llama.cpp`: LLM inference (Vulkan, ROCm, CPU, Metal).
  - `whisper.cpp`: Speech-to-text (NPU, Vulkan, CPU).
  - `sd-cpp`: Stable Diffusion image generation (ROCm, CPU).
  - `kokoro`: Text-to-speech (CPU).
  - `OnnxRuntime GenAI`: NPU-optimized inference.
- **Form Factors:** Available as a standalone **Lemonade Server** (OpenAI/Anthropic compatible) or as **Embeddable Lemonade** (portable binary).
- **API:** Exposes standard OpenAI-compatible endpoints at `http://localhost:13305/v1`.

## Agent Handoff
- **Setup:** Build from source following `./docs/dev/getting-started.md`.
- **Testing:** Use the `lemonade` CLI:
  - `lemonade backends`: Check supported engines on the current machine.
  - `lemonade list`: See available models.
  - `lemonade run <model>`: Start a model and enter chat.
- **Hot Paths:** The server's request routing logic and the integration layers for the various C++ engines.
- **Current Priorities:** Native multi-modal tool calling, expanding whisper.cpp and SD.cpp backends, and refining the Tauri-based app.

## Decisions & Gotchas
- **Multi-Backend Strategy:** Automatically selects the best backend (Vulkan, ROCm, NPU) based on hardware detection.
- **NPU Support:** XDNA 2 NPU support is a primary focus for Strix Halo, requiring specific drivers and memory lock limits.
- **Portability:** Embeddable version allows bundling the entire stack into 3rd party apps without external dependencies.
- **OpenAI Compatibility:** High priority on maintaining drop-in compatibility for existing AI apps and SDKs.
