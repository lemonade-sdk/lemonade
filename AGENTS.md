# AGENTS.md

This file provides guidance to agent driven code reviews when working with this repository.

## Project Overview

Lemonade is a local LLM server providing GPU and NPU acceleration for running large language models on consumer hardware. It exposes OpenAI-compatible, Ollama-compatible, and Anthropic-compatible REST APIs, plus a WebSocket Realtime API. It supports multiple backends: llama.cpp, FastFlowLM, RyzenAI, whisper.cpp, stable-diffusion.cpp, and Kokoro TTS.

## Architecture

### Executables

- **lemond** — Pure HTTP server. Handles REST API, routes requests to backends, manages model loading/unloading. Configured via `config.json` in the lemonade cache directory. CLI args: `[cache_dir] [--port PORT] [--host HOST]`.
- **lemonade** — CLI client (`src/cpp/cli/`). Commands: `list`, `pull`, `delete`, `run`, `status`, `logs`, `launch`, `recipes`, `scan`, etc. Communicates with router via HTTP. Discovers running server via UDP beacon.
- **LemonadeServer.exe** (Windows) — SUBSYSTEM:WINDOWS GUI app that embeds `lemond` and shows a system tray icon. Auto-starts via Windows startup folder.
- **lemonade-tray** (macOS/Linux) — Lightweight tray client that connects to a running `lemond`. Platform code in `src/cpp/tray/platform/`.
- **lemonade-server** — Deprecated backwards-compatibility shim. Delegates to `lemond` or `lemonade`.

### Backend Abstraction

`WrappedServer` (`src/cpp/include/lemon/wrapped_server.h`) is the abstract base class. Each backend inherits it and implements `load()`, `unload()`, `chat_completion()`, `completion()`, `responses()`, and optionally `install()` / `download_model()`. Backends run as **subprocesses** — Lemonade forwards HTTP requests to them.

| Backend | Class | Capabilities | Device | Purpose |
|---------|-------|-------------|--------|---------|
| llama.cpp | `LlamaCppServer` | Completion, Embeddings, Reranking | GPU | LLM inference — CPU/GPU (Vulkan, ROCm, Metal) |
| FastFlowLM | `FastFlowLMServer` | Completion, Embeddings, Reranking, Audio | NPU | NPU inference (multi-modal: LLM, ASR, embeddings, reranking) |
| RyzenAI | `RyzenAIServer` | Completion | NPU | Hybrid NPU inference |
| whisper.cpp | `WhisperServer` | Audio | CPU | Audio transcription |
| stable-diffusion.cpp | `SdServer` | Image | CPU | Image generation, editing, variations |
| Kokoro | `KokoroServer` | TTS | CPU | Text-to-speech |

Capability interfaces: `ICompletionServer`, `IEmbeddingsServer`, `IRerankingServer`, `IAudioServer`, `IImageServer`, `ITextToSpeechServer` (defined in `server_capabilities.h`). Use `supports_capability<T>(server)` template for runtime checks.

### Router & Multi-Model Support

`Router` (`src/cpp/server/router.cpp`) manages a vector of `WrappedServer` instances. Routes requests based on model recipe, maintains LRU caches per model type (LLM, embedding, reranking, audio, image, TTS — see `model_types.h`), and enforces NPU exclusivity. Configurable via `--max-loaded-models`. On non-file-not-found errors, the router uses a "nuclear option" — evicts all models and retries the load.

### Model Manager & Recipe System

`ModelManager` (`src/cpp/server/model_manager.cpp`) loads the registry from `src/cpp/resources/server_models.json`. Each model has "recipes" defining which backend and config to use. Backend versions are pinned in `src/cpp/resources/backend_versions.json`. Models download from Hugging Face.

### API Routes

All core endpoints are registered under **4 path prefixes**:
- `/api/v0/` — Legacy
- `/api/v1/` — Current
- `/v0/` — Legacy short
- `/v1/` — OpenAI SDK / LiteLLM compatibility

**Core endpoints:** `chat/completions`, `completions`, `embeddings`, `reranking`, `models`, `models/{id}`, `health`, `pull`, `load`, `unload`, `delete`, `params`, `install`, `uninstall`, `audio/transcriptions`, `audio/speech`, `images/generations`, `images/edits`, `images/variations`, `responses`, `stats`, `system-info`, `system-stats`, `log-level`, `logs/stream`

**Ollama-compatible endpoints** (under `/api/` without version prefix): `chat`, `generate`, `tags`, `show`, `delete`, `pull`, `embed`, `embeddings`, `ps`, `version`

**Anthropic-compatible endpoint:** `POST /api/messages` — supports message completion, tool use, and SSE streaming.

**WebSocket Realtime API**: OpenAI-compatible Realtime protocol for real-time audio transcription. Binds to an OS-assigned port (9000+), exposed via the `websocket_port` field in the `/health` endpoint response.

**Internal endpoints:** `POST /internal/shutdown`

Optional API key auth via `LEMONADE_API_KEY` env var. CORS enabled on all routes.

### Desktop & Web App

- **Electron app** — React 19 + TypeScript in `src/app/`. Pure CSS (dark theme), context-based state. Key components: `ChatWindow.tsx`, `ModelManager.tsx`, `DownloadManager.tsx`, `BackendManager.tsx`. Feature panels: LLMChat, ImageGeneration, Transcription, TTS, Embedding, Reranking.
- **Web app** — Browser-only version in `src/web-app/`. Symlinks source from `src/app/src/`. Built via CMake `BUILD_WEB_APP=ON`. Served at `/app`.

