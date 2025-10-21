# Spec: lemon.cpp

## Introduction

This document describes `lemon.cpp`, a C++ implementation of Lemonade's python LLM server.

At a high level, `lemon.cpp` is a lightweight local LLM server-router. The user/client sends an LLM completion request, `lemon.cpp` routes that request to a local LLM server, and then routes the response back to the client.

The local LLM servers, which we refer to as `WrappedServer`s, are responsible for generating the completion using an LLM. `WrappedServer` is defined in detail below.

The initial implementaiton of `lemon.cpp` will support two `WrappedServer`s: `llama-server` (from `llama.cpp`), and `FastFlowLM`. This support allows `lemon.cpp` to run LLMs on CPU/GPU and NPU, respectively.

## Implementation Guidelines

### Similarities to Python

From a black-box user's perspective, `lemon.cpp` should be very similar to the original Python implementation:
- `lemon.cpp` will implement all server endpoints described in `docs/server/server_spec.md`.
- `lemon.cpp` will pass all tests in `test/server_llamacpp.py` and `test/server_flm.py`.
    - We will keep using these Python-based integration tests; no need to write new integration tests in C++.
    - `test/utils/server_base.py` will need a new command line argument to instantiate a `lemon.cpp` server instead of a Python `lemonade-server-dev`.
- `lemon.cpp` will be fully compatible with the web ui HTML, JavaScript, and CSS defined in `src\lemonade\tools\server\static`
    - That web ui code may need to be copied to a new location at build time so that `lemon.cpp` can serve it.
- The `lemon.cpp` CLI will implement all of the same commands and arguments as the Python `lemonade-server-dev` defined in `src\lemonade_server\cli.py`.
- `lemon.cpp` will use `src\lemonade_server\server_models.json` as its model registry.
    - This file may need to be copied at build time so that `lemon.cpp` can use it.
    - `lemon.cpp` should be able to correctly filter out models with recipes that aren't supported yet.
- `lemon.cpp` should have the same policies as the Python implemention for downloading and installing `llama.cpp` and `FastFlowLM` backends.

### Differences from Python

`lemon.cpp` also represents an opportunity for streamling the codebase relative to the original Python implementaiton:
- `lemon.cpp` will have no benchmarking, accuracy, or model building/optimization tooling.
- `lemon.cpp` will treat all LLM backends equally as peer `WrappedServer`s in the implementation.
- `lemon.cpp` will have minimal external dependencies, a fast install time, and minimal size on disk.

## Build Artifacts

The `lemon.cpp` source code is located in `src\cpp`.

Building the `lemon.cpp` project with `cmake` will result in a `lemonade` CLI, which should implement all of the same commands and arguments as the Python `lemonade-server-dev`.

`lemon.cpp` should work on Windows, Ubuntu, and macOS. On Windows, the executable should be named `lemonade.exe`.

## WrappedServer

The most important class in `lemon.cpp` is called `WrappedServer`. There is a Python reference implementation in `src\lemonade\tools\server\wrapped_server.py`.

There are also reference implementations for `llama.cpp` and `FastFlowLM` in `src\lemonade\tools\server\llamacpp.py` and `src\lemonade\tools\server\flm.py`, respetively. Note that a lot of logic is implemented in `src\lemonade\tools\llamacpp` and `src\lemonade\tools\flm`, respectively, as well as `src\lemonade_server\model_manager.py`. This code structure is messy and should not be immitated in `lemon.cpp`--just use this code as a functional reference.

The purpose of `WrappedServer` is to provide a unified interface between domain-specific local LLM servers to the high-level `lemon.cpp` server-router. 

A `WrappedServer` implementation needs to be able to:
- Install and start the backend server.
- Pull, load, unload, and delete server-specific models.
- Route `chat/completions` requests to the backend server and route the response back to the client.
    - We will implement support for `completions`, `responses`, `embeddings`, and `reranking` APIs later.
