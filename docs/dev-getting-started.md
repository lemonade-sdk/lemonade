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
  - [Internal Endpoints](#internal-endpoints)
  - [Dependencies](#dependencies)
- [Usage](#usage)
  - [lemond (Server Only)](#lemond-server-only)
  - [lemonade-server.exe (Console CLI Client)](#lemonade-serverexe-console-cli-client)
  - [lemonade-tray (GUI Tray Application)](#lemonade-tray-gui-tray-application---windows-and-linux)
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
- **lemond** - Core HTTP server that handles requests and LLM backend orchestration
- **lemonade** - CLI client for terminal users (list, pull, delete, run, status, logs, launch, backends, scan)
- **LemonadeServer.exe** (Windows only) - SUBSYSTEM:WINDOWS GUI app that embeds the server and shows a system tray icon
- **lemonade-tray** (macOS/Linux) - Lightweight tray client that connects to a running `lemond`
- **lemonade-server** - Deprecated backwards-compatibility shim (delegates to `lemond` or `lemonade`)

## Building from Source

### Prerequisites

**All Platforms:**
- CMake 3.28 or higher
- C++17 compatible compiler
- Git (for fetching dependencies)
- Internet connection (first build downloads dependencies)

**Windows:**
- Visual Studio 2022 or later (2022 and 2026 are supported via CMake presets)
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

**Windows (Visual Studio 2022)**
```powershell
cmake --build --preset windows
```

**Windows (Visual Studio 2026)**
```powershell
cmake --build --preset vs18
```

### Build Outputs

- **Windows:**
  - `build/Release/lemond.exe` - HTTP server
  - `build/Release/LemonadeServer.exe` - GUI app (embedded server + system tray)
  - `build/Release/lemonade.exe` - CLI client
  - `build/Release/lemonade-server.exe` - Legacy shim (deprecated)
- **Linux/macOS:**
  - `build/lemond` - HTTP server
  - `build/lemonade` - CLI client
  - `build/lemonade-tray` - System tray client (macOS always; Linux when AppIndicator3 found)
  - `build/lemonade-server` - Legacy shim (deprecated)
- **Resources:** Automatically copied to `build/Release/resources/` on Windows, `build/resources/` on Linux/macOS (web UI files, model registry, backend version configuration)

### Building the Electron Desktop App (Optional)

The tray menu's "Open app" option and the `lemonade run` command can launch the Electron desktop app. To include it in your build:

Build the Electron app using CMake (requires Node.js 20+):

**Linux**
```bash
cmake --build --preset default --target electron-app
```

**Windows (Visual Studio 2022)**
```powershell
cmake --build --preset windows --target electron-app
```

**Windows (Visual Studio 2026)**
```powershell
cmake --build --preset vs18 --target electron-app
```

This will:
1. Copy src/app to build/app-src (keeps source tree clean)
2. Run npm install in build/app-src
3. Build to build/app/linux-unpacked/ (Linux) or build/app/win-unpacked/ (Windows)

The tray app searches for the Electron app in these locations:
- **Windows installed**: `../app/lemonade-app.exe` (relative to bin/ directory)
- **Windows development**: `../app/win-unpacked/lemonade-app.exe` (from build/Release/)
- **Linux installed**: `/opt/share/lemonade-server/app/lemonade-app`
- **Linux development**: `../app/linux-unpacked/lemonade-app` (from build/)

If not found, the "Open app" menu option is hidden but everything else works.

### Building an AppImage (Linux Only)

AppImage builds are opt-in. Enable the option at configure time and then build:

```bash
cmake --preset default -DBUILD_APPIMAGE=ON
cmake --build --preset default --target appimage
```

Alternatively, if you've already configured without the flag, you can still trigger a one-off build using the manual target:

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
- `build/app-appimage/lemonade-app-<version>-<arch>.AppImage`

The AppImage is a self-contained executable that includes all dependencies and can be run on any Linux distribution without installation. Simply make it executable and run it:

```bash
chmod +x build/app-appimage/lemonade-app-*.AppImage
./build/app-appimage/lemonade-app-*.AppImage
```

### Platform-Specific Notes

**Windows:**
- The build uses static linking to minimize DLL dependencies
- All dependencies are built from source (no external DLL requirements)
- Security features enabled: Control Flow Guard, ASLR, DEP

**Linux:**
- `lemond` is always headless on Linux (GTK-free, daemon-friendly); use `lemond` to start the server directly
- `lemonade-tray` is a separate binary for the system tray, auto-detected at build time: built if AppIndicator3 libraries are found (GTK3 only needed for non-glib variants)
- To require tray support (fail if deps missing): `-DREQUIRE_LINUX_TRAY=ON`
- Optional tray dependencies: one of `ayatana-appindicator-glib-devel` (preferred, no GTK3 needed), `ayatana-appindicator3-devel`, or `libappindicator-gtk3-devel` (the latter two also require `gtk3-devel`)
- Fully functional for server operations and model management
- Uses permissively licensed dependencies only (MIT, Apache 2.0, BSD, curl license)
- Clean .deb package with only runtime files (no development headers)
- Proper graceful shutdown - all child processes cleaned up correctly
- File locations:
  - Installed binaries: `/opt/bin`
  - Downloaded backends (llama-server, ryzenai-server): `~/.cache/lemonade/bin/`
  - Model downloads: `~/.cache/huggingface/` (follows HF conventions)
  - Runtime files (lock, log): `$XDG_RUNTIME_DIR/lemonade/` when set and writable, otherwise `/tmp/`

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

```powershell
cmake --build build --config Release --target wix_installers
```

**Installer Output:**

Creates `lemonade-server-minimal.msi` which:
- MSI-based installer (Windows Installer technology)
- **Per-user install (default):** Installs to `%LOCALAPPDATA%\lemonade_server\`, adds to user PATH, no UAC required
- **All-users install (CLI only):** Installs to `%PROGRAMFILES%\Lemonade Server\`, adds to system PATH, requires elevation
- Creates Start Menu shortcuts (launches `lemonade-tray.exe`)
- Optionally creates desktop shortcut and startup entry
- Uses Windows Installer Restart Manager to gracefully close running processes
- Includes all core executables (router, server, tray, CLI, and optional desktop app)
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
- Declares dependencies: `libcurl4`, `libssl3`, `libz1`, `unzip`, `fonts-katex`
- Recommends: `ffmpeg` for whisper.cpp audio resampling and/or transcoding, plus a Chromium-compatible browser for `lemonade-web-app`
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
lemonade --help
lemond --help
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

- **`lemond`**: The main HTTP server executable that handles LLM inference requests
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
2. If you want to debug lemond you may pass --llamacpp cpu for cpu based tests.
3. For `lemonade` you may pass a subcommand (e.g., `run MODEL`) as arguments.

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
|       |── network_beacon.h    # Helps broadcast a beacon on port 13305 to network multicast
│
└── tray/                       # System tray application
    ├── CMakeLists.txt          # Tray-specific build config
    ├── main.cpp                # Entry point (WinMain on Windows, main on macOS/Linux)
    ├── tray_ui.h               # TrayUI class header
    ├── tray_ui.cpp             # TrayUI class — menu, HTTP, icon, app launch (~500 lines)
    ├── agent_launcher.cpp      # Agent (claude/codex) launcher (shared with CLI)
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

#### lemond (Server Component)

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

#### lemonade (CLI Client)

A console application for terminal users:
- Provides command-based user interface (`list`, `pull`, `delete`, `run`, `status`, `logs`, `launch`, `backends`, `scan`)
- Communicates with `lemond` via HTTP endpoints
- Expects the server to already be running (auto-started by the OS after installation)

#### lemonade-tray / LemonadeServer.exe (GUI Tray Application)

A GUI application for desktop users that exposes the server via a system tray icon:
- **Windows:** `LemonadeServer.exe` — a SUBSYSTEM:WINDOWS app that embeds the server and shows a system tray icon. No console window.
- **Linux:** `lemonade-tray` — tray application (requires GTK3 + AppIndicator3). Connects to an already-running server if one is found; otherwise starts one (via systemd if a unit is installed, or by spawning `lemond` directly).
- Zero console output or CLI interface
- Used by application launchers, desktop shortcuts, and autostart entries
- Provides seamless GUI experience for non-technical users

### Client-Server Communication

The `lemonade` client communicates with `lemond` server via HTTP:
- **Model operations:** `/api/v1/models`, `/api/v1/pull`, `/api/v1/delete`
- **Model control:** `/api/v1/load`, `/api/v1/unload`
- **Server management:** `/api/v1/health`, `/internal/shutdown`, `/internal/set`, `/internal/config`
- **Inference:** `/api/v1/chat/completions`, `/api/v1/completions`, `/api/v1/audio/transcriptions`

The client automatically:
- Discovers the running server's port
- Reports an error if no server is reachable

**Single-Instance Protection:**
- **Windows:** `LemonadeServer.exe` holds a system-wide mutex (`Global\LemondMutex`). A second launch shows a "Server is already running" dialog and exits.
- **Linux/macOS:** `lemonade-tray` acquires an exclusive `flock()` on a lock file in the runtime directory to prevent duplicate tray instances.

**Server Discovery:**
- The `lemonade` CLI auto-discovers the running server via UDP beacon broadcast, falling back to the default port if no beacon is found.

**Network Beacon based broadcasting:**
- Uses port 13305 to broadcast to the network that it exists
- Clients can read the json broadcast message to add server to server picker.
- Uses machine hostname as broadcast name.
- The custom flag --no-broadcast is available in the command line to disable.
- Auto protection, doesnt broadcast on non RFC1918 Networks.

### Internal Endpoints

> **These endpoints are for first-party Lemonade software only** (CLI, tray app, desktop app). They are not part of the public API, may change without notice, and must not be relied upon by third-party integrations.

Internal endpoints are restricted to loopback (`127.0.0.1` / `::1`) — requests from non-localhost addresses receive `403 Forbidden`.

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/internal/shutdown` | Unloads all models and shuts down the server |
| `POST` | `/internal/set` | Unified config setter (see below) |
| `GET`  | `/internal/config` | Returns the full runtime config snapshot |

#### `POST /internal/set`

Accepts a JSON object with one or more keys to update atomically. Returns `{"status":"success","updated":{...}}` on success, or `400` with an error message on validation failure.

**Server-level keys** (trigger immediate side effects):

| Key | Type | Side Effect |
|-----|------|-------------|
| `port` | int (1–65535) | HTTP rebind |
| `host` | string | HTTP rebind |
| `log_level` | string (`trace`, `debug`, `info`, `warning`, `error`, `fatal`, `none`) | Reconfigures log filter |
| `global_timeout` | int (positive) | Updates default HTTP client timeout |
| `no_broadcast` | bool | Stops or starts UDP beacon |
| `extra_models_dir` | string | Updates model manager search path |

**Deferred keys** (affect the next model load or eviction decision, no immediate side effect):

| Key | Type |
|-----|------|
| `max_loaded_models` | int (-1 or positive) |
| `ctx_size` | int (positive) |
| `llamacpp_backend` | string |
| `llamacpp_args` | string |
| `sdcpp_backend` | string |
| `whispercpp_backend` | string |
| `whispercpp_args` | string |
| `steps` | int (positive) |
| `cfg_scale` | number |
| `width` | int (positive) |
| `height` | int (positive) |
| `flm_args` | string |

**Example:**
```bash
curl -X POST http://localhost:13305/internal/set \
  -H "Content-Type: application/json" \
  -d '{"ctx_size": 8192, "max_loaded_models": 3, "log_level": "debug"}'
```

#### `GET /internal/config`

Returns the full runtime configuration as a flat JSON object containing all server-level and recipe option keys with their current values.

**Example:**
```bash
curl http://localhost:13305/internal/config
```

### Dependencies

All dependencies are automatically fetched by CMake via FetchContent:

- **cpp-httplib** (v0.26.0) - HTTP server with thread pool support [MIT License]
- **nlohmann/json** (v3.11.3) - JSON parsing and serialization [MIT License]
- **CLI11** (v2.4.2) - Command-line argument parsing [BSD 3-Clause]
- **libcurl** (8.5.0) - HTTP client for model downloads [curl license]
- **zstd** (v1.5.7) - Compression library for HTTP [BSD License]

Platform-specific SSL backends are used (Schannel on Windows, SecureTransport on macOS, OpenSSL on Linux).

## Usage

### lemond (Server Only)

The `lemond` executable is a pure HTTP server without any command-based interface:

```bash
# Start server with default options
./lemond

# Start server with custom port
./lemond --port 8080

# Available options:
#   [cache_dir]              Path to lemonade cache directory (optional)
#   --port PORT              Port number (default: 13305)
#   --host HOST              Bind address (default: localhost)
#   --version, -v            Show version
#   --help, -h               Show help
```

All other server settings are managed via `lemonade config set` (see [Server Configuration](./server/configuration.md)).

### lemonade (CLI Client)

The `lemonade` executable is the command-line interface for terminal users:
- Command-line interface for model management and server interaction
- Communicates with `lemond` via HTTP endpoints
- Expects the server to already be running (auto-started by the OS after installation)

```bash
# List available models
./lemonade list

# Pull a model
./lemonade pull Llama-3.2-1B-Instruct-CPU

# Delete a model
./lemonade delete Llama-3.2-1B-Instruct-CPU

# Check server status
./lemonade status

# Run a model (loads model and opens browser)
./lemonade run Llama-3.2-1B-Instruct-CPU

# View server logs
./lemonade logs

# List recipes and backends
./lemonade backends
```

### LemonadeServer.exe / lemonade-tray (GUI Tray Application)

The tray application provides a system tray icon for desktop users:
- Double-click from Start Menu, application launcher, or Desktop to start server
- Zero console windows or CLI interface — always starts the tray directly
- Perfect for non-technical users
- Single-instance protection: shows friendly message if already running

**Platform support:**
- **Windows:** `LemonadeServer.exe` — a SUBSYSTEM:WINDOWS app that embeds `lemond` and shows a system tray icon. No separate console process. Auto-starts via the Windows startup folder.
- **Linux:** `lemonade-tray` — available when compiled with GTK3 + AppIndicator3 support (auto-detected at build time). Connects to an already-running server if one is found; otherwise starts one (via systemd if a unit is installed, or by spawning `lemond` directly).

**What it does (Linux):**
1. Starts immediately in tray mode (no subcommand needed)
2. Connects to an already-running server, or starts one (via systemd if a unit is installed, otherwise spawns `lemond` directly)
3. Shows a system tray icon connected to the server

**When to use:**
- Launching from Start Menu (Windows) or application launcher (Linux)
- Desktop shortcuts
- Windows startup / Linux autostart
- Any GUI/point-and-click scenario

**System Tray Features (when running):**
- Left-click or right-click icon to show menu
- Load/unload models via menu
- Change server port and context size
- Open web UI, documentation, and logs
- "Show Logs" opens the desktop app's logs view with historical and live logs
- Background model monitoring
- Click balloon notifications to open menu
- Quit option

**UI Improvements:**
- Displays as "Lemonade Local LLM Server" in Task Manager
- Shows large lemon icon in notification balloons
- Single-instance protection prevents multiple tray apps

### Logging and Console Output

When running `LemonadeServer.exe` or `lemond`:
- **Log File:** Direct runs write logs to a persistent log file (default: `%TEMP%\lemonade-server.log` on Windows). When `lemond` runs as the systemd service, logs go to the journal instead.
- **Logs UI:** Click "Show Logs" in the tray or use `lemonade logs` to open the desktop app's logs view
  - Connects to the server's WebSocket log stream
  - Shows retained recent log history plus live entries
  - Reconnects automatically if the stream drops

**Logs UI Features:**
- Real-time streaming over `/logs/stream`
- Snapshot + live log entries
- Integrated into the desktop app instead of a standalone log viewer binary

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
