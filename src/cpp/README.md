# Lemonade C++ Server

This directory contains the C++ implementation of the Lemonade Server, providing a lightweight, high-performance alternative to the Python implementation.

## Components

- **lemonade-router.exe** - Core HTTP server executable that handles requests and LLM backend orchestration
- **lemonade-server.exe** - Console CLI client for terminal users that manages server lifecycle, executes commands via HTTP API
- **lemonade-tray.exe** (Windows only) - GUI tray launcher for desktop users, automatically starts `lemonade-server.exe serve`
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
sudo apt install build-essential cmake libcurl4-openssl-dev libssl-dev pkg-config
# Note: Tray application is disabled on Linux (headless mode only)
# This avoids LGPL dependencies and provides a cleaner server-only experience
```

**macOS:**
```bash
# Install Xcode command line tools
xcode-select --install
```

### Developer IDE & IDE Build Steps
#### Visual Studio Code Setup Guide
1. Clone the repository into a blank folder locally on your computer.
2. Open the folder in visual studio code.
3. Install Dev Containers extension in visual studio code by using
  control + p to open the command bar at the top of the IDE or if on mac with Cmd + p.
4. Type "> Extensions: Install Extensions" which will open the Extensions side panel.
5. in the extensions search type ```Dev Containers``` and install it.
6. Once completed with the prior steps you may run command
```>Dev Containers: Open Workspace in Container``` or ```>Dev Containers: Open Folder in Container``` which you can do in the command bar in the IDE and it should reopen the visual studio code project.
7. It will launch a docker and start building a new docker and then the project will open in visual studio code.

#### Build & Compile Options

1. Assuming your VSCode IDE is open and the dev container is working.
2. Go to the CMake plugin you may select the "Folder" that is where you currently want to build.
3. Once done with that you may select which building toolkit you are using under Configure and then begin configure.
4. Under Build, Test, Debug and/or Launch you may select whatever configuration you want to build, test, debug and/or launch.

#### Debug / Runtime / Console arguments
1. You may find arguments which are passed through to the application you are debugging in .vscode/settings.json which will look like the following:
```
"cmake.debugConfig": {
        "args": [
            "--llamacpp", "cpu"
        ]
    }
```
2. If you want to debug lemonade-router you may pass --llamacpp cpu for cpu based tests.
3. For lemonade-server you may pass serve as a argument as well.

##### The hard way - commands only.
1. Now if you want to do it the hard way below are the commands in which you can run in the command dropdown in which you can see if you use the following keyboard shortcuts. cmd + p / control + p
```

> Cmake: Select a Kit
# Select a kit or Scan for kit. (Two options should be available gcc or clang)
> Cmake: Configure
# Optional commands are: 
> Cmake: Build Target
# use this to select a cmake target to build
> Cmake: Set Launch/Debug target
# use this to select/set your cmake target you want to build/debug

# This next command lets you debug
> Cmake: Debug

# This command lets you delete the cmake cache and reconfigure which is rarely needed.
> Cmake: Delete Cache and Reconfigure
```

2. Custom configurations for cmake are in the root directory under ```.vscode/settings.json``` in which you may set custom args for launching the debug in the json key ```cmake.debugConfig```


### Build Steps

```bash
# Navigate to the C++ source directory
cd src/cpp

# Create and enter build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build with all cores
cmake --build . --config Release -j

# On Windows, executables will be in: build/Release/
# On Linux/macOS, executables will be in: build/
```

### Build Outputs

- **Windows:** 
  - `build/Release/lemonade-router.exe` - HTTP server
  - `build/Release/lemonade-server.exe` - Console CLI client
  - `build/Release/lemonade-tray.exe` - GUI tray launcher
  - `build/Release/lemonade-log-viewer.exe` - Log file viewer
- **Linux/macOS:** 
  - `build/lemonade-router` - HTTP server
  - `build/lemonade-server` - Console CLI client
- **Resources:** Automatically copied to `build/Release/resources/` (web UI files, model registry, backend version configuration)

### RyzenAI Server Dependency

The `lemonade-router` server has a runtime dependency on `ryzenai-server` for NPU model inference. This dependency can be fulfilled in two ways:

1. **Development builds:** Build `ryzenai-server` from source in the same repository:
   ```bash
   # Build ryzenai-server
   cd src/ryzenai-server
   mkdir build && cd build
   cmake .. -G "Visual Studio 17 2022"
   cmake --build . --config Release
   
   # The executable will be at: src/ryzenai-server/build/bin/Release/ryzenai-server.exe
   ```

2. **Runtime download:** For end users, `lemonade-router` will automatically download the `ryzenai-server` executable from GitHub releases as needed when attempting to run NPU models.

### Platform-Specific Notes

**Windows:**
- The build uses static linking to minimize DLL dependencies
- All dependencies are built from source (no external DLL requirements)
- Security features enabled: Control Flow Guard, ASLR, DEP

**Linux:**
- Linux builds are headless-only (no tray application) by default
- This avoids LGPL dependencies (GTK3, libappindicator3, libnotify)
- Run server using: `lemonade-server serve` (headless mode is automatic)
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

### Windows Installer (WiX/MSI)

**Prerequisites:**
- WiX Toolset 5.0.2 installed from [wix-cli-x64.msi](https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi)
- Completed C++ build (see above)

**Building:**

Using PowerShell script (recommended):
```powershell
cd src\cpp
.\build_installer.ps1
```

Manual build using CMake:
```powershell
cd src\cpp\build
cmake --build . --config Release --target wix_installer
```

**Installer Output:**

Creates `lemonade-server-minimal.msi` which:
- MSI-based installer (Windows Installer technology)
- Installs to `%LOCALAPPDATA%\lemonade_server\`
- Adds `bin\` folder to user PATH using Windows Installer standard methods
- Creates Start Menu shortcuts (launches `lemonade-tray.exe`)
- Optionally creates desktop shortcut and startup entry
- Uses Windows Installer Restart Manager to gracefully close running processes
- Includes all executables (router, server, tray, log-viewer)
- Proper upgrade handling between versions
- Includes uninstaller

**Installation:**

GUI installation:
```powershell
# Double-click lemonade-server-minimal.msi or run:
msiexec /i lemonade-server-minimal.msi
```

Silent installation:
```powershell
# Install silently
msiexec /i lemonade-server-minimal.msi /qn