- Parse the wrapped server's stdout/stderr for important information such as performance data, important errors to escalate, etc.

## Testing

`lemon.cpp` will be hosted in the GitHub repo https://github.com/lemonade-sdk/lemonade

`lemonade.cpp` should be tested using GitHub actions. There are already workflows in `.github` for running `test/server_llamacpp.py` and `test/server_flm.py`, so these will be copied as a starting point and adapted for testing `lemon.cpp` instead of the Python implementation.

## Critical Implementation Details and Gotchas

### 1. API Endpoint Details
- **Dual API Prefixes**: Must support both `/api/v0` and `/api/v1` prefixes for backward compatibility (register all endpoints twice)
- **Additional Endpoints**: Beyond the documented endpoints, also implement:
  - `/api/v1/responses` - Alternative completion format
  - `/api/v1/logs/ws` - WebSocket endpoint for log streaming
  - `/api/v1/embeddings` - Embeddings generation (supported by llama.cpp)
  - `/api/v1/rerank` - Document reranking (supported by llama.cpp)

### 2. Model Management Complexities
- **User Models Registry**: Need to support `user_models.json` in cache directory for custom model registration
- **Model Namespacing**: User-registered models must use "user." prefix
- **GGUF Variant Handling**: GGUF models have variants (Q4_0, Q4_K_M) requiring specific file verification
- **Model Labels**: Support labels ("embeddings", "reranking", "reasoning", "vision") that enable features
- **FLM Models**: Use `flm pull` command instead of Hugging Face downloads

### 3. Request/Response Handling
- **Parameter Conflicts**: `max_tokens` and `max_completion_tokens` are mutually exclusive
- **Tool Calls**: Must parse and format OpenAI-style tool calls in chat completions
- **Streaming Format**: Preserve exact SSE format including blank lines between events
- **Error Format**: Use consistent JSON format with "status" and "message" fields
- **Extra Parameters**: Support non-standard parameters via `extra_body` mechanism

### 4. WrappedServer Implementation Details
- **Port Management**: Automatically find free ports for wrapped servers
- **Process Lifecycle**: Handle stubborn child processes (especially llama-server)
- **Health Checking**: Poll `/health` endpoint until server ready
- **Telemetry Parsing**: Extract performance metrics from subprocess output
- **Offline Mode**: Respect LEMONADE_OFFLINE environment variable

### 5. Platform-Specific Features
- **Tray Icon**: Windows and macOS support (disabled on macOS for `run` command)
- **Default Backends**: Metal on macOS ARM, Vulkan elsewhere
- **Process Detection**: Different methods for finding running servers per platform
- **Path Handling**: Windows backslashes vs Unix forward slashes

### 6. Web UI Integration
- **Static Files**: Serve with no-cache headers
- **WebSocket Logs**: Real-time log streaming to web interface
- **CORS Headers**: Required for web UI functionality
- **Default Page**: Redirect `/` to web UI with model preselection

### 7. Configuration and State
- **Persistent Settings**: Save log level preference
- **Environment Variables**: Support HF_TOKEN, LEMONADE_OFFLINE, LEMONADE_CACHE_DIR
- **Context Size**: Different defaults per backend (4096 for llama.cpp)
- **Cache Compatibility**: Use exact same structure as huggingface_hub

### 8. Testing Considerations
- **Offline Testing**: Mock network calls except localhost
- **Self-Hosted Runners**: Respect hardware-specific test requirements
- **Model Variants**: Test both base models and specific GGUF variants
- **Concurrent Requests**: Handle multiple simultaneous model operations

## Detailed Implementation Plan

### Project Structure

