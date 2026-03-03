# Lemonade Project Context for Claude Code Reviews

## Project Overview

Lemonade is a local LLM server providing GPU and NPU acceleration for running large language models on consumer hardware. It exposes an OpenAI-compatible REST API and supports multiple backends (llama.cpp, FastFlowLM, RyzenAI, whisper.cpp, stable-diffusion.cpp, Kokoro TTS).

## Architecture

### Four Executables

1. **lemonade-router** - Pure HTTP server. Handles OpenAI-compatible REST API, routes requests to backends, manages model loading/unloading. No CLI interface.
2. **lemonade-server** - CLI client. Commands: `list`, `pull`, `delete`, `run`, `serve`, `status`, `stop`. Communicates with router via HTTP. Manages server lifecycle.
3. **lemonade-tray** - Windows/macOS GUI launcher. Starts `lemonade-server serve` with no console window. Platform-specific implementations in `src/cpp/tray/platform/`.
4. **lemonade-log-viewer** - Windows-only log file viewer.

### Backend Abstraction

`WrappedServer` (defined in `src/cpp/include/lemon/wrapped_server.h`) is the abstract base class. Each backend inherits from it and implements `install()`, `download_model()`, `load()`, `unload()`, and inference methods:

- `LlamaCppServer` - llama.cpp for CPU/GPU (Vulkan, ROCm, Metal)
- `FastFlowLMServer` - NPU inference (multi-modal with ASR + embeddings)
- `RyzenAIServer` - Hybrid NPU inference
- `WhisperServer` - Audio transcription
- `SdServer` - Stable Diffusion image generation
- `KokoroServer` - Text-to-speech

### Router & Multi-Model Support

The `Router` manages a vector of `WrappedServer` instances. It routes requests based on model recipe, maintains separate LRU caches per model type (LLM, embedding, reranking, audio), and enforces NPU exclusivity. Configurable via `--max-loaded-models`.

### Model Manager & Recipe System

`ModelManager` loads the model registry from `src/cpp/resources/server_models.json`. Each model has "recipes" that define which backend and configuration to use. Backend versions are pinned in `src/cpp/resources/backend_versions.json`. Models are downloaded from Hugging Face.

### API Routes

All core endpoints are registered under 4 path variations:
- `/api/v0/` - Legacy
- `/api/v1/` - Current standard
- `/v0/` - Legacy short form
- `/v1/` - LiteLLM / OpenAI SDK compatibility

Core endpoints: `/chat/completions`, `/completions`, `/embeddings`, `/reranking`, `/models`, `/health`, `/pull`, `/delete`, `/load`, `/unload`, `/audio/transcriptions`, `/audio/speech`, `/images/generations`, `/images/edits`, `/images/variations`, `/responses`, `/stats`, `/system-info`, `/system-stats`, `/install`, `/uninstall`, `/params`, `/log-level`, `/logs/stream`

Ollama-compatible endpoints (under `/api/` without version prefix): `/api/chat`, `/api/generate`, `/api/tags`, `/api/show`, `/api/delete`, `/api/pull`, `/api/embed`, `/api/embeddings`, `/api/ps`, `/api/version`

Optional API key authentication via `LEMONADE_API_KEY` environment variable. CORS is enabled on all routes.

### Electron & Web App

The desktop app is React 19 + TypeScript in `src/app/`. Key components: `ChatWindow.tsx` (chat UI), `ModelManager.tsx` (model management), `DownloadManager.tsx` (download tracking). The web-app (`src/web-app/`) symlinks source from `src/app/src/` and builds a browser-only version served at `/app`.

### Key Dependencies (CMake FetchContent)

- cpp-httplib (HTTP server), nlohmann/json, CLI11, libcurl, zstd
- Platform SSL: Schannel (Windows), SecureTransport (macOS), OpenSSL (Linux)

## Code Style & Conventions

### C++
- C++17 standard, `lemon::` namespace
- `snake_case` for functions/variables, `CamelCase` for classes/types
- 4-space indentation, `#pragma once` for headers

### Python
- Black formatting (v26.1.0, enforced in CI)
- Pylint compliance with `.pylintrc`
- Pre-commit hooks: trailing-whitespace, end-of-file-fixer, check-yaml, check-added-large-files

### TypeScript/React
- React 19, pure CSS (dark theme), context-based state management
- UI/frontend changes are handled by core maintainers only

## Key Files

- `CMakeLists.txt` - Root build configuration (project version, dependencies, build targets)
- `src/cpp/server/server.cpp` - HTTP route registration and server setup
- `src/cpp/server/router.cpp` - Request routing and multi-model orchestration
- `src/cpp/server/model_manager.cpp` - Model registry and downloads
- `src/cpp/include/lemon/wrapped_server.h` - Backend interface definition
- `src/cpp/resources/server_models.json` - Model registry data
- `src/cpp/resources/backend_versions.json` - Backend version pinning
- `src/cpp/tray/tray_app.cpp` - Tray application UI and logic

## Critical Invariants (MUST be enforced in reviews)

1. **Quad-prefix registration**: Every new endpoint MUST be registered under `/api/v0/`, `/api/v1/`, `/v0/`, AND `/v1/`.
2. **NPU exclusivity**: Only one NPU backend can be loaded at a time. The router must unload existing NPU models before loading a new one.
3. **WrappedServer contract**: New backends MUST implement all virtual methods from `WrappedServer` (`install()`, `download_model()`, `load()`, `unload()`).
4. **Backend subprocess model**: Backends run as subprocesses (llama-server, whisper, sd-server, kokoro). Lemonade forwards HTTP requests. Backends must NOT run in-process.
5. **Model recipe integrity**: Changes to `server_models.json` must have valid recipe structures and reference backends defined in `backend_versions.json`.
6. **Cross-platform builds**: Code must compile on Windows (MSVC), Linux (GCC/Clang), and macOS (AppleClang). Platform-specific code must be properly guarded with `#ifdef`.
7. **No hardcoded paths**: Use the path utilities in the codebase. Windows/Linux/macOS paths differ.
8. **Thread safety**: The router serves concurrent HTTP requests. Shared state must be properly guarded.
9. **Ollama compatibility**: Changes to model listing or management must not break the Ollama-compatible `/api/*` endpoints.

## Testing

Tests are Python-based integration tests in `test/`. Key test files: `server_endpoints.py`, `server_llm.py`, `server_sd.py`, `server_whisper.py`, `server_tts.py`, `server_system_info.py`, `server_cli.py`. Test utilities in `test/utils/` with `server_base.py` providing the base test class.

## Contributing Rules

- Open an Issue before submitting major PRs
- UI/frontend changes are handled by core maintainers only
- Python formatting with Black is required
- PRs trigger CI for linting, formatting, unit tests, and end-to-end tests
