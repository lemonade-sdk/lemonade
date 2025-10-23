# Ryzen AI LLM Server

## Known Issues

### Streaming Implementation Note
**Status**: ✅ Streaming is fully functional with a workaround for a JSON library compatibility issue.

**Implementation Detail**:
Creating `nlohmann::json` objects directly within OGA streaming callbacks causes crashes. This appears to be related to memory allocation or threading interactions between the JSON library and the callback context.

**Solution**: JSON strings are manually constructed within streaming callbacks instead of using `nlohmann::json` objects. This provides stable, real-time streaming performance.

**Performance**: Both streaming and non-streaming modes work reliably with excellent performance characteristics.

# Ryzen AI LLM Server

This project implements an OpenAI API compatible local LLM server for Ryzen AI LLMs written in C++.

Ryzen AI Software is a fork of ONNX Runtime GenAI that adds the capability to run models on the NPU of supported Ryzen AI 300-series CPUs.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Command Line Interface](#command-line-interface)
3. [Core Components](#core-components)
4. [Execution Providers](#execution-providers)
5. [API Design](#api-design)
6. [Build System](#build-system)
7. [Dependencies](#dependencies)
8. [Error Handling](#error-handling)
9. [Performance Considerations](#performance-considerations)
10. [Security Considerations](#security-considerations)

## Architecture Overview

The Ryzen AI LLM Server follows a simple, single-model architecture similar to llama-server:

```
┌─────────────────────────────────────────────────────────────┐
│                     HTTP Server Layer                        │
│                  (httplib, CORS, routing)                    │
├─────────────────────────────────────────────────────────────┤
│                    API Handler Layer                         │
│              (OpenAI API compatibility)                      │
├─────────────────────────────────────────────────────────────┤
│                 Inference Engine Layer                       │
│        (ONNX Runtime GenAI with NPU/Hybrid support)         │
├─────────────────────────────────────────────────────────────┤
│                  Hardware Abstraction                        │
│              (NPU, iGPU, CPU providers)                      │
└─────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Simplicity**: One process = one model, no dynamic loading/unloading
2. **RAII**: Resource management follows C++ best practices with smart pointers and RAII patterns
3. **Thread Safety**: All shared resources are protected with appropriate synchronization primitives
4. **Error Propagation**: Errors are handled gracefully and propagated with meaningful messages
5. **Minimal Dependencies**: Keep the dependency tree small and manageable

## Command Line Interface

The server follows a simple command-line interface similar to llama-server:

```bash
# Basic usage
ryzenai-serve.exe -m path/to/onnx/model/folder

# With options
ryzenai-serve.exe -m path/to/model --port 8080 --host 0.0.0.0 --mode hybrid
```

### Command Line Arguments

```cpp
struct CommandLineArgs {
    std::string model_path;      // -m, --model (required)
    std::string host = "127.0.0.1"; // --host
    int port = 8080;             // --port
    std::string mode = "auto";   // --mode (auto|npu|hybrid)
    int ctx_size = 2048;         // --ctx-size
    int threads = 4;             // --threads
    bool verbose = false;        // --verbose
};
```

Example implementation:
```cpp
int main(int argc, char* argv[]) {
    CommandLineArgs args = parseCommandLine(argc, argv);
    
    if (args.model_path.empty()) {
        std::cerr << "Error: Model path is required (-m flag)" << std::endl;
        return 1;
    }
    
    try {
        RyzenAIServer server(args);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
```

## Core Components

### 1. HTTP Server (`RyzenAIServer`)

The main server class that loads a single model at startup and serves requests:

```cpp
class RyzenAIServer {
public:
    RyzenAIServer(const CommandLineArgs& args);
    void run();
    void stop();
    
private:
    void loadModel();
    void setupRoutes();
    
    std::unique_ptr<httplib::Server> http_server_;
    std::unique_ptr<InferenceEngine> inference_engine_;
    CommandLineArgs args_;
    std::string model_id_;  // Extracted from model path
};
```

### 2. Inference Engine (`InferenceEngine`)

Abstracts the ONNX Runtime GenAI interface:

```cpp
class InferenceEngine {
public:
    InferenceEngine(const std::string& model_path, const std::string& mode);
    ~InferenceEngine();
    
    CompletionResponse complete(const CompletionRequest& request);
    void streamComplete(const CompletionRequest& request,
                       StreamCallback callback);
    
    std::string getModelName() const { return model_name_; }
    std::string getExecutionMode() const { return execution_mode_; }
    int getMaxPromptLength() const { return max_prompt_length_; }
    
private:
    void loadModel();
    void setupExecutionProvider();
    void loadRaiConfig();
    std::vector<int32_t> truncatePrompt(const std::vector<int32_t>& input_ids);
    
    std::unique_ptr<OgaModel> model_;
    std::unique_ptr<OgaTokenizer> tokenizer_;
    std::unique_ptr<OgaGeneratorParams> default_params_;
    
    std::string model_path_;
    std::string model_name_;
    std::string execution_mode_;  // "npu", "hybrid", or "auto"
    std::string ryzenai_version_;
    int max_prompt_length_ = 2048;  // Default, overridden by rai_config.json
};
```

#### Prompt Length Handling

The inference engine automatically handles prompts that exceed the model's maximum length:

```cpp
void InferenceEngine::loadRaiConfig() {
    // Detect Ryzen AI version based on installed software
    ryzenai_version_ = detectRyzenAIVersion();  // e.g., "1.6.0"
    
    // Load rai_config.json if it exists
    std::string rai_config_path = model_path_ + "/rai_config.json";
    if (std::filesystem::exists(rai_config_path)) {
        std::ifstream file(rai_config_path);
        nlohmann::json config = nlohmann::json::parse(file);
        
        if (config.contains("max_prompt_length") && 
            config["max_prompt_length"].contains(ryzenai_version_)) {
            max_prompt_length_ = config["max_prompt_length"][ryzenai_version_];
        }
    }
}

std::string InferenceEngine::detectRyzenAIVersion() {
    // Check for Ryzen AI installation path
    std::string ryzenai_path = "C:/Program Files/RyzenAI/1.6.0";
    if (std::filesystem::exists(ryzenai_path)) {
        return "1.6.0";
    }
    
    // Check environment variable or version file
    const char* version_env = std::getenv("RYZENAI_VERSION");
    if (version_env) {
        return std::string(version_env);
    }
    
    // Default to latest supported version
    return "1.6.0";
}

std::vector<int32_t> InferenceEngine::truncatePrompt(
    const std::vector<int32_t>& input_ids) {
    
    if (input_ids.size() <= max_prompt_length_) {
        return input_ids;
    }
    
    // Truncate from the beginning to keep the most recent context
    size_t truncate_amount = input_ids.size() - max_prompt_length_;
    std::cout << "[WARNING] Prompt exceeds maximum length (" 
              << input_ids.size() << " > " << max_prompt_length_ 
              << "). Truncating " << truncate_amount << " tokens from the beginning."
              << std::endl;
    
    return std::vector<int32_t>(
        input_ids.begin() + truncate_amount, 
        input_ids.end()
    );
}
```

### 3. Request Handlers

OpenAI API-compatible request handlers:

```cpp
class CompletionHandler {
public:
    httplib::Response handle(const httplib::Request& req);
    
private:
    CompletionRequest parseRequest(const nlohmann::json& body);
    nlohmann::json formatResponse(const CompletionResponse& response);
    void handleStreaming(httplib::Response& res, 
                        const CompletionRequest& request);
};

class ChatCompletionHandler {
public:
    httplib::Response handle(const httplib::Request& req);
    
private:
    std::string formatMessages(const std::vector<ChatMessage>& messages);
};
```

## Execution Providers

### NPU-Only Mode

For NPU-only execution, the system uses the VitisAI execution provider:

```cpp
class NPUExecutionProvider {
public:
    void initialize() {
        // Load VitisAI custom ops
        loadCustomOps("onnxruntime_vitis_ai_custom_ops.dll");
        
        // Configure NPU-specific session options
        session_options_.AddConfigEntry("provider.vitisai.backend", "npu");
        session_options_.AppendExecutionProvider("VitisAI");
    }
};
```

### Hybrid Mode (NPU + iGPU)

Hybrid mode distributes workload between NPU and iGPU for optimal performance:

```cpp
class HybridExecutionProvider {
public:
    void initialize() {
        // Load hybrid custom ops
        loadCustomOps("onnx_custom_ops.dll");
        
        // Configure both providers
        configureDMLProvider();  // For iGPU
        configureNPUProvider();  // For NPU
        
        // Set hybrid-specific optimizations
        session_options_.AddConfigEntry("hybrid.mode", "balanced");
    }
    
private:
    void configureDMLProvider() {
        session_options_.AppendExecutionProvider("DML");
    }
    
    void configureNPUProvider() {
        session_options_.AddConfigEntry("provider.vitisai.backend", "npu");
        session_options_.AppendExecutionProvider("VitisAI");
    }
};
```

## API Design

### Endpoints

The server implements the following OpenAI-compatible endpoints:

1. **POST /v1/completions** - Text completion endpoint
   ```json
   {
     "prompt": "Hello, how are",
     "max_tokens": 50,
     "temperature": 0.7,
     "stream": false
   }
   ```
   Note: The "model" field is accepted for compatibility but ignored since only one model is loaded.

2. **POST /v1/chat/completions** - Chat completion endpoint
   ```json
   {
     "messages": [
       {"role": "system", "content": "You are a helpful assistant."},
       {"role": "user", "content": "Hello!"}
     ],
     "stream": true
   }
   ```
   Note: The "model" field is accepted for compatibility but ignored since only one model is loaded.

3. **GET /health** - Health check endpoint
   ```json
   {
     "status": "ok",
     "model": "phi-3-mini-4k-instruct",
     "execution_mode": "hybrid",
     "model_path": "C:/Users/user/.cache/huggingface/hub/models--microsoft--phi-3-mini-4k-instruct-onnx"
   }
   ```

### Streaming Support

Server-Sent Events (SSE) implementation for streaming responses:

```cpp
class StreamingResponse {
public:
    void writeChunk(const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_.write("data: " + data + "\n\n");
    }
    
    void complete() {
        writeChunk("[DONE]");
    }
    
private:
    httplib::DataSink& sink_;
    std::mutex mutex_;
};
```

## Model Loading Process

The model is loaded once at server startup:

```cpp
class RyzenAIServer {
    void loadModel() {
        std::cout << "Loading model from: " << args_.model_path << std::endl;
        
        // Validate model directory
        if (!validateModelDirectory(args_.model_path)) {
            throw std::runtime_error("Invalid model directory");
        }
        
        // Determine execution mode
        std::string mode = args_.mode;
        if (mode == "auto") {
            mode = detectBestExecutionMode(args_.model_path);
        }
        
        // Create and initialize inference engine
        inference_engine_ = std::make_unique<InferenceEngine>(
            args_.model_path, 
            mode
        );
        
        // Extract model name from path for API responses
        model_id_ = extractModelName(args_.model_path);
        
        std::cout << "Model loaded successfully: " << model_id_ 
                  << " (mode: " << mode << ")" << std::endl;
    }
};
```

### Startup Sequence

1. Parse command line arguments
2. Validate model path exists and contains required files
3. Load model into ONNX Runtime GenAI
4. Configure execution provider (NPU/Hybrid)
5. Initialize HTTP server and routes
6. Start serving requests

### Model Validation

Required files in model directory:
- `genai_config.json` - Model configuration
- `model.onnx` - Main model file  
- `tokenizer.json` - Tokenizer configuration
- `tokenizer_config.json` - Additional tokenizer settings
- `rai_config.json` - (Optional) Ryzen AI specific configuration
- Custom ops DLL path configured in genai_config.json

#### Ryzen AI Configuration (`rai_config.json`)

NPU models may include a [`rai_config.json`](https://huggingface.co/amd/Qwen2-1.5B-onnx-ryzenai-npu/blob/main/rai_config.json) file that specifies version-specific parameters:

```json
{
  "max_prompt_length": {
    "1.3.1": 2048,
    "1.4.0": 2048,
    "1.5.0": 2048,
    "1.6.0": 4096
  }
}
```

This configuration is used to enforce prompt length limits based on the installed Ryzen AI version.

## Build System

### CMake Configuration

```cmake
cmake_minimum_required(VERSION 3.20)
project(ryzenai-serve)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(Threads REQUIRED)

# ONNX Runtime GenAI
set(OGA_ROOT "C:/Program Files/RyzenAI/1.6.0")
find_library(OGA_LIB onnxruntime_genai PATHS ${OGA_ROOT}/lib)
find_path(OGA_INCLUDE onnxruntime_genai.h PATHS ${OGA_ROOT}/include)

# Add executable
add_executable(ryzenai-serve
    src/main.cpp
    src/server.cpp
    src/inference_engine.cpp
    src/command_line.cpp
    src/handlers/completion_handler.cpp
    src/handlers/chat_completion_handler.cpp
)

target_include_directories(ryzenai-serve PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OGA_INCLUDE}
)

target_link_libraries(ryzenai-serve PRIVATE
    ${OGA_LIB}
    Threads::Threads
)

# Copy required DLLs to output directory
add_custom_command(TARGET ryzenai-serve POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${OGA_ROOT}/bin/onnxruntime_genai.dll
    ${OGA_ROOT}/bin/onnxruntime.dll
    ${OGA_ROOT}/bin/onnxruntime_vitis_ai_custom_ops.dll
    $<TARGET_FILE_DIR:ryzenai-serve>
)
```

## Dependencies

### Required Libraries

1. **ONNX Runtime GenAI** (1.6.0+)
   - Core inference engine
   - NPU/Hybrid execution providers
   - Custom operations support

2. **cpp-httplib** (Header-only)
   - HTTP server implementation
   - SSE streaming support

3. **nlohmann/json** (Header-only)
   - JSON parsing and serialization
   - OpenAI API compatibility

4. **Windows SDK**
   - NPU driver interaction
   - System information queries

### Runtime Dependencies

- AMD NPU Driver (version 32.0.130.1018 or higher)
- Visual C++ Redistributables 2022
- Ryzen AI Software 1.6.0 with LLM patch

## Error Handling

### Error Categories

1. **Initialization Errors**
   - NPU driver not found/incompatible
   - Missing DLL dependencies
   - Invalid Ryzen AI installation

2. **Model Errors**
   - Model not found in cache
   - Incompatible model format
   - Corrupted model files

3. **Runtime Errors**
   - Out of memory
   - NPU timeout
   - Invalid request parameters

### Error Response Format

```cpp
struct ErrorResponse {
    std::string error;
    std::string type;
    std::string message;
    
    nlohmann::json toJSON() const {
        return {
            {"error", {
                {"type", type},
                {"message", message}
            }}
        };
    }
};
```

## Performance Considerations

### Memory Management

1. **Model Caching**: Keep loaded models in memory to avoid reload overhead
2. **Token Buffer Pooling**: Reuse token buffers for multiple requests
3. **DLL Preloading**: Load all required DLLs at startup

### Concurrency

1. **Thread Pool**: Use thread pool for handling concurrent requests
2. **Lock-Free Queues**: For request queuing and response streaming
3. **Async Model Loading**: Load models asynchronously to avoid blocking

### NPU Optimization

1. **Prompt Length**: NPU models have version-specific prompt length limits (2048-4096 tokens)
   - Automatically truncated if exceeded to maintain performance
   - Configured via `rai_config.json` per Ryzen AI version
2. **Batch Size**: Configure optimal batch size for NPU (typically 1 for interactive use)
3. **Graph Capture**: Enable graph capture for repeated inference patterns
4. **Memory Pinning**: Pin frequently accessed memory for NPU access

## Security Considerations

### Input Validation

1. **Request Size Limits**: Enforce maximum request body size
2. **Token Limits**: Validate max_tokens parameter
3. **Path Traversal**: Sanitize model paths to prevent directory traversal

### Network Security

1. **CORS Configuration**: Restrictive CORS policy by default
2. **Rate Limiting**: Built-in rate limiting for API endpoints
3. **Authentication**: Optional API key authentication support

### Process Isolation

1. **Sandboxing**: Run inference in separate process if needed
2. **Resource Limits**: Set memory and CPU limits
3. **Timeout Handling**: Enforce request timeouts

## Resources

Documentation is available in these places:

1. ONNX Runtime GenAI
    a. github: https://github.com/microsoft/onnxruntime-genai
    b. docs: https://onnxruntime.ai/docs/genai/

2. Ryzen AI Software
    a. Docs: https://ryzenai.docs.amd.com/en/latest/hybrid_oga.html
    b. Example: https://github.com/amd/RyzenAI-SW/tree/main/example/llm/oga_api

## Implementation Guidelines

1. **Single Model Architecture**: Following the llama-server pattern, one process = one model
    - Model is specified at startup with `-m` flag pointing to ONNX model folder
    - No dynamic loading/unloading of models
    - To serve a different model, start a new server instance on a different port

2. **Prerequisites**: Assume Ryzen AI SW 1.6 and LLM 1.6 patch are installed
    - Installation path: `C:\Program Files\RyzenAI\1.6.0`
    - NPU driver version 32.0.130.1018 or higher

3. **Required OpenAI API Endpoints**:
    - POST `/v1/completions` (streaming and non-streaming)
    - POST `/v1/chat/completions` (streaming and non-streaming)
    - GET `/health`
    - No models endpoint needed (single model architecture)

4. **Model Format**: ONNX models with genai_config.json
    - Models should be pre-converted to ONNX format
    - Must include tokenizer files and configuration
    - Custom ops library path configured in genai_config.json

5. **Execution Modes**:
    - NPU-only: Uses VitisAI execution provider
    - Hybrid: Uses both NPU (VitisAI) and iGPU (DirectML) providers
    - Auto: Detect best mode based on model configuration

6. **Platform**: Windows-only implementation
    - Use Windows-specific APIs where appropriate
    - Leverage existing llama-server patterns from `src\cpp`

7. **Build Output**: `ryzenai-serve.exe`
    - Single executable with minimal dependencies
    - All required DLLs copied to output directory

## Usage Examples

### Starting the Server

```bash
# Basic usage - auto-detect execution mode
ryzenai-serve.exe -m C:\models\phi-3-mini-4k-instruct-onnx

# Specify NPU-only mode
ryzenai-serve.exe -m C:\models\phi-3-mini-4k-instruct-onnx --mode npu

# Hybrid mode on custom port
ryzenai-serve.exe -m C:\models\phi-3-mini-4k-instruct-onnx --mode hybrid --port 8081

# Verbose output for debugging
ryzenai-serve.exe -m C:\models\phi-3-mini-4k-instruct-onnx --verbose
```

### Making API Calls

```bash
# Chat completion
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "Hello!"}],
    "max_tokens": 50
  }'

# Text completion
curl http://localhost:8080/v1/completions \
  -H "Content-Type: application/json" \
  -d '{
    "prompt": "Once upon a time",
    "max_tokens": 100,
    "temperature": 0.7
  }'

# Health check
curl http://localhost:8080/health
```

### Multiple Models

To serve multiple models simultaneously:

```bash
# Terminal 1 - Phi-3 on port 8080
ryzenai-serve.exe -m C:\models\phi-3-mini-4k-instruct-onnx --port 8080

# Terminal 2 - Llama-2 on port 8081
ryzenai-serve.exe -m C:\models\llama-2-7b-chat-onnx --port 8081

# Terminal 3 - Mistral on port 8082
ryzenai-serve.exe -m C:\models\mistral-7b-instruct-onnx --port 8082
```