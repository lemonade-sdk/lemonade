# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Lemonade is a local LLM server (v9.4.x) providing GPU and NPU acceleration for running large language models on consumer hardware. It exposes OpenAI-compatible and Ollama-compatible REST APIs and supports multiple backends: llama.cpp, FastFlowLM, RyzenAI, whisper.cpp, stable-diffusion.cpp, and Kokoro TTS.

## Architecture

### Four Executables

- **lemonade-router** — Pure HTTP server. Handles REST API, routes requests to backends, manages model loading/unloading. No CLI.
- **lemonade-server** — CLI client. Commands: `list`, `pull`, `delete`, `run`, `serve`, `status`, `stop`, `logs`. Communicates with router via HTTP.
- **lemonade-tray** — GUI launcher (Windows/macOS/Linux). Starts `lemonade-server serve` without a console. Platform code in `src/cpp/tray/platform/`.
- **lemonade-log-viewer** — Windows-only log file viewer.

### Backend Abstraction

`WrappedServer` (`src/cpp/include/lemon/wrapped_server.h`) is the abstract base class. Each backend inherits it and implements `install()`, `download_model()`, `load()`, `unload()`, and inference methods. Backends run as **subprocesses** — Lemonade forwards HTTP requests to them.

| Backend | Class | Purpose |
|---------|-------|---------|
| llama.cpp | `LlamaCppServer` | LLM inference — CPU/GPU (Vulkan, ROCm, Metal) |
| FastFlowLM | `FastFlowLMServer` | NPU inference (multi-modal: LLM, ASR, embeddings, reranking) |
| RyzenAI | `RyzenAIServer` | Hybrid NPU inference |
| whisper.cpp | `WhisperServer` | Audio transcription |
| stable-diffusion.cpp | `SdServer` | Image generation, editing, variations |
| Kokoro | `KokoroServer` | Text-to-speech |

Capability interfaces: `ICompletionServer`, `IEmbeddingsServer`, `IRerankingServer`, `IAudioServer`, `IImageServer`, `ITextToSpeechServer` (defined in `server_capabilities.h`).

### Router & Multi-Model Support

`Router` (`src/cpp/server/router.cpp`) manages a vector of `WrappedServer` instances. Routes requests based on model recipe, maintains LRU caches per model type (LLM, embedding, reranking, audio), and enforces NPU exclusivity. Configurable via `--max-loaded-models`.

### Model Manager & Recipe System

`ModelManager` (`src/cpp/server/model_manager.cpp`) loads the registry from `src/cpp/resources/server_models.json`. Each model has "recipes" defining which backend and config to use. Backend versions are pinned in `src/cpp/resources/backend_versions.json`. Models download from Hugging Face.

**7 recipe types:** `llamacpp`, `whispercpp`, `sd-cpp`, `ryzenai-llm`, `flm`, `kokoro`, `experience`

Recipe resolution flow: model entry → `recipe` field → `Router::create_backend_server()` instantiates the correct `WrappedServer` subclass → backend subprocess launched. Recipe options cascade: runtime options > per-model `recipe_options` > global defaults (see `recipe_options.cpp`).

### Experience Recipe & Composite Models (Lemonade Ultra/Medium/Lite)

The `experience` recipe is a **virtual composite model** that bundles multiple standalone models into a single selectable experience. It does NOT create a backend — it orchestrates loading/unloading of its component models.

**Three experience tiers defined in `server_models.json`:**

| Experience | LLM | Image | Audio | TTS |
|-----------|-----|-------|-------|-----|
| Lemonade Ultra | Qwen3.5-35B-A3B-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| Lemonade Medium | Qwen3.5-35B-A3B-GGUF | SDXL-Turbo (gen only) | Whisper-Large-v3-Turbo | kokoro-v1 |
| Lemonade Lite | Qwen3.5-4B-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

**JSON structure:**
```json
"Lemonade Ultra": {
    "checkpoint": "",
    "recipe": "experience",
    "composite_models": ["Qwen3.5-35B-A3B-GGUF", "Flux-2-Klein-9B-GGUF", "Whisper-Large-v3-Turbo", "kokoro-v1"]
}
```

