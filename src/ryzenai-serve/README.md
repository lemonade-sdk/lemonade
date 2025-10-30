# Ryzen AI LLM Server

A lightweight, OpenAI API-compatible server for running LLMs on AMD Ryzen AI NPUs using ONNX Runtime GenAI.

## Overview

This server enables running Large Language Models on AMD Ryzen AI 300-series processors with NPU acceleration. It implements the OpenAI API specification, making it compatible with existing LLM applications and tools.

**Key Features:**
- **OpenAI API Compatible:** `/v1/chat/completions`, `/v1/completions`, `/v1/responses`
- **Tool/Function Calling:** OpenAI-compatible function calling support
- **Multiple Execution Modes:** NPU, Hybrid (NPU+iGPU), CPU
- **Streaming Support:** Real-time Server-Sent Events for all endpoints
- **Echo Parameter:** Option to include prompt in completion output
- **Stop Sequences:** Custom stop sequences for generation control
- **Minimal Dependencies:** Single executable + DLLs
- **Simple Architecture:** One-model-per-process design

## Building from Source

### Prerequisites

**Windows Requirements:**
- Windows 11 (64-bit)
- Visual Studio 2022
- CMake 3.20 or higher
- **Ryzen AI Software 1.6.0** with LLM patch
  - Base installation must be at `C:\Program Files\RyzenAI\1.6.0`
  - LLM patch must be applied on top of base installation
  - Download from: https://ryzenai.docs.amd.com

**Hardware Requirements:**
- AMD Ryzen AI 300-series processor (for NPU execution)
- Minimum 16GB RAM (32GB recommended for larger models)

### Build Steps

```cmd
# Navigate to the ryzenai-serve directory
cd src\ryzenai-serve

# Create and enter build directory
mkdir build
cd build

# Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64

# Build
cmake --build . --config Release
```

### Build Output

The executable and required DLLs will be created at:
```
build\bin\Release\ryzenai-serve.exe
```

All necessary Ryzen AI DLLs are automatically copied to the output directory during build.

### Custom Ryzen AI Installation Path

If Ryzen AI is installed in a custom location:

```cmd
cmake .. -G "Visual Studio 17 2022" -A x64 -DOGA_ROOT="C:\custom\path\to\RyzenAI\1.6.0"
```

## Code Structure

```
src/ryzenai-serve/
├── CMakeLists.txt              # Build configuration
│
├── src/                        # Source files
│   ├── main.cpp                # Entry point
│   ├── server.cpp              # HTTP server (cpp-httplib)
│   ├── inference_engine.cpp    # ONNX Runtime GenAI wrapper
│   ├── command_line.cpp        # CLI argument parsing
│   ├── types.cpp               # Data structures
│   ├── tool_calls.cpp          # OpenAI tool/function calling
│   └── handlers/               # HTTP endpoint handlers
│
├── include/ryzenai/            # Headers
│   ├── server.h
│   ├── inference_engine.h
│   ├── command_line.h
│   ├── types.h
│   └── tool_calls.h
│
└── external/                   # Header-only dependencies
    ├── cpp-httplib/            # HTTP server (auto-downloaded)
    └── json/                   # JSON library (auto-downloaded)
```

## Architecture Overview

### Design Principles

1. **Simplicity**: One process serves one model - no dynamic loading/unloading
2. **RAII**: Resource management follows C++ best practices with smart pointers
3. **Thread Safety**: Shared resources protected with proper synchronization
4. **Single Binary**: Minimal dependencies for easy deployment

### Component Layers

```
┌─────────────────────────────────────────────────┐
│         HTTP Server (cpp-httplib)               │
│         OpenAI API Endpoints                    │
├─────────────────────────────────────────────────┤
│         Request Handlers                        │
│         (chat, completions, streaming)          │
├─────────────────────────────────────────────────┤
│         Inference Engine                        │
│         ONNX Runtime GenAI                      │
├─────────────────────────────────────────────────┤
│         Execution Providers                     │
│         NPU / Hybrid / CPU                      │
└─────────────────────────────────────────────────┘
```

**Server:** HTTP server using cpp-httplib with OpenAI-compatible endpoints. Features:
- 8-thread pool for concurrent request handling
- Built-in CORS support (Access-Control-Allow-Origin: *)
- Request routing and response formatting
- Chunked transfer encoding for streaming

**Inference Engine:** Wraps ONNX Runtime GenAI API, managing model loading, generation parameters, and streaming callbacks. Applies chat templates and handles tool call extraction.

**Execution Providers:** Supports three modes:
- **Hybrid**: NPU + iGPU
- **NPU**: Pure NPU execution
- **CPU**: CPU-only fallback

### Dependencies

These dependencies are automatically downloaded during build:

- **cpp-httplib** (v0.26.0) - HTTP server [MIT License]
- **nlohmann/json** (v3.11.3) - JSON parsing [MIT License]