```
src/cpp/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── lemon/
│   │   ├── server.h
│   │   ├── router.h
│   │   ├── wrapped_server.h
│   │   ├── model_manager.h
│   │   ├── cli_parser.h
│   │   └── utils/
│   │       ├── json_utils.h
│   │       ├── http_client.h
│   │       └── process_manager.h
│   └── backends/
│       ├── llamacpp_server.h
│       └── fastflowlm_server.h
├── src/
│   ├── main.cpp
│   ├── server.cpp
│   ├── router.cpp
│   ├── wrapped_server.cpp
│   ├── model_manager.cpp
│   ├── cli_parser.cpp
│   ├── utils/
│   │   ├── json_utils.cpp
│   │   ├── http_client.cpp
│   │   └── process_manager.cpp
│   └── backends/
│       ├── llamacpp_server.cpp
│       └── fastflowlm_server.cpp
├── resources/
│   └── static/  # Copy of web UI files
├── tests/
│   └── unit/
│       ├── test_model_manager.cpp
│       ├── test_router.cpp
│       └── test_utils.cpp
└── third_party/
    ├── httplib/  # HTTP server library (MIT License)
    ├── nlohmann_json/  # JSON library (MIT License)
    └── cli11/  # CLI parsing library (BSD 3-Clause License)
```

### Implementation Phases

#### Phase 1: Core Infrastructure
**Goal**: Set up project structure and basic HTTP server

**Tasks**:
1. Set up CMake build system
   - Configure for Windows, Ubuntu, and macOS
   - Set up dependency management
   - Configure static resource embedding

2. Implement basic HTTP server
   - Use cpp-httplib for HTTP handling
   - Implement health check endpoint (`/health`)
   - Set up request routing infrastructure

3. Implement CLI parser
   - Parse all arguments from Python `lemonade-server-dev`
   - Support subcommands: `serve`, `status`, `stop`, `list`, `pull`, `delete`, `run`

4. Set up logging infrastructure
   - Console and file logging
   - Log levels matching Python implementation

**Deliverables**:
- Working HTTP server with health endpoint
- CLI that parses all required arguments
- CMake build for all platforms

#### Phase 2: Model Management
**Goal**: Implement model registry and management

**Tasks**:
1. Implement ModelManager class
   - Parse `server_models.json`
   - Filter models by supported backends
   - Model metadata management

2. Implement model operations
   - List available models
   - Track loaded models
   - Model configuration parsing

3. File system operations
   - Model download directory management
   - Cache management
   - Configuration file handling

**Deliverables**:
- Working `models` command
- Model registry parsing and filtering
- Model metadata management

#### Phase 3: WrappedServer Interface
**Goal**: Implement the core WrappedServer abstraction

**Tasks**:
1. Define WrappedServer base class
   - Virtual methods for all operations
   - Process lifecycle management
   - Health monitoring

2. Implement ProcessManager utility
   - Cross-platform process spawning
   - stdout/stderr capture and parsing
   - Process termination handling

3. Implement backend installation
   - Download backend executables
   - Version management
   - Platform-specific installation

**Deliverables**:
- Complete WrappedServer interface
- Process management utilities
- Backend installation framework

#### Phase 4: Llama.cpp Integration
**Goal**: Complete llama.cpp backend implementation

**Tasks**:
1. Implement LlamaCppServer class
   - Server startup with model loading
   - Request proxying to llama-server
   - Response streaming

2. Model operations
   - GGUF file management
   - Model loading/unloading
   - Memory management

3. Performance monitoring
   - Parse llama-server output for metrics
   - Token generation statistics
   - Error handling

**Deliverables**:
- Working llama.cpp backend
- All model operations functional
- Performance metrics collection

#### Phase 5: FastFlowLM Integration
**Goal**: Complete FastFlowLM backend implementation

**Tasks**:
1. Implement FastFlowLMServer class
   - NPU-specific initialization
   - Model loading for NPU
   - Request handling

2. Platform-specific handling
   - Windows NPU driver detection
   - Error handling for unsupported platforms
   - Fallback mechanisms

3. Integration testing
   - Verify NPU model loading
   - Performance validation
   - Error scenarios

**Deliverables**:
- Working FastFlowLM backend
- NPU support on Windows
- Complete backend parity

