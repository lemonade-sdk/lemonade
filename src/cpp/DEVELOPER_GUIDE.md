# Lemon.cpp Developer Guide

## Project Overview

`lemon.cpp` is a C++ implementation of the Lemonade LLM server, designed to be:
- **Fast**: < 100ms startup time
- **Lightweight**: < 50MB memory footprint
- **Compatible**: Drop-in replacement for Python implementation
- **Cross-platform**: Windows, Ubuntu, macOS

## Architecture

### Core Components

```
lemon::Server
  ├── Handles HTTP requests via cpp-httplib
  ├── Manages CORS, static files, WebSocket
  └── Routes to lemon::Router

lemon::Router
  ├── Manages model loading/unloading
  ├── Selects appropriate WrappedServer
  └── Forwards requests to backends

lemon::WrappedServer (abstract)
  ├── lemon::backends::LlamaCppServer
  │   └── Wraps llama-server for GGUF models
  └── lemon::backends::FastFlowLMServer
      └── Wraps flm serve for NPU models

lemon::ModelManager
  ├── Parses server_models.json
  ├── Manages user_models.json
  └── Coordinates downloads
```

### Request Flow

```
Client Request
  ↓
Server (HTTP endpoint)
  ↓
Router (determine backend)
  ↓
WrappedServer (forward to llama-server or flm)
  ↓
Parse Response & Add Telemetry
  ↓
Return to Client
```

## Building

### Prerequisites

- CMake 3.20+
- C++17 compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Git (for FetchContent dependencies)

### Build Steps

```bash
cd src/cpp
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### Platform-Specific Notes

**Windows:**
- Use Visual Studio 2019+ or MinGW-w64
- Will build `lemonade.exe`

**Ubuntu:**
- Install build-essential: `sudo apt install build-essential cmake`
- Will build `lemonade`

**macOS:**
- Install Xcode command line tools
- Will build `lemonade`

## Development Workflow

### Adding a New Endpoint

1. Add handler declaration in `include/lemon/server.h`
2. Implement handler in `src/server.cpp`
3. Register route in `Server::setup_routes()`
4. Add both `/api/v0` and `/api/v1` prefixes
5. Test with Python test suite

### Adding a New Backend

1. Create header in `include/lemon/backends/`
2. Inherit from `lemon::WrappedServer`
3. Implement all virtual methods
4. Add backend selection logic in `Router`
5. Update model registry filters

### Debugging

Enable debug logging:
```bash
./lemonade serve --log-level debug
```

Use GDB/LLDB:
```bash
gdb ./lemonade
(gdb) run serve --port 8000
```

## Testing

### Unit Tests (TODO)
```bash
cd build
ctest
```

### Integration Tests

Use existing Python tests with `--server-binary` flag:
```bash
python test/server_llamacpp.py vulkan --server-binary ./src/cpp/build/lemonade
```

## Code Style

- Use snake_case for functions and variables
- Use CamelCase for classes
- 4-space indentation
- Curly braces on same line for functions/classes
- Include guards using `#pragma once`
- Namespace all code under `lemon::`

## Common Issues

### Build Fails with "FetchContent not found"
Upgrade CMake to 3.20+

### Cannot find httplib.h
Ensure FetchContent is working and you have internet connection during first build

### Process spawn fails on Windows
Check that you have proper permissions and the executable path is correct

## Performance Targets

- Startup time: < 100ms
- Memory usage: < 50MB base
- Request overhead: < 1ms
- Binary size: < 10MB (compressed)

## Contributing

1. Follow the implementation plan in `README.md`
2. Maintain compatibility with Python implementation
3. Pass all existing Python tests
4. Update `IMPLEMENTATION_STATUS.md` with progress
5. Document any deviations from the plan

## Resources

- [cpp-httplib documentation](https://github.com/yhirose/cpp-httplib)
- [nlohmann/json documentation](https://json.nlohmann.me/)
- [CLI11 documentation](https://cliutils.github.io/CLI11/book/)
- [Python reference implementation](../../src/lemonade/tools/server/)
- [API specification](../../docs/server/server_spec.md)

