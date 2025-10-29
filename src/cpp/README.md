# Lemonade C++ Server

This directory contains the C++ implementation of the Lemonade Server, providing a lightweight, high-performance alternative to the Python implementation.

## Components

- **lemonade-router** - Core server executable that handles HTTP requests, model management, and LLM backend orchestration
- **lemonade-server-beta** - System tray application for Windows/macOS/Linux that manages the server process

## Building from Source

### Prerequisites

**All Platforms:**
- CMake 3.20 or higher
- C++17 compatible compiler
- Git (for fetching dependencies)
- Internet connection (first build downloads dependencies)

**Windows:**
- Visual Studio 2019 or later
- Miniforge or Miniconda (provides zstd.dll dependency)
- NSIS 3.x (only required for building the installer)

**Linux (Ubuntu/Debian):**
```bash
sudo apt install build-essential cmake libcurl4-openssl-dev
# For tray application:
sudo apt install libappindicator3-dev libgtk-3-dev libnotify-dev pkg-config
```

**macOS:**
```bash
# Install Xcode command line tools
xcode-select --install
```

### Build Steps

```bash
# Navigate to the C++ source directory
cd src/cpp

# Create and enter build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build
cmake --build . --config Release

# On Windows, executables will be in: build/Release/
# On Linux/macOS, executables will be in: build/
```

### Build Outputs

- **Windows:** `build/Release/lemonade-router.exe` and `build/Release/lemonade-server-beta.exe`
- **Linux/macOS:** `build/lemonade-router` and `build/lemonade-server-beta`
- **Resources:** Automatically copied to `build/Release/resources/` (web UI files, model registry)

### RyzenAI Serve Dependency

The `lemonade-router` server has a runtime dependency on `ryzenai-serve` for NPU model inference. This dependency can be fulfilled in two ways:

1. **Development builds:** Build `ryzenai-serve` from source in the same repository:
   ```bash
   # Build ryzenai-serve
   cd src/ryzenai-serve
   mkdir build && cd build
   cmake .. -G "Visual Studio 17 2022"
   cmake --build . --config Release
   
   # The executable will be at: src/ryzenai-serve/build/bin/Release/ryzenai-serve.exe
   ```

2. **Runtime download:** For end users, `lemonade-router` will automatically download the `ryzenai-serve` executable from GitHub releases as needed when attempting to run NPU models.

### Platform-Specific Notes

**Windows:**
- The build uses static linking to minimize DLL dependencies
- `zstd.dll` is automatically copied from your system PATH if available (provided by Miniforge/Miniconda)
- If zstd.dll is not found, the build will complete but the user will need to install Miniforge
- Security features enabled: Control Flow Guard, ASLR, DEP

**Linux:**
- System tray requires GTK3 and libappindicator3
- Different desktop environments may have varying tray support
- ⚠️ **Note:** Linux build is currently a stub implementation and not fully functional

**macOS:**
- Uses native system frameworks (Cocoa, Foundation)
- ARM Macs use Metal backend by default for llama.cpp
- ⚠️ **Note:** macOS build is currently a stub implementation and not fully functional

## Building the Windows Installer

### Prerequisites
- NSIS 3.x installed at `C:\Program Files (x86)\NSIS\`
- Completed C++ build (see above)

### Building

**Using PowerShell script (recommended):**
```powershell
cd src\cpp
.\build_installer.ps1
```

**Manual build:**
```powershell
cd src\cpp
"C:\Program Files (x86)\NSIS\makensis.exe" Lemonade_Server_Installer_beta.nsi
```

### Installer Output

Creates `Lemonade_Server_Installer_beta.exe` which:
- Installs to `%LOCALAPPDATA%\lemonade_server_beta\`
- Adds `bin\` folder to user PATH
- Creates Start Menu shortcuts
- Optionally creates desktop shortcut and startup entry
- Includes uninstaller

## Code Structure

```
src/cpp/
├── CMakeLists.txt              # Main build configuration
├── build_installer.ps1         # Installer build script
├── Lemonade_Server_Installer_beta.nsi  # NSIS installer definition
│
├── server/                     # Server implementation
│   ├── main.cpp                # Entry point, CLI routing
│   ├── server.cpp              # HTTP server (cpp-httplib)
│   ├── router.cpp              # Routes requests to backends
│   ├── model_manager.cpp       # Model registry, downloads, caching
│   ├── cli_parser.cpp          # Command-line argument parsing (CLI11)
│   ├── wrapped_server.cpp      # Base class for backend wrappers
│   ├── streaming_proxy.cpp     # Server-Sent Events for streaming
│   ├── system_info.cpp         # NPU/GPU device detection
│   │
│   ├── backends/               # LLM backend implementations
│   │   ├── llamacpp_server.cpp # Wraps llama.cpp (CPU/GPU)
│   │   ├── fastflowlm_server.cpp # Wraps FastFlowLM (NPU)
│   │   └── ryzenaiserver.cpp   # Wraps RyzenAI server
│   │
│   └── utils/                  # Utility functions
│       ├── http_client.cpp     # HTTP client using libcurl
│       ├── json_utils.cpp      # JSON file I/O
│       ├── process_manager.cpp # Cross-platform process management
│       ├── path_utils.cpp      # Path manipulation
│       └── wmi_helper.cpp      # Windows WMI for NPU detection
│
├── include/lemon/              # Public headers
│   ├── server.h, router.h, model_manager.h, cli_parser.h
│   ├── wrapped_server.h, streaming_proxy.h, system_info.h
│   ├── backends/               # Backend headers
│   │   ├── llamacpp_server.h
│   │   ├── fastflowlm_server.h
│   │   └── ryzenaiserver.h
│   └── utils/                  # Utility headers
│       ├── http_client.h, json_utils.h
│       ├── process_manager.h, path_utils.h
│
└── tray/                       # System tray application
    ├── CMakeLists.txt          # Tray-specific build config
    ├── main.cpp                # Tray entry point
    ├── server_manager.cpp      # Manages lemonade-router process
    ├── tray_app.cpp            # Main tray application logic
    └── platform/               # Platform-specific implementations
        ├── windows_tray.cpp    # Win32 system tray API
        ├── macos_tray.mm       # Objective-C++ NSStatusBar
        ├── linux_tray.cpp      # GTK/AppIndicator
        └── tray_factory.cpp    # Platform detection