#### Phase 6: API Endpoints
**Goal**: Implement all required API endpoints

**Tasks**:
1. Implement core completion endpoints
   - POST `/api/v1/chat/completions` - Chat completions with tool call support
   - POST `/api/v1/completions` - Text completions with echo support
   - POST `/api/v1/responses` - Alternative response format
   - POST `/api/v1/embeddings` - Generate embeddings (llama.cpp)
   - POST `/api/v1/rerank` - Rerank documents (llama.cpp)

2. Implement model management endpoints
   - GET `/api/v1/models` - List available models
   - GET `/api/v1/models/{model_id}` - Get specific model info
   - POST `/api/v1/pull` - Download/register models
   - POST `/api/v1/load` - Load model into memory
   - POST `/api/v1/unload` - Unload model from memory
   - POST `/api/v1/delete` - Delete model from storage

3. Implement system endpoints
   - GET `/api/v1/health` - Server health check
   - GET `/api/v1/stats` - Performance statistics
   - GET `/api/v1/system-info` - Device enumeration (NPU/GPU)
   - POST `/api/v1/params` - Set generation parameters
   - POST `/api/v1/log-level` - Change log level

4. Implement UI endpoints
   - GET `/` - Redirect to web UI
   - Static file serving with no-cache headers
   - WebSocket `/api/v1/logs/ws` - Real-time log streaming
   - CORS handling for all endpoints

**Deliverables**:
- All API endpoints functional
- Web UI integration
- OpenAI-compatible API

#### Phase 7: Testing and Polish
**Goal**: Comprehensive testing and optimization

**Tasks**:
1. Integration with existing test suite
   - Modify `test/utils/server_base.py` for lemon.cpp
   - Ensure all Python tests pass
   - Performance benchmarking

2. Unit testing
   - Test coverage for core components
   - Edge case handling
   - Memory leak detection

3. Platform-specific testing
   - Windows installer integration
   - macOS code signing preparation
   - Linux distribution testing

**Deliverables**:
- All tests passing
- Performance optimization complete
- Release candidates for all platforms

### Key Components and Classes

#### 1. Server Class
```cpp
class Server {
    // Main HTTP server coordinating all operations
    // Handles request routing and lifecycle management
};
```

#### 2. Router Class
```cpp
class Router {
    // Routes requests to appropriate WrappedServer
    // Manages backend selection based on model
};
```

#### 3. WrappedServer Base Class
```cpp
class WrappedServer {
public:
    virtual bool Install() = 0;
    virtual bool Start(const ModelConfig& config) = 0;
    virtual bool Stop() = 0;
    virtual Response ProcessRequest(const Request& req) = 0;
    virtual bool LoadModel(const std::string& model_id) = 0;
    virtual bool UnloadModel() = 0;
    virtual ModelStatus GetStatus() = 0;
};
```

#### 4. ModelManager Class
```cpp
class ModelManager {
    // Parses and manages server_models.json
    // Tracks loaded models and their backends
    // Handles model file operations
};
```

### Build System Configuration

#### CMakeLists.txt Structure
```cmake
cmake_minimum_required(VERSION 3.20)
project(lemon_cpp VERSION 1.0.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
include(FetchContent)

# cpp-httplib (MIT License)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.0
)

# nlohmann/json (MIT License)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)

# CLI11 (BSD 3-Clause License)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.3.2
)

# Platform-specific configurations
if(WIN32)
    set(EXECUTABLE_NAME "lemonade.exe")
else()
    set(EXECUTABLE_NAME "lemonade")
endif()

# Copy resources at build time
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade/tools/server/static
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade_server/server_models.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
```

### Testing Strategy

#### 1. Unit Tests
- Use Google Test framework
- Test each component in isolation
- Mock external dependencies
- Achieve >80% code coverage

#### 2. Integration Tests
- Reuse existing Python test suite
- Add `--server-type=cpp` flag to test runner
- Ensure feature parity with Python implementation
- Test all supported model types

