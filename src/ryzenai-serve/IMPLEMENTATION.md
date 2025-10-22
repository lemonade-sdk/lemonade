# Implementation Summary

This document provides an overview of the Ryzen AI LLM Server implementation.

## Project Structure

```
ryzenai-serve/
├── CMakeLists.txt              # Build configuration
├── README.md                   # Architecture documentation
├── BUILD.md                    # Build instructions
├── QUICKSTART.md               # Quick start guide
├── IMPLEMENTATION.md           # This file
├── .gitignore                  # Git ignore rules
├── test_server.py              # Python test script
│
├── include/ryzenai/            # Public headers
│   ├── types.h                 # Common types and structures
│   ├── command_line.h          # CLI argument parsing
│   ├── inference_engine.h      # ONNX GenAI wrapper
│   └── server.h                # HTTP server
│
├── src/                        # Implementation files
│   ├── main.cpp                # Entry point
│   ├── types.cpp               # Type implementations
│   ├── command_line.cpp        # CLI parsing implementation
│   ├── inference_engine.cpp    # ONNX GenAI integration
│   ├── server.cpp              # HTTP server implementation
│   └── handlers/               # (Future: separate handler files)
│
└── external/                   # Downloaded dependencies
    ├── cpp-httplib/            # HTTP server library
    └── json/                   # JSON parsing library
```

## Core Components

### 1. Command Line Interface (`command_line.h/cpp`)

**Purpose**: Parse and validate command line arguments

**Key Features**:
- Simple argument parsing with `-m` for model path
- Support for `--port`, `--host`, `--mode`, etc.
- Built-in help (`-h`, `--help`)
- Validation of arguments

**Usage**:
```cpp
CommandLineArgs args = CommandLineParser::parse(argc, argv);
```

### 2. Type Definitions (`types.h/cpp`)

**Purpose**: Define common data structures for requests and responses

**Key Types**:
- `CommandLineArgs`: Server configuration
- `CompletionRequest`: Text completion request (OpenAI format)
- `ChatCompletionRequest`: Chat completion request (OpenAI format)
- `ChatMessage`: Individual chat message
- `GenerationParams`: ONNX GenAI generation parameters
- `StreamCallback`: Callback function type for streaming

**Key Methods**:
- `CompletionRequest::fromJSON()`: Parse JSON to request
- `ChatCompletionRequest::toPrompt()`: Convert chat to prompt string

### 3. Inference Engine (`inference_engine.h/cpp`)

**Purpose**: Wrapper around ONNX Runtime GenAI for NPU/Hybrid inference

**Key Features**:
- Model loading and validation
- NPU and Hybrid execution mode support
- `rai_config.json` parsing for version-specific parameters
- Automatic prompt truncation based on max length
- Thread-safe inference with mutex
- Both synchronous and streaming generation

**Key Methods**:
- `InferenceEngine(model_path, mode)`: Constructor, loads model
- `complete()`: Synchronous text generation
- `streamComplete()`: Streaming text generation with callback
- `loadRaiConfig()`: Load Ryzen AI specific configuration
- `truncatePrompt()`: Truncate prompts exceeding max length
- `detectRyzenAIVersion()`: Detect installed Ryzen AI version

**Ryzen AI Integration**:
- Detects Ryzen AI version (1.5.0, 1.6.0, etc.)
- Reads `rai_config.json` for version-specific `max_prompt_length`
- Auto-truncates prompts to fit within limits
- Configures execution providers (NPU/Hybrid)

### 4. HTTP Server (`server.h/cpp`)

**Purpose**: HTTP server with OpenAI API compatible endpoints

**Key Features**:
- Built on cpp-httplib
- CORS support for web clients
- Server-Sent Events (SSE) for streaming
- Comprehensive error handling
- Request logging

**Endpoints Implemented**:
- `GET /health`: Health check with model info
- `POST /v1/completions`: Text completion (streaming & non-streaming)
- `POST /v1/chat/completions`: Chat completion (streaming & non-streaming)
- `GET /`: Server information

**Key Methods**:
- `RyzenAIServer(args)`: Constructor, loads model and sets up routes
- `run()`: Start the HTTP server (blocking)
- `stop()`: Gracefully stop the server
- `handleHealth()`: Health endpoint handler
- `handleCompletions()`: Completions endpoint handler
- `handleChatCompletions()`: Chat completions endpoint handler

### 5. Main Entry Point (`main.cpp`)

**Purpose**: Program entry point with signal handling

**Features**:
- Signal handling for graceful shutdown (Ctrl+C)
- Command line argument parsing
- Exception handling and error reporting
- Server lifecycle management

## Key Design Decisions

### 1. Single Model Architecture

**Decision**: One process = one model (no dynamic loading/unloading)

**Rationale**:
- Simplicity and reliability
- Avoids complex state management
- Follows llama-server pattern
- Easy to scale horizontally (multiple processes)

**Trade-off**: Need multiple server instances for multiple models

### 2. ONNX Runtime GenAI Integration

**Decision**: Direct integration with ONNX Runtime GenAI C++ API

**Rationale**:
- Native C++ performance
- Full control over execution providers
- No Python dependency