```

## Architecture Overview

### Server Architecture

The server is organized into several key layers:

**HTTP Layer:** Uses cpp-httplib to serve OpenAI-compatible REST API endpoints. Supports both `/api/v0` and `/api/v1` prefixes for backward compatibility.

**Router:** Determines which backend (llamacpp, fastflowlm, or ryzenai) should handle a request based on the loaded model's recipe.

**Model Manager:** Handles model discovery, downloads from Hugging Face, caching (compatible with `huggingface_hub` cache structure), and model registry management (server_models.json).

**Backend Wrappers:** Each LLM backend (llama.cpp, FastFlowLM, RyzenAI) is wrapped in a common interface. The wrapper manages:
- Backend installation and versioning
- Process lifecycle (spawn, monitor, terminate)
   - Model loading/unloading
- Request proxying and response streaming
- Performance telemetry extraction

**Utilities:**
- HTTP client for downloads and API calls (libcurl)
- Cross-platform process management
- JSON file I/O
- Path utilities with Windows/Unix compatibility

### Tray Application

The tray application is a separate executable that:
- Spawns and manages the `lemonade-router` server process
- Provides platform-native system tray integration
- Offers a context menu for:
  - Loading/unloading models
  - Changing server port and context size
  - Opening web UI and documentation
  - Viewing logs
  - Quitting the server
- Monitors model state and checks for updates
- Can run in headless mode with `--no-tray` flag

### Dependencies

All dependencies are automatically fetched by CMake via FetchContent:

- **cpp-httplib** (v0.26.0) - HTTP server with thread pool support [MIT License]
- **nlohmann/json** (v3.11.3) - JSON parsing and serialization [MIT License]
- **CLI11** (v2.4.2) - Command-line argument parsing [BSD 3-Clause]
- **libcurl** (8.5.0) - HTTP client for model downloads [curl license]

Platform-specific SSL backends are used (Schannel on Windows, SecureTransport on macOS, OpenSSL on Linux).

## Usage

### Command-Line Server

```bash
# Check version
./lemonade-router --version

# List available models
./lemonade-router list

# Start server
./lemonade-router serve --port 8000

# Pull a model
./lemonade-router pull Llama-3.2-1B-Instruct-CPU

# Run server with a specific model
./lemonade-router run Llama-3.2-1B-Instruct-CPU
```

### Tray Application

The tray application provides a GUI for managing the server:

```bash
# Launch with system tray (default)
./lemonade-server-beta

# Launch without tray (headless mode)
./lemonade-server-beta --no-tray

# Custom configuration
./lemonade-server-beta --port 8080 --ctx-size 8192
```

**Tray features:**
- System tray icon with context menu
- Load/unload models via menu
- Change server port and context size
- Open web UI, documentation, and logs
- Background model monitoring
- Version update notifications

**Command-line options:**
- `--port PORT` - Server port (default: 8000)
- `--ctx-size SIZE` - Context size (default: 4096)
- `--log-file PATH` - Custom log file location
- `--log-level LEVEL` - Log level (debug, info, warning, error)
- `--server-binary PATH` - Path to lemonade-router executable
- `--no-tray` - Run without tray (headless mode)

## Testing

### Basic Functionality Tests

Run the commands from the Usage section above to verify basic functionality.

### Integration Tests

The C++ implementation is tested using the existing Python test suite.

**Prerequisites:**
- Python 3.10+ (Miniforge or Miniconda recommended)
- Test dependencies: `pip install -r test/requirements.txt`

**Running tests:**
```bash
# Run llamacpp backend tests
python test/server_llamacpp.py vulkan --server-binary ./src/cpp/build/Release/lemonade-router.exe

# Run FastFlowLM backend tests
python test/server_flm.py --server-binary ./src/cpp/build/Release/lemonade-router.exe
```

See the `.github/workflows/` directory for CI/CD test configurations.

## Development

### Code Style

- C++17 standard
- Snake_case for functions and variables
- CamelCase for classes and types
- 4-space indentation
- Header guards using `#pragma once`
- All code in `lemon::` namespace

### Key Resources

- **API Specification:** `docs/server/server_spec.md`
- **Model Registry:** `src/lemonade_server/server_models.json`
- **Web UI Files:** `src/lemonade/tools/server/static/`
- **Python Reference:** `src/lemonade_server/` and `src/lemonade/tools/server/`

### Adding New Features

When adding functionality, ensure compatibility with the Python implementation:
1. Review the Python reference implementation
2. Implement the C++ equivalent
3. Test using the Python test suite
4. Update API documentation if adding new endpoints

## License

This project is licensed under the Apache 2.0 License. All dependencies use permissive licenses (MIT, BSD, Apache 2.0, curl license).