#### 3. System Tests
- End-to-end testing with real models
- Performance benchmarking vs Python
- Memory usage profiling
- Stress testing with concurrent requests

#### 4. Platform Tests
- Automated testing on Windows, Ubuntu, macOS
- Test installer integration on Windows
- Verify static linking and dependencies
- Test with various Python client libraries

### CI/CD Pipeline

The CI/CD pipeline will leverage the existing self-hosted runner infrastructure with specific hardware capabilities (NPU, GPU) as documented in `docs/self_hosted_runners.md`.

#### GitHub Actions Workflows

##### 1. test_lemon_cpp_flm.yml (NPU Testing)
```yaml
name: Test lemon.cpp with FLM 🌩️

on:
  push:
    branches: ["main"]
    paths:
      - 'src/cpp/**'
      - '.github/workflows/test_lemon_cpp_flm.yml'
  pull_request:
    paths:
      - 'src/cpp/**'
  workflow_dispatch:

jobs:
  build-lemon-cpp:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build lemon.cpp
        run: |
          cd src/cpp
          mkdir build && cd build
          cmake .. -DCMAKE_BUILD_TYPE=Release
          make -j$(nproc)
      - name: Upload artifacts
        uses: actions/upload-artifact@v3
        with:
          name: lemon-cpp-linux
          path: src/cpp/build/lemonade

  test-flm-npu:
    needs: build-lemon-cpp
    runs-on: [stx, Windows]  # Self-hosted runner with NPU
    env:
      LEMONADE_CI_MODE: "True"
      PYTHONIOENCODING: utf-8
    steps:
      - uses: actions/checkout@v3
      - name: Download lemon.cpp
        uses: actions/download-artifact@v3
        with:
          name: lemon-cpp-linux
      
      - name: Build Windows version
        shell: PowerShell
        run: |
          cd src/cpp
          mkdir build
          cd build
          cmake .. -G "Visual Studio 17 2022"
          cmake --build . --config Release
          
      - name: Install FLM
        shell: PowerShell
        run: |
          # Download and install FLM (reuse existing logic)
          Invoke-WebRequest -Uri "https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe" -OutFile "flm-setup.exe"
          .\flm-setup.exe /VERYSILENT
          
      - name: Run FLM NPU tests with lemon.cpp
        shell: PowerShell
        env:
          LEMONADE_CACHE_DIR: ".\\ci-cache"
        run: |
          # Use lemon.cpp instead of Python implementation
          .\src\cpp\build\Release\lemonade.exe pull llama3.2:1b
          python test/server_flm.py --server-binary .\src\cpp\build\Release\lemonade.exe
```