# Install to custom directory
msiexec /i lemonade-server-minimal.msi /qn INSTALLDIR="C:\Custom\Path"

# Install without desktop shortcut
msiexec /i lemonade-server-minimal.msi /qn ADDDESKTOPSHORTCUT=0

# Install with startup entry
msiexec /i lemonade-server-minimal.msi /qn ADDTOSTARTUP=1
```

### Linux .deb Package (Debian/Ubuntu)

**Prerequisites:**
- Completed C++ build (see above)

**Building:**

```bash
cd src/cpp/build
cpack
```

**Package Output:**

Creates `lemonade-server-minimal_<VERSION>_amd64.deb` (e.g., `lemonade-server-minimal_9.0.3_amd64.deb`) which:
- Installs to `/usr/local/bin/` (executables)
- Installs resources to `/usr/local/share/lemonade-server/`
- Creates desktop entry in `/usr/local/share/applications/`
- Declares dependencies: libcurl4, libssl3, libz1
- Package size: ~2.2 MB (clean, runtime-only package)
- Includes postinst script that creates writable `/usr/local/share/lemonade-server/llama/` directory

**Installation:**

```bash
# Replace <VERSION> with the actual version (e.g., 9.0.0)
sudo dpkg -i lemonade-server-minimal_<VERSION>_amd64.deb

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
lemonade-server --help
lemonade-router --help

# Start server in headless mode:
lemonade-server serve --no-tray

# Or just:
lemonade-server serve
```

## Code Structure

```
src/cpp/
├── CMakeLists.txt              # Main build configuration
├── build_installer.ps1         # Installer build script
├── resources/                  # Configuration and data files
│   └── backend_versions.json   # llama.cpp version configuration (user-editable)
│
├── installer/                  # WiX MSI installer (Windows)
│   ├── Product.wxs.in          # WiX installer definition template
│   ├── installer_banner_wix.bmp  # Left-side banner (493×312)
│   └── top_banner.bmp          # Top banner with lemon icon (493×58)
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
│   ├── backends/               # Model backend implementations
│   │   ├── llamacpp_server.cpp   # Wraps llama.cpp for LLM inference (CPU/GPU)
│   │   ├── fastflowlm_server.cpp # Wraps FastFlowLM for NPU inference
│   │   ├── ryzenaiserver.cpp     # Wraps RyzenAI server for hybrid NPU
│   │   └── whisper_server.cpp    # Wraps whisper.cpp for audio transcription
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
│   │   ├── ryzenaiserver.h
│   │   └── whisper_server.h
│   └── utils/                  # Utility headers
│       ├── http_client.h, json_utils.h
│       ├── process_manager.h, path_utils.h
│
└── tray/                       # System tray application
    ├── CMakeLists.txt          # Tray-specific build config
    ├── main.cpp                # Tray entry point (lemonade-server)
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
- Supports loading multiple models simultaneously with LRU eviction
- Handles all inference requests
- No command-based user interface - only accepts startup options

**Key Layers:**
- **HTTP Layer:** Uses cpp-httplib for HTTP server
- **Router:** Determines which backend handles each request based on model recipe, manages multiple WrappedServer instances with LRU cache
- **Model Manager:** Handles model discovery, downloads, and registry management
- **Backend Wrappers:** Manages llama.cpp, FastFlowLM, RyzenAI, and whisper.cpp backends

