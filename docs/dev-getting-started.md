# Lemonade Development

This guide covers everything you need to build, test, and contribute to Lemonade from source. Whether you're fixing a bug, adding a feature, or just exploring the codebase, this document will help you get started.

## Table of Contents

- [Components](#components)
- [Building from Source](#building-from-source)
  - [Prerequisites](#prerequisites)
  - [Build Steps](#build-steps)
  - [Build Outputs](#build-outputs)
  - [Building the Electron Desktop App (Optional)](#building-the-electron-desktop-app-optional)
  - [Platform-Specific Notes](#platform-specific-notes)
- [Building Installers](#building-installers)
  - [Windows Installer (WiX/MSI)](#windows-installer-wixmsi)
  - [Linux .deb Package (Debian/Ubuntu)](#linux-deb-package-debianubuntu)
  - [Linux .rpm Package (Fedora, RHEL etc)](#linux-rpm-package-fedora-rhel-etc)
  - [Developer IDE & IDE Build Steps](#developer-ide--ide-build-steps)
- [Code Structure](#code-structure)
- [Architecture Overview](#architecture-overview)
  - [Overview](#overview)
  - [Client-Server Communication](#client-server-communication)
  - [Dependencies](#dependencies)
- [Usage](#usage)
  - [lemonade-router (Server Only)](#lemonade-router-server-only)
  - [lemonade-server.exe (Console CLI Client)](#lemonade-serverexe-console-cli-client)
  - [lemonade-tray.exe (GUI Tray Launcher)](#lemonade-trayexe-gui-tray-launcher---windows-only)
  - [Logging and Console Output](#logging-and-console-output)
- [Testing](#testing)
  - [Basic Functionality Tests](#basic-functionality-tests)
  - [Integration Tests](#integration-tests)
- [Development](#development)
  - [Code Style](#code-style)
  - [Key Resources](#key-resources)
- [License](#license)

## Components

Lemonade consists of these main executables:
- **lemonade-router.exe** - Core HTTP server executable that handles requests and LLM backend orchestration
- **lemonade-server.exe** - Console CLI client for terminal users that manages server lifecycle, executes commands via HTTP API
- **lemonade-tray.exe** (Windows only) - GUI tray launcher for desktop users, automatically starts `lemonade-server.exe serve`
- **lemonade-log-viewer.exe** (Windows only) - Log file viewer with live tail support and installer-friendly file sharing

## Building from Source

### Prerequisites

**All Platforms:**
- CMake 3.28 or higher
- C++17 compatible compiler
- Git (for fetching dependencies)
- Internet connection (first build downloads dependencies)

**Windows:**
- Visual Studio 2019 or later
- WiX 5.x (only required for building the installer)

**Linux:**
 - Ninja build system (optional, recommended)

### Build Steps
A helper script is available that will set up the build environment on popular
Linux distributions and macOS.  This will prompt to install dependencies via native
package managers and create the build directory.

**Linux / macOS**
```bash
./setup.sh
```

**Windows**
```shell
./setup.ps1
```

Build by running:

**Linux / macOS**
```bash
cmake --build --preset default
```

**Windows**
```powershell
cmake --build --preset windows
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
- **Resources:** Automatically copied to `build/Release/resources/` on Windows, `build/resources/` on Linux/macOS (web UI files, model registry, backend version configuration)

### Building the Electron Desktop App (Optional)

The tray menu's "Open app" option and the `lemonade-server run` command can launch the Electron desktop app. To include it in your build:

Build the Electron app using CMake (requires Node.js 20+):

**Linux**
```bash
cmake --build --preset default --target electron-app
```

**Windows**
```powershell
cmake --build --preset windows --target electron-app
```

This will:
1. Copy src/app to build/app-src (keeps source tree clean)
2. Run npm install in build/app-src
3. Build to build/app/linux-unpacked/ (Linux) or build/app/win-unpacked/ (Windows)

The tray app searches for the Electron app in these locations:
- **Windows installed**: `../app/Lemonade.exe` (relative to bin/ directory)
- **Windows development**: `../app/win-unpacked/Lemonade.exe` (from build/Release/)
- **Linux installed**: `/usr/local/share/lemonade-server/app/lemonade`
- **Linux development**: `../app/linux-unpacked/lemonade` (from build/)

If not found, the "Open app" menu option is hidden but everything else works.

### Building an AppImage (Linux Only)

To create a standalone AppImage package that can run on any Linux distribution:

```bash
cmake --build --preset default --target appimage
```

This will:
1. Copy the Electron app source to a separate build directory
2. Set the package.json version to match the CMake project version
3. Install npm dependencies
4. Build the renderer with production optimizations
5. Package the application as an AppImage using electron-builder

The generated AppImage will be located in:
- `build/app-appimage/Lemonade-<version>-<arch>.AppImage`

The AppImage is a self-contained executable that includes all dependencies and can be run on any Linux distribution without installation. Simply make it executable and run it:

```bash
chmod +x build/app-appimage/Lemonade-*.AppImage
./build/app-appimage/Lemonade-*.AppImage
```

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
  - Installed binaries: `/opt/bin`
  - Downloaded backends (llama-server, ryzenai-server): `~/.cache/lemonade/bin/`
  - Model downloads: `~/.cache/huggingface/` (follows HF conventions)

**macOS (beta):**
- Uses native system frameworks (Cocoa, Foundation)
- ARM Macs use Metal backend by default for llama.cpp
- macOS support is currently in beta; a signed and notarized `.pkg` installer is available from the [releases page](https://github.com/lemonade-sdk/lemonade/releases/latest)

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
- **Per-user install (default):** Installs to `%LOCALAPPDATA%\lemonade_server\`, adds to user PATH, no UAC required
- **All-users install (CLI only):** Installs to `%PROGRAMFILES%\Lemonade Server\`, adds to system PATH, requires elevation
- Creates Start Menu shortcuts (launches `lemonade-tray.exe`)
- Optionally creates desktop shortcut and startup entry
- Uses Windows Installer Restart Manager to gracefully close running processes
- Includes all executables (router, server, tray, log-viewer)
- Proper upgrade handling between versions
- Includes uninstaller

**Available Installers:**
- `lemonade-server-minimal.msi` - Server only (~3 MB)
- `lemonade.msi` - Full installer with Electron desktop app (~105 MB)

**Installation:**

For detailed installation instructions including silent install, custom directories, and all-users installation, see the [Server Integration Guide](../../docs/server/server_integration.md#windows-installation).

### Linux .deb Package (Debian/Ubuntu)

**Prerequisites:**
- Completed C++ build (see above)

**Building:**

```bash
cd build
cpack
```

**Package Output:**

Creates `lemonade-server_<VERSION>_amd64.deb` (e.g., `lemonade-server_9.0.3_amd64.deb`) which:
- Installs to `/opt/bin/` (executables)
- Installs resources to `/opt/share/lemonade-server/`
- Creates desktop entry in `/opt/share/applications/`
- Declares dependencies: libcurl4, libssl3, libz1
- Package size: ~2.2 MB (clean, runtime-only package)
- Includes postinst script that creates writable `/opt/share/lemonade-server/llama/` directory

**Installation:**

```bash
# Replace <VERSION> with the actual version (e.g., 9.0.0)
sudo apt install ./lemonade-server_<VERSION>_amd64.deb
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

### Linux .rpm Package (Fedora, RHEL etc)

Very similar to the Debian instructions above with minor changes

**Building:**

```bash
cd build
cpack -G RPM
```

**Package Output:**

Creates `lemonade-server-<VERSION>.x86_64.rpm` (e.g., `lemonade-server-9.1.2.x86_64.rpm`) and
resources are installed as per DEB version above

**Installation:**

```bash
# Replace <VERSION> with the actual version (e.g., 9.0.0)
sudo dnf install ./lemonade-server-<VERSION>.x86_64.rpm
```

**Uninstallation:**

```bash
sudo dnf remove lemonade-server
```

**Post-Installation:**

Same as .deb above

**macOS:**

### Building from Source on MacOS for M-Series / arm64 Family

#### Macos Notary Tool Command
For access with P
```
xcrun notarytool store-credentials AC_PASSWORD --apple-id "your-apple-id@example.com" --team-id "your-team-id" --private-key "/path/to/AuthKey_XXXXXX.p8"
```
or
For access with API password
```
xcrun notarytool store-credentials AC_PASSWORD --apple-id "your-apple-id@example.com" --team-id "your-team-id" --password ""
```
Get your team id at:
https://developer.apple.com/account

#### Cmake build instructions

```bash
# Install Xcode command line tools
xcode-select --install

# Navigate to the C++ source directory
cd src/cpp

# Create and enter build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build with all cores
cmake --build . --config Release -j
```

### CMake Targets

The build system provides several CMake targets for different build configurations:

- **`lemonade-router`**: The main HTTP server executable that handles LLM inference requests
- **`package-macos`**: Creates a signed macOS installer package (.pkg) using productbuild
- **`notarize_package`**: Builds and submits the package to Apple for notarization and staples the ticket
- **`electron-app`**: Builds the Electron-based GUI application
- **`prepare_electron_app`**: Prepares the Electron app for inclusion in the installer

### Building and Notarizing for Distribution

To build a notarized macOS installer for distribution:

1. **Prerequisites**:
   - Apple Developer Program membership
   - Valid Developer ID Application and Installer certificates
   - App-specific password for notarization
   - Xcode command line tools

2. **Set Environment Variables**:
   ```bash
   export DEVELOPER_ID_APPLICATION_IDENTITY="Developer ID Application: Your Name (TEAMID)"
   export DEVELOPER_ID_INSTALLER_IDENTITY="Developer ID Installer: Your Name (TEAMID)"
   export AC_PASSWORD="your-app-specific-password"
   ```

3. **Configure Notarization Keychain Profile**:
   ```bash
   xcrun notarytool store-credentials "AC_PASSWORD" \
     --apple-id "your-apple-id@example.com" \
     --team-id "YOURTEAMID" \
     --password "your-app-specific-password"
   ```

4. **Build and Notarize**:
   ```bash
   cd src/cpp/build
   cmake --build . --config Release --target package-macos
   cmake --build . --config Release --target notarize_package
   ```

The notarization process will:
- Submit the package to Apple's notarization service
- Wait for approval
- Staple the notarization ticket to the package

**Note**: The package is signed with hardened runtime entitlements during the build process for security.

### Developer IDE & IDE Build Steps

#### Visual Studio Code Setup Guide
1. Clone the repository into a blank folder locally on your computer.
2. Open the folder in visual studio code.
3. Install Dev Containers extension in Visual Studio Code by using
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

> **Note**
>
>  For running Lemonade as a containerized application (as an alternative to the MSI-based distribution), see `DOCKER_GUIDE.md`.

## Code Structure

```
src/cpp/
├── build_installer.ps1         # Installer build script
├── CopyElectronApp.cmake       # CMake module to copy Electron app to build output
├── CPackRPM.cmake              # RPM packaging configuration
├── DOCKER_GUIDE.md             # Docker containerization guide
├── Extra-Models-Dir-Spec.md    # Extra models directory specification
├── Multi-Model-Spec.md         # Multi-model loading specification
├── postinst                    # Debian package post-install script
├── postinst-full               # Debian package post-install script (full version)
├── resources/                  # Configuration and data files (self-contained)
│   ├── backend_versions.json   # llama.cpp/whisper version configuration
│   ├── server_models.json      # Model registry (available models)
│   └── static/                 # Web UI assets
│       ├── index.html          # Server landing page (with template variables)
│       └── favicon.ico         # Site icon
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
│   ├── recipe_options.cpp      # Recipe option handling
│   ├── wrapped_server.cpp      # Base class for backend wrappers
│   ├── streaming_proxy.cpp     # Server-Sent Events for streaming
│   ├── system_info.cpp         # NPU/GPU device detection
│   ├── lemonade.manifest.in    # Windows manifest template
│   ├── version.rc              # Windows version resource
│   │
│   ├── backends/               # Model backend implementations
│   │   ├── backend_utils.cpp     # Shared backend utilities
│   │   ├── llamacpp_server.cpp   # Wraps llama.cpp for LLM inference (CPU/GPU)
│   │   ├── fastflowlm_server.cpp # Wraps FastFlowLM for NPU inference
│   │   ├── ryzenaiserver.cpp     # Wraps RyzenAI server for hybrid NPU
│   │   ├── sd_server.cpp         # Wraps Stable Diffusion for image generation
│   │   └── whisper_server.cpp    # Wraps whisper.cpp for audio transcription (CPU/NPU)
│   │
│   └── utils/                  # Utility functions
│       ├── http_client.cpp     # HTTP client using libcurl
│       ├── json_utils.cpp      # JSON file I/O
│       ├── process_manager.cpp # Cross-platform process management
│       ├── path_utils.cpp      # Path manipulation
│       ├── wmi_helper.cpp      # Windows WMI for NPU detection
│       └── wmi_helper.h        # WMI helper header
│
├── include/lemon/              # Public headers
│   ├── server.h                # HTTP server interface
│   ├── router.h                # Request routing
│   ├── model_manager.h         # Model management
│   ├── cli_parser.h            # CLI argument parsing
│   ├── recipe_options.h        # Recipe option definitions
│   ├── wrapped_server.h        # Backend wrapper base class
│   ├── streaming_proxy.h       # Streaming proxy
│   ├── system_info.h           # System information
│   ├── model_types.h           # Model type definitions
│   ├── audio_types.h           # Audio type definitions
│   ├── error_types.h           # Error type definitions
│   ├── server_capabilities.h   # Server capability definitions
│   ├── single_instance.h       # Single instance enforcement
│   ├── version.h.in            # Version header template
│   ├── backends/               # Backend headers
│   │   ├── backend_utils.h       # Backend utilities
│   │   ├── llamacpp_server.h     # LlamaCpp backend
│   │   ├── fastflowlm_server.h   # FastFlowLM backend
│   │   ├── ryzenaiserver.h       # RyzenAI backend
│   │   ├── sd_server.h           # Stable Diffusion backend
│   │   └── whisper_server.h      # Whisper backend
│   └── utils/                  # Utility headers
│       ├── http_client.h       # HTTP client
│       ├── json_utils.h        # JSON utilities
│       ├── process_manager.h   # Process management
│       |── path_utils.h        # Path utilities
|       |── network_beacon.h    # Helps broadcast a beacon on port 8000 to network multicast
│
└── tray/                       # System tray application
    ├── CMakeLists.txt          # Tray-specific build config
    ├── main.cpp                # Tray entry point (lemonade-server)
    ├── tray_launcher.cpp       # GUI launcher (lemonade-tray)
    ├── log-viewer.cpp          # Log file viewer (lemonade-log-viewer)
    ├── server_manager.cpp      # Manages lemonade-router process
    ├── tray_app.cpp            # Main tray application logic
    ├── lemonade-server.manifest.in  # Windows manifest template
    ├── version.rc              # Windows version resource
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
- Configurable limits via `--max-loaded-models N` (default: 1)
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

**Network Beacon based broadcasting:**
- Uses port 8000 to broadcast to the network that it exists
- Clients can read the json broadcast message to add server to server picker.
- Uses machine hostname as broadcast name.
- The custom flag --no-broadcast is available in the command line to disable.
- Auto protection, doesnt broadcast on non RFC1918 Networks.

### Dependencies

All dependencies are automatically fetched by CMake via FetchContent:

- **cpp-httplib** (v0.26.0) - HTTP server with thread pool support [MIT License]
- **nlohmann/json** (v3.11.3) - JSON parsing and serialization [MIT License]
- **CLI11** (v2.4.2) - Command-line argument parsing [BSD 3-Clause]
- **libcurl** (8.5.0) - HTTP client for model downloads [curl license]
- **zstd** (v1.5.7) - Compression library for HTTP [BSD License]

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
#   --max-loaded-models N    Maximum models per type slot (default: 1)
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
- `--max-loaded-models N` - Maximum number of models to keep loaded per type slot (default: 1)

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

**Python integration tests** (from `test/` directory, ordered least to most complex):

| Test File | Description |
|-----------|-------------|
| `server_cli.py` | CLI commands (version, list, pull, status, delete, serve, stop, run) |
| `server_endpoints.py` | HTTP endpoints (health, models, pull, load, unload, system-info, stats) |
| `server_llm.py` | LLM inference (chat completions, embeddings, reranking) |
| `server_whisper.py` | Audio transcription (whisper models) |
| `server_sd.py` | Image generation (Stable Diffusion, ~2-3 min per image on CPU) |

**Running tests:**
```bash
# CLI tests (no inference backend needed)
python test/server_cli.py

# Endpoint tests (no inference backend needed)
python test/server_endpoints.py

# LLM tests (specify wrapped server and backend)
python test/server_llm.py --wrapped-server llamacpp --backend vulkan

# Audio transcription tests
python test/server_whisper.py

# Image generation tests (slow)
python test/server_sd.py
```

The tests auto-discover the server binary from the build directory. Use `--server-binary` to override if needed.

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
- **Model Registry:** `src/cpp/resources/server_models.json`
- **Web UI Files:** `src/cpp/resources/static/`
- **Backend Versions:** `src/cpp/resources/backend_versions.json`

## License

This project is licensed under the Apache 2.0 License. All dependencies use permissive licenses (MIT, BSD, Apache 2.0, curl license).
