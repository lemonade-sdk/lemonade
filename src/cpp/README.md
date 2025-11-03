# Lemonade C++ Server

This directory contains the C++ implementation of the Lemonade Server, providing a lightweight, high-performance alternative to the Python implementation.

## Components

- **lemonade-router.exe** - Core HTTP server executable that handles requests and LLM backend orchestration
- **lemonade-server-beta.exe** - Console CLI client for terminal users that manages server lifecycle, executes commands via HTTP API
- **lemonade-tray.exe** (Windows only) - GUI tray launcher for desktop users, automatically starts `lemonade-server-beta.exe serve`
- **lemonade-log-viewer.exe** (Windows only) - Log file viewer with live tail support and installer-friendly file sharing

## Building from Source

### Prerequisites

**All Platforms:**
- CMake 3.20 or higher
- C++17 compatible compiler
- Git (for fetching dependencies)
- Internet connection (first build downloads dependencies)

**Windows:**
- Visual Studio 2019 or later
- NSIS 3.x (only required for building the installer)

**Linux (Ubuntu/Debian):**
```bash
sudo apt install build-essential cmake libcurl4-openssl-dev pkg-config
# Note: Tray application is disabled on Linux (headless mode only)
# This avoids LGPL dependencies and provides a cleaner server-only experience
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

- **Windows:** 
  - `build/Release/lemonade-router.exe` - HTTP server
  - `build/Release/lemonade-server-beta.exe` - Console CLI client
  - `build/Release/lemonade-tray.exe` - GUI tray launcher
  - `build/Release/lemonade-log-viewer.exe` - Log file viewer
- **Linux/macOS:** 
  - `build/lemonade-router` - HTTP server
  - `build/lemonade-server-beta` - Console CLI client
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
- All dependencies are built from source (no external DLL requirements)
- Security features enabled: Control Flow Guard, ASLR, DEP

**Linux:**
- Linux builds are headless-only (no tray application) by default
- This avoids LGPL dependencies (GTK3, libappindicator3, libnotify)
- Run server using: `lemonade-server-beta serve` (headless mode is automatic)
- Fully functional for server operations and model management
- Uses permissively licensed dependencies only (MIT, Apache 2.0, BSD, curl license)
- Clean .deb package with only runtime files (no development headers)
- PID file system (`/tmp/lemonade-router.pid`) for reliable process management
- Proper graceful shutdown - all child processes cleaned up correctly
- File locations:
  - Installed binaries: `/usr/local/bin/`
  - llama.cpp downloads: `~/.cache/huggingface/` (follows HF conventions)
  - llama-server binaries: `/usr/local/share/lemonade-server/llama/` (from .deb) or next to binary (dev builds)

**macOS:**
- Uses native system frameworks (Cocoa, Foundation)
- ARM Macs use Metal backend by default for llama.cpp
- ⚠️ **Note:** macOS build is currently a stub implementation and not fully functional

## Building Installers

### Windows Installer (NSIS)

**Prerequisites:**
- NSIS 3.x installed at `C:\Program Files (x86)\NSIS\`
- Completed C++ build (see above)

**Building:**

Using PowerShell script (recommended):
```powershell
cd src\cpp
.\build_installer.ps1
```

Manual build:
```powershell
cd src\cpp
"C:\Program Files (x86)\NSIS\makensis.exe" Lemonade_Server_Installer_beta.nsi
```

**Installer Output:**

Creates `Lemonade_Server_Installer_beta.exe` which:
- Installs to `%LOCALAPPDATA%\lemonade_server_beta\`
- Adds `bin\` folder to user PATH
- Creates Start Menu shortcuts (launches `lemonade-tray.exe`)
- Optionally creates desktop shortcut and startup entry
- Gracefully stops running server before install/uninstall
- Includes all executables (router, server-beta, tray, log-viewer)
- Includes uninstaller

**Installation Process:**
- Automatically detects and stops running Lemonade instances using `lemonade-server-beta.exe stop`
- Prevents "files in use" errors during installation
- Works gracefully on fresh installs (no existing installation)

### Linux .deb Package (Debian/Ubuntu)

**Prerequisites:**
- Completed C++ build (see above)

**Building:**

```bash
cd src/cpp/build
cpack
```

**Package Output:**

Creates `lemonade-server-1.0.0-Linux.deb` which:
- Installs to `/usr/local/bin/` (executables)
- Installs resources to `/usr/local/share/lemonade-server/`
- Creates desktop entry in `/usr/local/share/applications/`
- Declares dependencies: libcurl4, libssl3, libz1
- Package size: ~2.2 MB (clean, runtime-only package)
- Includes postinst script that creates writable `/usr/local/share/lemonade-server/llama/` directory

**Installation:**

```bash
sudo dpkg -i lemonade-server-1.0.0-Linux.deb