**Multi-Model Support:**
- Router maintains multiple WrappedServer instances simultaneously
- Separate LRU caches for LLM, embedding, reranking, and audio model types
- NPU exclusivity: only one model can use NPU at a time
- Configurable limits via `--max-loaded-models` (default: 1 1 1 1)
- Automatic eviction of least-recently-used models when limits reached
- Thread-safe model loading with serialization to prevent races
- Protection against evicting models actively serving inference requests

#### lemonade-server (CLI Client Component)

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
- Simple launcher that starts `lemonade-server.exe serve`
- Zero console output or CLI interface
- Used by Start Menu, Desktop shortcuts, and autostart
- Provides seamless GUI experience for non-technical users

### Client-Server Communication

The `lemonade-server` client communicates with `lemonade-router` server via HTTP:
- **Model operations:** `/api/v1/models`, `/api/v1/pull`, `/api/v1/delete`
- **Model control:** `/api/v1/load`, `/api/v1/unload`
- **Server management:** `/api/v1/health`, `/internal/shutdown`
- **Inference:** `/api/v1/chat/completions`, `/api/v1/completions`, `/api/v1/audio/transcriptions`

The client automatically:
- Detects if a server is already running
- Starts ephemeral servers for one-off commands
- Cleans up ephemeral servers after command completion
- Manages persistent servers with proper lifecycle handling

**Single-Instance Protection:**
- Each component (`lemonade-router`, `lemonade-server serve`, `lemonade-tray`) enforces single-instance using system-wide mutexes
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
#   --host HOST              Bind address (default: localhost)
#   --ctx-size SIZE          Context size (default: 4096)
#   --log-level LEVEL        Log level: critical, error, warning, info, debug, trace
#   --llamacpp BACKEND       LlamaCpp backend: vulkan, rocm, metal
#   --max-loaded-models LLMS [EMBEDDINGS] [RERANKINGS] [AUDIO]
#                            Maximum models to keep loaded (default: 1 1 1 1)
#   --version, -v            Show version
#   --help, -h               Show help
```

### lemonade-server.exe (Console CLI Client)

The `lemonade-server` executable is the command-line interface for terminal users:
- Command-line interface for all model and server management
- Starts persistent servers (with optional tray interface)
- Manages ephemeral servers for one-off commands
- Communicates with `lemonade-router` via HTTP endpoints

```bash
# List available models
./lemonade-server list

# Pull a model
./lemonade-server pull Llama-3.2-1B-Instruct-CPU

# Delete a model
./lemonade-server delete Llama-3.2-1B-Instruct-CPU

# Check server status
./lemonade-server status

# Stop the server
./lemonade-server stop

# Run a model (starts persistent server with tray and opens browser)
./lemonade-server run Llama-3.2-1B-Instruct-CPU

# Start persistent server (with tray on Windows/macOS, headless on Linux)
./lemonade-server serve

# Start persistent server without tray (headless mode, explicit on all platforms)
./lemonade-server serve --no-tray

# Start server with custom options
./lemonade-server serve --port 8080 --ctx-size 8192
```

**Available Options:**
- `--port PORT` - Server port (default: 8000)
- `--host HOST` - Server host (default: localhost)
- `--ctx-size SIZE` - Context size (default: 4096)
- `--log-level LEVEL` - Logging verbosity: info, debug (default: info)
- `--log-file PATH` - Custom log file location
- `--server-binary PATH` - Path to lemonade-router executable
- `--no-tray` - Run without tray (headless mode)
- `--max-loaded-models LLMS [EMBEDDINGS] [RERANKINGS] [AUDIO]` - Maximum number of models to keep loaded simultaneously (default: 1 1 1 1)

**Note:** `lemonade-router` is always launched with `--log-level debug` for optimal troubleshooting. Use `--log-level debug` on `lemonade-server` commands to see client-side debug output.

### lemonade-tray.exe (GUI Tray Launcher - Windows Only)

The `lemonade-tray` executable is a simple GUI launcher for desktop users:
- Double-click from Start Menu or Desktop to start server
- Automatically runs `lemonade-server.exe serve` in tray mode
- Zero console windows or CLI interface
- Perfect for non-technical users
- Single-instance protection: shows friendly message if already running

**What it does:**
1. Finds `lemonade-server.exe` in the same directory
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

When running `lemonade-server.exe serve`:
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
- Python 3.10+
- Test dependencies: `pip install -r test/requirements.txt`

**Running tests:**
```bash
# Test lemonade-router (server) directly
./src/cpp/build/Release/lemonade-router.exe --port 8000 --log-level debug

# Test lemonade-server (client) commands
./src/cpp/build/Release/lemonade-server.exe list
./src/cpp/build/Release/lemonade-server.exe status

# Run Python integration tests
python test/server_llamacpp.py vulkan --server-binary ./src/cpp/build/Release/lemonade-server.exe
python test/server_flm.py --server-binary ./src/cpp/build/Release/lemonade-server.exe
```

See the `.github/workflows/` directory for CI/CD test configurations.

**Note:** The Python tests should now use `lemonade-server.exe` as the entry point since it provides the CLI interface.

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
