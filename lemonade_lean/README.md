# Lemonade Lean

A minimal version of Lemonade Server with only llama.cpp Vulkan backend support.

## Features

- `ls-lean serve` - Start the server
- `ls-lean list-models` - Find GGUF models in your HuggingFace cache
- **Auto-downloads llama.cpp** on first run (Vulkan backend)
- OpenAI-compatible chat/completions API
- No system checks or complex dependencies
- Only built-in Python dependencies

## Installation

```bash
pip install -e .
```

## Usage

```bash
# List GGUF models in your HuggingFace cache
ls-lean list-models

# Start server with a GGUF model - multiple ways:

# 1. Direct file path
ls-lean serve --model /path/to/model.gguf

# 2. HuggingFace repo ID (automatically finds GGUF file in cache)
ls-lean serve --model unsloth/Qwen3-0.6B-GGUF

# 3. HuggingFace repo ID with specific file (if multiple GGUF files exist)
ls-lean serve --model unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf

# Custom port and host
ls-lean serve --model unsloth/Qwen3-0.6B-GGUF --port 8080 --host 0.0.0.0
```

On first run, llama-server will be automatically downloaded to `~/.lemonade_lean/llama_server/`

### Finding Models

The `list-models` command scans your HuggingFace cache for downloaded GGUF models:

```bash
ls-lean list-models
```

This will show all GGUF models in your cache with their full paths. You can then use the repository ID directly with the `serve` command - no need to copy the full path!

## API Endpoints

- `POST /v1/chat/completions` - Chat completions (OpenAI compatible)
- `POST /v1/completions` - Text completions (OpenAI compatible)
- `GET /v1/health` - Health check
- `GET /v1/models` - List loaded model

## Dependencies

- Python 3.8+
- fastapi
- uvicorn

All other dependencies have been removed for maximum lightweightness.