# If dependencies are missing:
sudo apt-get install -f
```

**Uninstallation:**

```bash
sudo dpkg -r lemonade-server
```

**Post-Installation:**

The executables will be available in PATH:
```bash
lemonade-server-beta --help
lemonade-router --help

# Start server in headless mode:
lemonade-server-beta serve --no-tray

# Or just:
lemonade-server-beta serve
```

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
    ├── main.cpp                # Tray entry point (lemonade-server-beta)
    ├── tray_launcher.cpp       # GUI launcher (lemonade-tray)
    ├── log-viewer.cpp          # Log file viewer (lemonade-log-viewer)
    ├── server_manager.cpp      # Manages lemonade-router process
    ├── tray_app.cpp            # Main tray application logic
    └── platform/               # Platform-specific implementations
        ├── windows_tray.cpp    # Win32 system tray API
        ├── macos_tray.mm       # Objective-C++ NSStatusBar
        ├── linux_tray.cpp      # GTK/AppIndicator
        └── tray_factory.cpp    # Platform detection
```

## Architecture Overview

### Overview

The Lemonade Server C++ implementation uses a client-server architecture:

#### lemonade-router (Server Component)

A pure HTTP server that:
- Serves OpenAI-compatible REST API endpoints (supports both `/api/v0` and `/api/v1`)
- Routes requests to appropriate LLM backends (llamacpp, fastflowlm, ryzenai)
- Manages model loading/unloading and backend processes
- Handles all inference requests
- No command-based user interface - only accepts startup options

**Key Layers:**
- **HTTP Layer:** Uses cpp-httplib for HTTP server
- **Router:** Determines which backend handles each request based on model recipe
- **Model Manager:** Handles model discovery, downloads, and registry management
- **Backend Wrappers:** Manages llama.cpp, FastFlowLM, and RyzenAI backends

#### lemonade-server-beta (CLI Client Component)

A console application for terminal users:
- Provides command-based user interface (`list`, `pull`, `delete`, `run`, `status`, `stop`, `serve`)
- Manages server lifecycle (start/stop persistent or ephemeral servers)
- Communicates with `lemonade-router` via HTTP endpoints
- Starts `lemonade-router` with appropriate options
- Provides optional system tray interface via `serve` command

**Command Types:**
- **serve:** Starts a persistent server (with optional tray interface)
- **run:** Starts persistent server, loads model, opens browser
- **Other commands:** Use existing server or start ephemeral server, execute command via API, auto-cleanup

#### lemonade-tray (GUI Launcher - Windows Only)

A minimal WIN32 GUI application for desktop users:
- Simple launcher that starts `lemonade-server-beta.exe serve`
- Zero console output or CLI interface
- Used by Start Menu, Desktop shortcuts, and autostart
- Provides seamless GUI experience for non-technical users

### Client-Server Communication

The `lemonade-server-beta` client communicates with `lemonade-router` server via HTTP:
- **Model operations:** `/api/v1/models`, `/api/v1/pull`, `/api/v1/delete`
- **Model control:** `/api/v1/load`, `/api/v1/unload`
- **Server management:** `/api/v1/health`, `/internal/shutdown`
- **Inference:** `/api/v1/chat/completions`, `/api/v1/completions`

The client automatically:
- Detects if a server is already running
- Starts ephemeral servers for one-off commands
- Cleans up ephemeral servers after command completion
- Manages persistent servers with proper lifecycle handling

**Single-Instance Protection:**
- Each component (`lemonade-router`, `lemonade-server-beta serve`, `lemonade-tray`) enforces single-instance using system-wide mutexes
- Only the `serve` command is blocked when a server is running
- Commands like `status`, `list`, `pull`, `delete`, `stop` can run alongside an active server
- Provides clear error messages with suggestions when blocked
- **Linux-specific:** Uses PID file (`/tmp/lemonade-router.pid`) for efficient server discovery and port detection
  - Avoids port scanning, finds exact server PID and port instantly
  - Validated on read (checks if process is still alive)
  - Automatically cleaned up on graceful shutdown

### Dependencies

All dependencies are automatically fetched by CMake via FetchContent:

- **cpp-httplib** (v0.26.0) - HTTP server with thread pool support [MIT License]
- **nlohmann/json** (v3.11.3) - JSON parsing and serialization [MIT License]
- **CLI11** (v2.4.2) - Command-line argument parsing [BSD 3-Clause]
- **libcurl** (8.5.0) - HTTP client for model downloads [curl license]
- **zstd** (v1.5.5) - Compression library for HTTP [BSD License]

Platform-specific SSL backends are used (Schannel on Windows, SecureTransport on macOS, OpenSSL on Linux).

## Usage

### lemonade-router (Server Only)

The `lemonade-router` executable is a pure HTTP server without any command-based interface:

```bash
# Start server with default options
./lemonade-router

# Start server with custom options
./lemonade-router --port 8080 --ctx-size 8192 --log-level debug

# Available options:
#   --port PORT              Port number (default: 8000)
#   --host HOST              Bind address (default: 0.0.0.0)
#   --ctx-size SIZE          Context size (default: 4096)
#   --log-level LEVEL        Log level: critical, error, warning, info, debug, trace
#   --llamacpp BACKEND       LlamaCpp backend: vulkan, rocm, metal
#   --version, -v            Show version
#   --help, -h               Show help
```

### lemonade-server-beta.exe (Console CLI Client)

The `lemonade-server-beta` executable is the command-line interface for terminal users:
- Command-line interface for all model and server management
- Starts persistent servers (with optional tray interface)
- Manages ephemeral servers for one-off commands
- Communicates with `lemonade-router` via HTTP endpoints

```bash
# List available models
./lemonade-server-beta list

# Pull a model
./lemonade-server-beta pull Llama-3.2-1B-Instruct-CPU

# Delete a model
./lemonade-server-beta delete Llama-3.2-1B-Instruct-CPU

# Check server status
./lemonade-server-beta status

# Stop the server
./lemonade-server-beta stop

# Run a model (starts persistent server with tray and opens browser)
./lemonade-server-beta run Llama-3.2-1B-Instruct-CPU

# Start persistent server (with tray on Windows/macOS, headless on Linux)
./lemonade-server-beta serve

# Start persistent server without tray (headless mode, explicit on all platforms)
./lemonade-server-beta serve --no-tray

# Start server with custom options
./lemonade-server-beta serve --port 8080 --ctx-size 8192
```

**Available Options:**
- `--port PORT` - Server port (default: 8000)
- `--host HOST` - Server host (default: localhost)
- `--ctx-size SIZE` - Context size (default: 4096)
- `--log-level LEVEL` - Logging verbosity: info, debug (default: info)
- `--log-file PATH` - Custom log file location
- `--server-binary PATH` - Path to lemonade-router executable
- `--no-tray` - Run without tray (headless mode)

**Note:** `lemonade-router` is always launched with `--log-level debug` for optimal troubleshooting. Use `--log-level debug` on `lemonade-server-beta` commands to see client-side debug output.

### lemonade-tray.exe (GUI Tray Launcher - Windows Only)

The `lemonade-tray` executable is a simple GUI launcher for desktop users:
- Double-click from Start Menu or Desktop to start server
- Automatically runs `lemonade-server-beta.exe serve` in tray mode
- Zero console windows or CLI interface
- Perfect for non-technical users
- Single-instance protection: shows friendly message if already running

**What it does:**
1. Finds `lemonade-server-beta.exe` in the same directory
2. Launches it with the `serve` command
3. Exits immediately (server continues running with tray icon)

**When to use:**
- Launching from Start Menu
- Desktop shortcuts
- Windows startup
- Any GUI/point-and-click scenario

**System Tray Features (when running):**
- Left-click or right-click icon to show menu
- Load/unload models via menu
- Change server port and context size
- Open web UI, documentation, and logs
- "Show Logs" opens log viewer with historical and live logs
- Background model monitoring
- Click balloon notifications to open menu
- Quit option

**UI Improvements:**
- Displays as "Lemonade Local LLM Server" in Task Manager
- Shows large lemon icon in notification balloons
- Single-instance protection prevents multiple tray apps

### Logging and Console Output

When running `lemonade-server-beta.exe serve`:
- **Console Output:** Router logs are streamed to the terminal in real-time via a background tail thread
- **Log File:** All logs are written to a persistent log file (default: `%TEMP%\lemonade-server.log`)
- **Log Viewer:** Click "Show Logs" in the tray to open `lemonade-log-viewer.exe`
  - Displays last 100KB of historical logs
  - Live tails new content as it's written
  - Automatically closes when server stops
  - Uses shared file access (won't block installer)

**Log Viewer Features:**
- Cross-platform tail implementation
- Parent process monitoring for auto-cleanup
- Installer-friendly (FILE_SHARE_DELETE on Windows)
- Real-time updates with minimal latency (100ms polling)

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
# Test lemonade-router (server) directly
./src/cpp/build/Release/lemonade-router.exe --port 8000 --log-level debug

# Test lemonade-server-beta (client) commands
./src/cpp/build/Release/lemonade-server-beta.exe list
./src/cpp/build/Release/lemonade-server-beta.exe status

# Run Python integration tests
python test/server_llamacpp.py vulkan --server-binary ./src/cpp/build/Release/lemonade-server-beta.exe
python test/server_flm.py --server-binary ./src/cpp/build/Release/lemonade-server-beta.exe
```

See the `.github/workflows/` directory for CI/CD test configurations.

**Note:** The Python tests should now use `lemonade-server-beta.exe` as the entry point since it provides the CLI interface.

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
