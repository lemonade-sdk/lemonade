# Quick Start Guide

## Prerequisites

1. **Python 3.8+** installed
2. **llama.cpp** with Vulkan support compiled and `llama-server` in PATH
   - Or set `LLAMA_SERVER_PATH` environment variable to point to llama-server executable
3. A **GGUF model file** (e.g., from Hugging Face)

## Installation

```bash
cd lemonade_lean
pip install -e .
```

## Usage

### 1. Start the Server

```bash
# Basic usage
lemonade-server serve --model /path/to/your/model.gguf

# With custom settings
lemonade-server serve \
  --model /path/to/your/model.gguf \
  --port 8000 \
  --host 0.0.0.0 \
  --ctx-size 4096 \
  --log-level info
```

### 2. Test the Server

In another terminal:

```bash
# Health check
curl http://localhost:8000/v1/health

# Chat completion
curl http://localhost:8000/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "Hello!"}],
    "max_tokens": 100
  }'
```

### 3. Run the Example Client

```bash
python examples/simple_client.py
```

## Getting llama.cpp

### Option 1: Use Pre-built Binaries

Download from: https://github.com/ggerganov/llama.cpp/releases

### Option 2: Build from Source

```bash
git clone https://github.com/ggerganov/llama.cpp.git
cd llama.cpp

# Build with Vulkan support
cmake -B build -DGGML_VULKAN=ON
cmake --build build --config Release

# The llama-server binary will be in build/bin/
export LLAMA_SERVER_PATH=$(pwd)/build/bin/llama-server
```

### Option 3: Install via Package Manager

Some package managers provide llama.cpp:

```bash
# Homebrew (macOS)
brew install llama.cpp

# After installation, llama-server should be in PATH
```

## Getting Models

Download GGUF models from Hugging Face:

```bash
# Example: Small model for testing
wget https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf

# Start server with this model
lemonade-server serve --model tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
```

## Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `--model` | (required) | Path to GGUF model file |
| `--port` | 8000 | Port to serve on |
| `--host` | localhost | Host to bind to |
| `--ctx-size` | 4096 | Context window size |
| `--log-level` | info | Log level (debug, info, warning, error) |

## API Endpoints

All endpoints are OpenAI-compatible:

- `GET /v1/health` - Health check
- `GET /v1/models` - List loaded model
- `POST /v1/chat/completions` - Chat completions
- `POST /v1/completions` - Text completions

## Environment Variables

- `LLAMA_SERVER_PATH` - Path to llama-server executable
- `LEMONADE_PORT` - Default port (overridden by --port)
- `LEMONADE_HOST` - Default host (overridden by --host)

## Troubleshooting

### llama-server not found

Set the path explicitly:

```bash
export LLAMA_SERVER_PATH=/path/to/llama-server
lemonade-server serve --model model.gguf
```

### Port already in use

Use a different port:

```bash
lemonade-server serve --model model.gguf --port 8001
```

### Model loading fails

Check:
1. Model file exists and is readable
2. You have enough RAM/VRAM
3. llama-server supports your GPU (Vulkan)

Enable debug logging:

```bash
lemonade-server serve --model model.gguf --log-level debug
```

## What's Removed vs Full Lemonade

This lean version removes:

- ❌ System checks and device enumeration
- ❌ Model downloading/management
- ❌ Multiple backends (only Vulkan)
- ❌ Web UI
- ❌ Tray icon
- ❌ Heavy dependencies (transformers, torch, etc.)
- ❌ Profiling tools
- ❌ Multiple server types
- ❌ Embeddings and reranking endpoints

What's kept:

- ✅ `lemonade-server serve` command
- ✅ llama.cpp Vulkan backend
- ✅ OpenAI-compatible API
- ✅ Streaming support
- ✅ Minimal dependencies (fastapi, uvicorn, pydantic, requests)

