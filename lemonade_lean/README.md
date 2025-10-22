# Lemonade Lean

A minimal version of Lemonade Server with only llama.cpp Vulkan backend support.

## Features

- `lemonade-server serve` - Start the server
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
# Start server with a GGUF model (auto-downloads llama.cpp on first run)
lemonade-server serve --model /path/to/model.gguf

# Custom port and host
lemonade-server serve --model /path/to/model.gguf --port 8080 --host 0.0.0.0
```

On first run, llama-server will be automatically downloaded to `~/.lemonade_lean/llama_server/`

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