##### 2. test_lemon_cpp_llamacpp.yml (GPU Testing)
```yaml
name: Test lemon.cpp with Llamacpp 🌩️

on:
  push:
    branches: ["main"]
    paths:
      - 'src/cpp/**'
      - '.github/workflows/test_lemon_cpp_llamacpp.yml'
  pull_request:
    paths:
      - 'src/cpp/**'
  workflow_dispatch:

jobs:
  test-llamacpp-windows:
    strategy:
      matrix:
        include:
          - backend: vulkan
            runner: [stx, Windows]
          - backend: rocm
            runner: [stx-halo, Windows]
    
    runs-on: ${{ matrix.runner }}
    env:
      LEMONADE_CI_MODE: "True"
      LEMONADE_CACHE_DIR: ./ci-cache
      PYTHONIOENCODING: utf-8
    
    steps:
      - uses: actions/checkout@v3
      
      - name: Build lemon.cpp
        shell: PowerShell
        run: |
          cd src/cpp
          mkdir build
          cd build
          cmake .. -G "Visual Studio 17 2022"
          cmake --build . --config Release
          
      - name: Set up Python environment
        shell: PowerShell
        run: |
          # Minimal Python setup for running tests
          conda create -p .\test-env python=3.10 -y
          conda run -p .\test-env pip install -r test/requirements.txt
          
      - name: Run lemon.cpp llamacpp tests
        shell: PowerShell
        run: |
          # Test with C++ server instead of Python
          .\src\cpp\build\Release\lemonade.exe pull unsloth/Qwen3-0.6B-GGUF:Q4_0
          conda run -p .\test-env python test/server_llamacpp.py ${{ matrix.backend }} --server-binary .\src\cpp\build\Release\lemonade.exe
          
  test-llamacpp-ubuntu:
    strategy:
      matrix:
        include:
          - backend: rocm
            runner: [stx-halo, Linux]
          - backend: vulkan
            runner: [stx, Linux]
    
    runs-on: ${{ matrix.runner }}
    
    steps:
      - uses: actions/checkout@v3
      
      - name: Build lemon.cpp
        run: |
          cd src/cpp
          mkdir build && cd build
          cmake .. -DCMAKE_BUILD_TYPE=Release
          make -j$(nproc)
          
      - name: Set up Python environment
        run: |
          conda create -p ./test-env python=3.10 -y
          conda run -p ./test-env pip install -r test/requirements.txt
          
      - name: Run lemon.cpp llamacpp tests
        run: |
          ./src/cpp/build/lemonade pull unsloth/Qwen3-0.6B-GGUF:Q4_0
          conda run -p ./test-env python test/server_llamacpp.py ${{ matrix.backend }} --server-binary ./src/cpp/build/lemonade

  test-llamacpp-macos:
    runs-on: macos-latest
    strategy:
      matrix:
        backend: ['metal']
    
    steps:
      - uses: actions/checkout@v3
      
      - name: Build lemon.cpp
        run: |
          cd src/cpp
          mkdir build && cd build
          cmake .. -DCMAKE_BUILD_TYPE=Release
          make -j$(sysctl -n hw.ncpu)
          
      - name: Set up Python environment
        run: |
          conda create -p ./test-env python=3.10 -y
          conda activate ./test-env
          pip install -r test/requirements.txt
          
      - name: Run lemon.cpp tests
        run: |
          ./src/cpp/build/lemonade pull unsloth/Qwen3-0.6B-GGUF:Q4_0
          python test/server_llamacpp.py metal --server-binary ./src/cpp/build/lemonade
```

#### Key CI/CD Considerations

1. **Self-Hosted Runner Usage**
   - Use `[stx, Windows]` for NPU testing with STX hardware
   - Use `[stx-halo, Windows/Linux]` for ROCm GPU testing
   - Use GitHub-hosted runners for generic builds and macOS

2. **Test Integration**
   - Modify `test/utils/server_base.py` to accept `--server-binary` parameter
   - This allows reusing existing Python tests with C++ binary
   - Tests should work identically with both Python and C++ servers

3. **Build Artifacts**
   - Cross-compile or build on target platforms
   - Upload artifacts for distribution
   - Cache builds where possible

4. **Resource Management**
   - Keep test runs under 15 minutes
   - Use local cache directories (wiped after each run)
   - Avoid installing persistent software on runners

### Dependencies and Libraries

All required dependencies use permissive licenses (MIT, BSD, Apache 2.0) that are compatible with the project's Apache 2.0 license.

#### Required Libraries
1. **cpp-httplib** - HTTP server implementation (MIT License) - Note: Does not support WebSocket, need separate library
2. **nlohmann/json** - JSON parsing (MIT License)
3. **CLI11** - Command line parsing (BSD 3-Clause License)
4. **spdlog** - Logging, optional (MIT License)
5. **libcurl** - HTTP client for downloads (MIT/X derivate license)
6. **websocketpp** or **uWebSockets** - WebSocket support for log streaming (BSD 3-Clause / Apache 2.0)

#### Optional Libraries
1. **Google Test** - Unit testing (BSD 3-Clause License)
2. **Google Benchmark** - Performance testing (Apache 2.0 License)
3. **Valgrind/AddressSanitizer** - Memory debugging (GPL v2 / Apache 2.0 with LLVM exceptions)