**Implementation**:
- Uses `OgaModel`, `OgaTokenizer`, `OgaGenerator` classes
- Configures NPU/Hybrid execution via genai_config.json
- Handles tokenization and generation

### 3. Streaming via Server-Sent Events (SSE)

**Decision**: Use SSE for streaming responses

**Rationale**:
- OpenAI API compatible
- Simple to implement
- Works well with HTTP

**Implementation**:
- `set_chunked_content_provider()` from cpp-httplib
- Token-by-token streaming via callback
- Proper SSE format: `data: {...}\n\n`

### 4. Prompt Length Handling

**Decision**: Automatic truncation based on `rai_config.json`

**Rationale**:
- NPU models have version-specific limits
- Better UX than hard errors
- Follows Python reference implementation

**Implementation**:
- Detects Ryzen AI version
- Reads `max_prompt_length` from `rai_config.json`
- Truncates from beginning to preserve recent context
- Logs warnings when truncation occurs

### 5. Header-Only Dependencies

**Decision**: Use header-only libraries (cpp-httplib, nlohmann/json)

**Rationale**:
- Easy integration (no separate compilation)
- Automatic download via CMake
- Reduces build complexity

**Libraries**:
- `cpp-httplib`: HTTP server
- `nlohmann/json`: JSON parsing

## Thread Safety

### Inference Engine
- Protected by `inference_mutex_`
- Only one inference at a time per server instance
- Multiple server instances can run in parallel

### HTTP Server
- cpp-httplib handles concurrent requests
- Each request gets its own thread
- Inference mutex prevents concurrent model access

## Error Handling

### Levels
1. **Validation**: Command line, model path, required files
2. **Loading**: Model loading, DLL dependencies
3. **Runtime**: Inference errors, HTTP errors
4. **Graceful**: Signal handling for Ctrl+C

### Error Responses
All errors return JSON with:
```json
{
  "error": {
    "message": "Error description",
    "type": "error_type"
  }
}
```

## Performance Considerations

### Memory
- Model loaded once at startup
- Tokenizer reused for all requests
- No caching of responses (stateless)

### Inference
- NPU offload reduces CPU usage
- Hybrid mode balances NPU/iGPU workload
- Single request at a time (serialized by mutex)

### Network
- Chunked transfer for streaming
- CORS enabled for web clients
- Keep-alive connections

## Future Enhancements (Not Implemented)

### Potential Improvements
1. **Batch Processing**: Handle multiple requests in parallel
2. **Request Queuing**: Queue requests when inference is busy
3. **Model Caching**: Cache multiple models (trade memory for flexibility)
4. **Metrics**: Prometheus-style metrics endpoint
5. **Authentication**: API key support
6. **Rate Limiting**: Per-client rate limits
7. **Longer Context**: Automatic context windowing for long conversations

### Not Needed (By Design)
- Model downloading (use existing tools)
- Model conversion (use Ryzen AI tools)
- Dynamic model loading (use multiple servers)
- Models endpoint (single model server)

## Testing

### Automated Testing
- `test_server.py`: Python script for endpoint testing
- Tests health, completion, chat, streaming

### Manual Testing
```bash
# Start server
ryzenai-serve.exe -m C:\path\to\model --verbose

# Test with curl
curl http://localhost:8080/health
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'
```

## Build System

### CMake Configuration
- Minimum CMake 3.20
- C++17 standard
- Visual Studio 2022 support
- Automatic dependency download
- DLL copying post-build

### Dependencies
1. **Required at Build Time**:
   - Visual Studio 2022
   - CMake 3.20+
   - Ryzen AI 1.6.0

2. **Downloaded Automatically**:
   - cpp-httplib (v0.14.3)
   - nlohmann/json (v3.11.3)

3. **Runtime Dependencies**:
   - onnxruntime_genai.dll
   - onnxruntime.dll
   - Custom ops DLLs (NPU/Hybrid specific)

## Documentation Files

1. **README.md**: Complete architecture documentation
2. **BUILD.md**: Build instructions and troubleshooting
3. **QUICKSTART.md**: Quick start guide for users
4. **IMPLEMENTATION.md**: This file - implementation details

## Code Statistics

- **Header files**: 4 (.h files)
- **Source files**: 5 (.cpp files)
- **Total lines**: ~1500 lines of C++
- **External dependencies**: 2 (header-only)
- **Supported platforms**: Windows (x64)

## OpenAI API Compatibility

### Implemented
- ✓ POST /v1/completions
- ✓ POST /v1/chat/completions
- ✓ Streaming (SSE)
- ✓ Temperature, top_p, top_k
- ✓ max_tokens
- ✓ Stop sequences (partial)

### Not Implemented (By Design)
- ✗ GET /v1/models (single model server)
- ✗ Embeddings endpoint
- ✗ Fine-tuning endpoints
- ✗ Assistant API
- ✗ Function calling

## Conclusion

This implementation provides a production-ready, OpenAI API compatible server for running LLMs on Ryzen AI NPUs. The design prioritizes simplicity, reliability, and performance while maintaining full compatibility with the OpenAI client ecosystem.

Key strengths:
- Simple architecture (one model per process)
- Full NPU/Hybrid support
- Automatic prompt length handling
- OpenAI API compatible
- Easy to build and deploy

For questions or issues, refer to the Ryzen AI documentation or create an issue.

