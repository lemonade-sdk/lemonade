# Build Instructions for lemon.cpp

## Current Status

**Phase 1 (Core Infrastructure) is IN PROGRESS**

✅ **Completed:**
- CMake build configuration
- Complete header file architecture (10 header files)
- CLI parser with all 7 subcommands
- Main entry point

🚧 **In Progress:**
- Server implementation
- Router implementation
- Model manager implementation
- Utility implementations
- Backend implementations

## Quick Start

### 1. Prerequisites

Ensure you have:
- **CMake** 3.20 or higher
- **C++17** compatible compiler:
  - Windows: Visual Studio 2019+ or MinGW-w64
  - Ubuntu: GCC 7+ (`sudo apt install build-essential cmake`)
  - macOS: Xcode command line tools
- **Git** (for fetching dependencies)
- **Internet connection** (first build downloads dependencies)
- **miniforge** (to get zstd.dll)

### 2. Build

```bash
# Navigate to the C++ source directory
cd src/cpp

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build (choose one based on your system)
# Linux/macOS:
cmake --build . --config Release

# Windows with Visual Studio:
cmake --build . --config Release

# Or use ninja for faster builds:
cmake -G Ninja ..
ninja
```

### 3. Run

After building successfully:

```bash
# Show version
./lemonade --version

# Show help
./lemonade --help

# Start server (when implementation is complete)
./lemonade serve --port 8000

# Run with a model (when implementation is complete)
./lemonade run Llama-3.2-1B-Instruct-Hybrid
```

## Development Build

For development with debug symbols:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .

# Run with debugger
gdb ./lemonade
# or on macOS:
lldb ./lemonade
```

## Build Output

- **Windows**: `build/Release/lemonade-router.exe` or `build/Debug/lemonade-router.exe`
- **Linux/macOS**: `build/lemonade`

## Dependencies (auto-downloaded)

The following libraries are automatically fetched during build:

1. **cpp-httplib** (v0.14.0) - HTTP server [MIT License]
2. **nlohmann/json** (v3.11.2) - JSON parsing [MIT License]
3. **CLI11** (v2.3.2) - Command line parsing [BSD 3-Clause]

Note: These will be downloaded on first build and cached.

## Platform-Specific Notes

### Windows

- If using Visual Studio, open "x64 Native Tools Command Prompt"
- The executable will be in `build/Release/lemonade-router.exe`
- WebSocket library may need additional configuration

### Ubuntu/Linux

- Install dependencies: `sudo apt install build-essential cmake git libcurl4-openssl-dev`
- The executable will be in `build/lemonade`
- May need `libssl-dev` for HTTPS support

### macOS

- Install Xcode: `xcode-select --install`
- The executable will be in `build/lemonade`
- Metal backend is used by default for llama.cpp on ARM Macs

## Troubleshooting

### CMake version too old
```bash
# Ubuntu
sudo apt install cmake

# macOS
brew install cmake

# Or download from https://cmake.org/download/
```

### FetchContent fails
- Ensure you have internet connection
- Check firewall isn't blocking git/https
- Delete `build/_deps` and try again

### Compiler not found
```bash
# Ubuntu
sudo apt install g++

# macOS
xcode-select --install
```

### Link errors
- Ensure all header files are present
- Check that CMakeLists.txt includes all source files
- Verify dependencies downloaded correctly in `build/_deps/`

## Next Steps

Once the implementation is complete:

1. Test CLI: `./lemonade --help`
2. Test serve: `./lemonade serve`
3. Run Python tests: `python test/server_llamacpp.py vulkan --server-binary ./build/lemonade`
4. Performance benchmark: Compare startup time vs Python

## Contributing

See `DEVELOPER_GUIDE.md` for:
- Architecture overview
- Coding standards
- How to add new features
- Testing procedures

## Documentation

- `README.md` - Complete implementation plan and specification
- `IMPLEMENTATION_STATUS.md` - Current progress tracking
- `DEVELOPER_GUIDE.md` - Developer documentation
- `BUILD_INSTRUCTIONS.md` - This file