**Key behaviors:**
- **No checkpoint** — `checkpoint` is empty; `resolve_model_path()` returns `""` for experience models
- **Device: DEVICE_NONE** — Experience models don't run on hardware directly (`model_types.h`)
- **Download status** — Marked downloaded only when ALL component models are downloaded (`check_composite_downloaded()` in `model_manager.cpp`)
- **Always visible** — Experience models are always shown in model listings regardless of system capabilities (filtered in `model_manager.cpp:1329`)
- **No backend instantiation** — Router never creates a `WrappedServer` for experience models; components load independently

**Frontend orchestration** (`src/app/src/renderer/utils/experienceModels.ts`):
- `isExperienceModel()` — checks `recipe === 'experience'` and non-empty `composite_models`
- `getExperiencePrimaryChatModel()` — finds the LLM component (first model without `NON_LLM_LABELS`: image, speech, tts, audio, transcription, embeddings, reranking)
- `getExperienceImageModel()` — finds the image component (first model with `image` label); used to check edit capability
- `isExperienceFullyDownloaded()` / `isExperienceFullyLoaded()` — check all components
- `ChatWindow.tsx` activates "experience mode" UI, routing chat to the primary LLM component and dispatching `experienceModeChanged` events

### API Routes

All core endpoints are registered under **4 path prefixes**:
- `/api/v0/` — Legacy
- `/api/v1/` — Current
- `/v0/` — Legacy short
- `/v1/` — OpenAI SDK / LiteLLM compatibility

**Core endpoints:** `chat/completions`, `completions`, `embeddings`, `reranking`, `models`, `models/{id}`, `health`, `pull`, `load`, `unload`, `delete`, `params`, `install`, `uninstall`, `audio/transcriptions`, `audio/speech`, `images/generations`, `images/edits`, `images/variations`, `responses`, `stats`, `system-info`, `system-stats`, `log-level`, `logs/stream`

**Ollama-compatible endpoints** (under `/api/` without version prefix): `chat`, `generate`, `tags`, `show`, `delete`, `pull`, `embed`, `embeddings`, `ps`, `version`

Optional API key auth via `LEMONADE_API_KEY` env var. CORS enabled on all routes.

### Desktop & Web App

- **Electron app** — React 19 + TypeScript in `src/app/`. Pure CSS (dark theme), context-based state. Key components: `ChatWindow.tsx`, `ModelManager.tsx`, `DownloadManager.tsx`, `BackendManager.tsx`. Feature panels: LLMChat, ImageGeneration, Transcription, TTS, Embedding, Reranking.
- **Web app** — Browser-only version in `src/web-app/`. Symlinks source from `src/app/src/`. Built via CMake `BUILD_WEB_APP=ON`. Served at `/app`.

### Key Dependencies

**C++ (FetchContent):** cpp-httplib, nlohmann/json, CLI11, libcurl, zstd, IXWebSocket (Windows/Linux), brotli (macOS). Platform SSL: Schannel (Windows), SecureTransport (macOS), OpenSSL (Linux).

**Electron:** React 19, TypeScript 5.3, Webpack 5, Electron 39, markdown-it, highlight.js, katex.

## Build Commands

```bash
# C++ server
cd src/cpp && mkdir build && cd build
cmake ..
cmake --build . --config Release -j

# Electron app
cd src/app && npm install
npm run build:win    # or build:mac / build:linux

# Windows MSI installer
cd src/cpp/build && cmake --build . --config Release --target wix_installer_minimal

# Linux .deb
cd src/cpp/build && cpack
```

CMake presets: `default` (Ninja), `windows` (VS 2022), `debug` (Ninja Debug).

## Testing

Integration tests in Python against a live server:

```bash
pip install -r test/requirements.txt
./src/cpp/build/Release/lemonade-router.exe --port 8000 --log-level debug

# Separate terminal
python test/server_endpoints.py
python test/server_llm.py
python test/server_sd.py
python test/server_whisper.py
python test/server_tts.py
python test/server_system_info.py
python test/server_cli.py
python test/test_ollama.py
```

Test utilities in `test/utils/` with `server_base.py` as the base class.

## Code Style

### C++
- C++17, `lemon::` namespace
- `snake_case` for functions/variables, `CamelCase` for classes/types
- 4-space indent, `#pragma once` for headers
- Platform guards: `#ifdef _WIN32`, `#ifdef __APPLE__`, `#ifdef __linux__`

### Python
- **Black** formatting (v26.1.0, enforced in CI)
- Pylint with `.pylintrc`
- Pre-commit hooks: trailing-whitespace, end-of-file-fixer, check-yaml, check-added-large-files

### TypeScript/React
- React 19, pure CSS (dark theme), context-based state
- UI/frontend changes are handled by core maintainers only

## Key Files

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Root build config (version, deps, targets) |
| `src/cpp/server/server.cpp` | HTTP route registration and all handlers |
| `src/cpp/server/router.cpp` | Request routing and multi-model orchestration |
| `src/cpp/server/model_manager.cpp` | Model registry, downloads, recipe resolution |
| `src/cpp/include/lemon/wrapped_server.h` | Backend abstract base class |
| `src/cpp/include/lemon/server_capabilities.h` | Backend capability interfaces |
| `src/cpp/resources/server_models.json` | Model registry |
| `src/cpp/resources/backend_versions.json` | Backend version pins |
| `src/cpp/server/anthropic_api.cpp` | Anthropic API compatibility |
| `src/cpp/server/ollama_api.cpp` | Ollama API compatibility |
| `src/cpp/tray/tray_app.cpp` | Tray application UI and logic |
| `src/cpp/include/lemon/model_types.h` | ModelType/DeviceType enums, recipe→device mapping |
| `src/cpp/server/recipe_options.cpp` | Per-recipe configurable options and defaults |
| `src/app/src/renderer/ModelManager.tsx` | Model management UI |
| `src/app/src/renderer/ChatWindow.tsx` | Chat interface |
| `src/app/src/renderer/utils/experienceModels.ts` | Experience model helpers (composite detection, primary LLM resolution) |

## Critical Invariants

These MUST be maintained in all changes:

1. **Quad-prefix registration** — Every new endpoint MUST be registered under `/api/v0/`, `/api/v1/`, `/v0/`, AND `/v1/`.
2. **NPU exclusivity** — Only one NPU backend can be loaded at a time. Router must unload existing NPU models before loading a new one.
3. **WrappedServer contract** — New backends MUST implement all virtual methods: `install()`, `download_model()`, `load()`, `unload()`.
4. **Subprocess model** — Backends run as subprocesses (llama-server, whisper-server, sd-server, koko). They must NOT run in-process.
5. **Recipe integrity** — Changes to `server_models.json` must have valid recipes referencing backends in `backend_versions.json`.
6. **Cross-platform** — Code must compile on Windows (MSVC), Linux (GCC/Clang), macOS (AppleClang). Platform-specific code must use `#ifdef` guards.
7. **No hardcoded paths** — Use path utilities. Windows/Linux/macOS paths differ.
8. **Thread safety** — Router serves concurrent HTTP requests. Shared state must be properly guarded.
9. **Ollama compatibility** — Changes to model listing or management must not break `/api/*` Ollama endpoints.
10. **API key passthrough** — When `LEMONADE_API_KEY` is set, all API routes must enforce authentication.
11. **Experience model integrity** — Experience models in `server_models.json` must reference only existing models in their `composite_models` array. Each component must have a valid recipe and checkpoint. The experience model itself must have `"recipe": "experience"` and `"checkpoint": ""`.

## Contributing

- Open an Issue before submitting major PRs
- UI/frontend changes are handled by core maintainers only
- Python formatting with Black is required
- PRs trigger CI for linting, formatting, and integration tests