### Key Dependencies

**C++ (FetchContent):** cpp-httplib, nlohmann/json, CLI11, libcurl, zstd, libwebsockets, brotli (macOS). Platform SSL: Schannel (Windows), SecureTransport (macOS), OpenSSL (Linux).

**Electron:** React 19, TypeScript 5.3, Webpack 5, Electron 39, markdown-it, highlight.js, katex.

## Build Commands

CMakeLists.txt is at the repository root. Build uses CMake presets — run the setup script first, then build with `--preset`.

```bash
# 1. Setup (configures build directory and installs deps)
./setup.sh          # Linux / macOS
./setup.ps1         # Windows (PowerShell)

# 2. Build C++ server
cmake --build --preset default          # Linux / macOS (Ninja)
cmake --build --preset windows          # Windows (Visual Studio 2022)
cmake --build --preset vs18             # Windows (Visual Studio 2026)

# 3. Electron app (optional, requires Node.js 20+)
cmake --build --preset default --target electron-app    # Linux / macOS
cmake --build --preset windows --target electron-app    # Windows (VS 2022)
cmake --build --preset vs18 --target electron-app       # Windows (VS 2026)

# 4. Web app (auto-built on non-Windows; manual on Windows)
cmake --build --preset default --target web-app         # Linux / macOS
cmake --build --preset windows --target web-app         # Windows

# 5. Windows MSI installer (WiX 5.0+ required)
cmake --build --preset windows --target wix_installer_minimal  # server + web-app
cmake --build --preset windows --target wix_installer_full     # server + electron + web-app

# 6. macOS signed installer
cmake --build --preset default --target package-macos

# 7. Linux .deb / .rpm
cd build && cpack            # .deb
cd build && cpack -G RPM     # .rpm

# 8. Linux AppImage
cmake --build --preset default --target appimage
```

CMake presets: `default` (Ninja, Release), `windows` (VS 2022), `vs18` (VS 2026), `debug` (Ninja, Debug).

CMake options: `BUILD_WEB_APP` (ON by default on non-Windows), `BUILD_ELECTRON_APP` (Linux only, include Electron in deb), `LEMONADE_SYSTEMD_UNIT_NAME` (default: `lemonade-server.service`).

## Testing

Integration tests in Python against a live server. Tests auto-discover the server binary from the build directory; use `--server-binary` to override.

```bash
pip install -r test/requirements.txt

# CLI tests (no inference backend needed)
python test/server_cli.py

# Endpoint tests (no inference backend needed)
python test/server_endpoints.py

# LLM tests (specify wrapped server and backend)
python test/server_llm.py --wrapped-server llamacpp --backend vulkan

# Audio transcription tests
python test/server_whisper.py

# Image generation tests (slow)
python test/server_sd.py
```

Test utilities in `test/utils/` with `server_base.py` as the base class. Test dependencies include `requests`, `httpx`, `openai`, `huggingface_hub`, `psutil`, `numpy`, `websockets`, and `ollama`.

## Code Style

### C++
- C++17, `lemon::` namespace
- `snake_case` for functions/variables, `CamelCase` for classes/types
- 4-space indent, `#pragma once` for headers
- Keep `#include` directives in alphabetical order within each include block
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
| `src/cpp/include/lemon/websocket_server.h` | WebSocket Realtime API server |
| `src/cpp/include/lemon/model_types.h` | Model type and device type enums |
| `src/cpp/include/lemon/config_file.h` | config.json load/save/migrate |
| `src/cpp/include/lemon/recipe_options.h` | Per-recipe JSON configuration |
| `src/cpp/tray/tray_app.cpp` | Tray application UI and logic |
| `src/app/src/renderer/ModelManager.tsx` | Model management UI |
| `src/app/src/renderer/ChatWindow.tsx` | Chat interface |

## Critical Invariants

These MUST be maintained in all changes:

1. **Quad-prefix registration** — Every new endpoint MUST be registered under `/api/v0/`, `/api/v1/`, `/v0/`, AND `/v1/`.
2. **NPU exclusivity** — Exclusive-NPU recipes (`ryzenai-llm`, `whispercpp` on NPU) evict ALL other NPU models before loading. FastFlowLM (`flm`) can coexist with other FLM types (max 1 per FLM type) but not with exclusive-NPU recipes.
3. **WrappedServer contract** — New backends MUST implement all core virtual methods: `load()`, `unload()`, `chat_completion()`, `completion()`, `responses()`.
4. **Subprocess model** — Backends run as subprocesses (llama-server, whisper-server, sd-server, koko, flm, ryzenai-server). They must NOT run in-process.
5. **Recipe integrity** — Changes to `server_models.json` must have valid recipes referencing backends in `backend_versions.json`.
6. **Cross-platform** — Code must compile on Windows (MSVC), Linux (GCC/Clang), macOS (AppleClang). Platform-specific code must use `#ifdef` guards.
7. **No hardcoded paths** — Use path utilities. Windows/Linux/macOS paths differ.
8. **Thread safety** — Router serves concurrent HTTP requests. Shared state must be properly guarded.
9. **Ollama compatibility** — Changes to model listing or management must not break `/api/*` Ollama endpoints.
10. **API key passthrough** — When `LEMONADE_API_KEY` is set, all API routes must enforce authentication.

## Contributing

- Open an Issue before submitting major PRs
- UI/frontend changes are handled by core maintainers only
- Python formatting with Black is required
- PRs trigger CI for linting, formatting, and integration tests