These dependencies must be manually installed by the developer:
- **ONNX Runtime GenAI** - Inference engine

## Usage

### Starting the Server

```cmd
# Specify NPU mode
ryzenai-serve.exe -m C:\path\to\onnx\model --mode npu

# Hybrid mode with custom port
ryzenai-serve.exe -m C:\path\to\onnx\model --mode hybrid --port 8081

# CPU mode
ryzenai-serve.exe -m C:\path\to\onnx\model --mode cpu

# Verbose logging
ryzenai-serve.exe -m C:\path\to\onnx\model --verbose
```

### Command-Line Arguments

- `-m, --model PATH` - Path to ONNX model directory (required)
- `--host ADDRESS` - Server host address (default: 127.0.0.1)
- `-p, --port PORT` - Server port (default: 8080)
- `--mode MODE` - Execution mode: npu, hybrid, cpu (default: hybrid)
- `-c, --ctx-size SIZE` - Context size in tokens (default: 2048)
- `-t, --threads NUM` - Number of CPU threads (default: 4)
- `-v, --verbose` - Enable verbose logging
- `-h, --help` - Show help message

### Model Requirements

Models must be in ONNX format compatible with Ryzen AI. Required files:
- `model.onnx` or `model.onnx.data`
- `genai_config.json`
- Tokenizer files (`tokenizer.json`, `tokenizer_config.json`, etc.)

Models are typically cached in:
```
C:\Users\<Username>\.cache\huggingface\hub\
```

## API Endpoints

The server implements OpenAI-compatible API endpoints. For complete API documentation, request/response formats, and parameters, see the [Lemonade Server API Specification](../../docs/server/server_spec.md).

### Health Check

```bash
GET /health
```

Returns server status and Ryzen AI-specific information:
   ```json
   {
     "status": "ok",
     "model": "phi-3-mini-4k-instruct",
     "execution_mode": "hybrid",
     "max_prompt_length": 4096,
     "ryzenai_version": "1.6.0"
}
```

### Other Endpoints

- `GET /` - Server information and available endpoints
- `POST /v1/chat/completions` - Chat completions with tool/function calling support
- `POST /v1/completions` - Text completions with echo parameter
- `POST /v1/responses` - OpenAI Responses API format

All endpoints support both streaming and non-streaming modes. The server applies chat templates automatically and extracts tool calls from model output.

## Testing

### Quick Test

```cmd
# Start the server
cd build\bin\Release
ryzenai-serve.exe -m C:\path\to\model --verbose

# Test health endpoint (in another terminal)
curl http://localhost:8080/health

# Test chat completion
curl http://localhost:8080/v1/chat/completions ^
  -H "Content-Type: application/json" ^
  -d "{\"messages\": [{\"role\": \"user\", \"content\": \"Hello!\"}], \"max_tokens\": 50}"
```

## Integration Examples

### Python with OpenAI SDK

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost:8080/v1",
    api_key="not-needed"
)

response = client.chat.completions.create(
    model="ignored",  # Model is already loaded
    messages=[
        {"role": "user", "content": "What is 2+2?"}
    ]
)

print(response.choices[0].message.content)
```

## Troubleshooting

### "Model not found" or "Failed to load model"

**Check:**
1. Model path is correct and contains required ONNX files
2. Ryzen AI 1.6.0 is installed at the correct path
3. NPU drivers are up to date (Windows Update)
4. Model is compatible with your Ryzen AI version

### Missing DLLs

All required DLLs should be automatically copied during build. If you get DLL errors:
1. Verify Ryzen AI is installed correctly
2. Rebuild with `cmake --build . --config Release`
3. Manually copy DLLs from `C:\Program Files\RyzenAI\1.6.0\deployment\` to the executable directory

### Port Already in Use

If port 8080 is occupied:
```cmd
ryzenai-serve.exe -m C:\path\to\model --port 8081
```

## Development

### Code Style

- C++17 standard
- RAII for resource management
- Smart pointers (no raw pointers)
- Const correctness
- Snake_case for functions
- PascalCase for types

### Building for Development

Debug build with symbols:
```cmd
cmake --build . --config Debug
```

Debug executable location:
```
build\bin\Debug\ryzenai-serve.exe
```

### Known Issues

**Streaming with JSON Library:** Creating `nlohmann::json` objects directly in ONNX Runtime streaming callbacks can cause crashes. The workaround is to manually construct JSON strings in callbacks. This is stable and performs well.

## Related Projects

- **Ryzen AI Documentation:** https://ryzenai.docs.amd.com
- **ONNX Runtime GenAI:** https://github.com/microsoft/onnxruntime-genai
- **Lemonade Server:** Parent project providing model orchestration

## License

This project is licensed under the Apache 2.0 License. Dependencies use MIT licenses.