### Performance Goals

1. **Startup Time**: < 100ms (vs Python ~1s)
2. **Memory Usage**: < 50MB base (vs Python ~200MB)
3. **Request Latency**: < 1ms overhead per request
4. **Binary Size**: < 10MB compressed
5. **Installation Time**: < 5 seconds

### Model Download Strategy

Since C++ doesn't have a direct equivalent to the `huggingface_hub` Python package, we need to implement model downloading using direct HTTP downloads with libcurl.

#### Implementation Approach
- Use libcurl for HTTP/HTTPS downloads
- Implement our own progress callbacks
- Handle authentication via HF tokens (environment variable)
- Manually construct download URLs using HF's CDN patterns
- Implement simple caching in local directories
- Full control over download process, minimal dependencies, predictable behavior

#### Implementation Details:

Key features to implement:
1. **Repository Downloads** (equivalent to `snapshot_download`)
   - Download entire model repositories
   - Support for specific file patterns (e.g., only GGUF files)
   - Cache management compatible with huggingface_hub's structure

2. **Model Metadata** (equivalent to `model_info`)
   - Query model information via HF API
   - Extract base model information
   - Check model compatibility

3. **Authentication & Network**
   - Support HF_TOKEN environment variable
   - Handle offline mode (LEMONADE_OFFLINE env var)
   - Retry logic for network failures
   - Windows symlink privilege handling

4. **Cache Structure**
   - Use same directory structure as huggingface_hub:
     - Windows: `%USERPROFILE%\.cache\huggingface\hub`
     - Linux/macOS: `~/.cache/huggingface/hub`
   - Support local_files_only mode
   - Handle cache invalidation

```cpp
class ModelDownloader {
private:
    std::string cache_dir;
    std::string hf_token;  // From HF_TOKEN env var
    bool offline_mode;     // From LEMONADE_OFFLINE env var
    
public:
    // Download entire model repository (like snapshot_download)
    std::string download_snapshot(
        const std::string& repo_id,
        const std::vector<std::string>& allow_patterns = {},
        bool local_files_only = false,
        bool do_not_upgrade = false,
        ProgressCallback callback = nullptr
    );
    
    // Get model metadata (like model_info)
    ModelInfo get_model_info(
        const std::string& repo_id
    );
    
    // List repository files (like list_repo_files)
    std::vector<std::string> list_repo_files(
        const std::string& repo_id,
        const std::string& revision = "main"
    );
    
    // Check if system is offline
    bool is_offline();
    
private:
    // Construct cache path compatible with huggingface_hub
    std::string get_cache_path(
        const std::string& repo_id,
        const std::string& revision
    );
    
    // Download individual file with resume support
    bool download_file_with_resume(
        const std::string& url,
        const std::string& output_path,
        ProgressCallback callback = nullptr
    );
};
```

**Implementation Notes**:
- Use libcurl's multi interface for parallel downloads
- Implement ETag-based cache validation
- Support chunked downloads for large files (>5GB)
- Parse HF's JSON responses for metadata
- Create symlinks on Unix, copy files on Windows without symlink privileges

### Risk Mitigation

1. **Cross-platform Compatibility**
   - Early testing on all platforms
   - Use standard C++17 features only
   - Abstract platform-specific code

2. **Backend Integration**
   - Maintain version compatibility tables
   - Graceful degradation for missing backends
   - Clear error messages

3. **Performance**
   - Profile early and often
   - Avoid unnecessary allocations
   - Use move semantics appropriately

4. **Maintenance**
   - Keep Python and C++ implementations in sync
   - Document all deviations
   - Maintain comprehensive test coverage

5. **Model Download Compatibility**
   - Use same cache directory structure as huggingface_hub
   - Support HF_TOKEN environment variable
   - Implement retry logic for failed downloads
   - Validate downloaded files with checksums