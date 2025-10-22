# Quick Start Guide

## Prerequisites

1. **Python 3.8+** installed
2. A **GGUF model file** (e.g., from Hugging Face)

That's it! llama-server will be automatically downloaded on first run.

## Installation

```bash
cd lemonade_lean
pip install -e .
```

## Usage

### 1. Start the Server

```bash
# Basic usage (auto-downloads llama.cpp on first run)
lemonade-server serve --model /path/to/your/model.gguf

# With custom settings
lemonade-server serve \
  --model /path/to/your/model.gguf \
  --port 8000 \
  --host 0.0.0.0 \
  --ctx-size 4096 \
  --log-level info
```

**Note:** On first run, llama-server will be automatically downloaded (~100-200 MB) to `~/.lemonade_lean/llama_server/`

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

**Automatic (Recommended):** llama-server is automatically downloaded on first run.

**Manual (Optional):** If you prefer to use your own llama-server:

```bash
# Set environment variable to use custom llama-server
export LLAMA_SERVER_PATH=/path/to/your/llama-server

# Or add llama-server to your PATH
```

The auto-download will:
- Download the appropriate build for your platform (Windows/Linux/macOS)
- Use Vulkan backend on Windows/Linux, Metal on macOS
- Store it in `~/.lemonade_lean/llama_server/`
- Only download once (reused on subsequent runs)

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

- `LLAMA_SERVER_PATH` - Path to llama-server executable (skips auto-download if set)

## Auto-Download Details

On first run, lemonade-lean will:
1. Check if `llama-server` is in PATH or `LLAMA_SERVER_PATH` is set
2. If not found, download the appropriate binary for your platform:
   - **Windows**: Vulkan build (~150 MB)
   - **Linux**: Vulkan build (~120 MB)  
   - **macOS**: Metal build (~100 MB)
3. Extract to `~/.lemonade_lean/llama_server/`
4. Use it automatically for all future runs

This happens only once. Subsequent runs use the cached binary.

## Troubleshooting

### Auto-download fails

If the automatic download fails:

1. Check your internet connection
2. Download manually from: https://github.com/ggerganov/llama.cpp/releases
3. Set the path:

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

