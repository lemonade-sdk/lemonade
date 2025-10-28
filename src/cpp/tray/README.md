# Lemonade Server Tray Application

This directory contains the cross-platform system tray application for the Lemonade C++ server.

## Overview

The tray application (`lemonade-server-beta`) is a separate executable that:
- Launches and manages the `lemonade` server process
- Provides a system tray icon for easy access
- Offers a context menu for server control and configuration
- Supports headless mode with `--no-tray` flag

## Architecture

```
lemonade-server-beta (Tray App)
    ↓ spawns & manages
lemonade (Server)
    ↑ HTTP API
lemonade-server-beta queries
```

This is a cleaner separation than the Python implementation where the server would launch the tray.

## Building

### Windows

Requirements:
- Visual Studio 2017 or later (includes Windows SDK)
- CMake 3.20 or later

```powershell
cd src/cpp/tray
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

Output: `build/Release/lemonade-server-beta.exe`

**Dependencies:** None! Uses standard Win32 APIs.

### macOS

Requirements:
- Xcode Command Line Tools
- CMake 3.20 or later

```bash
cd src/cpp/tray
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
```

Output: `build/lemonade-server-beta`

**Dependencies:** Cocoa and Foundation (system frameworks)

**Status:** Stub implementation - needs Objective-C++ code

### Linux (Ubuntu)

Requirements:
- CMake 3.20 or later
- GTK+ 3 development files
- libappindicator3 development files
- libnotify development files

```bash
# Install dependencies
sudo apt-get install \
    build-essential \
    cmake \
    libgtk-3-dev \
    libappindicator3-dev \
    libnotify-dev \
    pkg-config

# Build
cd src/cpp/tray
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Output: `build/lemonade-server-beta`

**Status:** Stub implementation - needs GTK/AppIndicator code

## Usage

### With Tray (Default)

```bash
# Windows
lemonade-server-beta.exe

# Linux/macOS
./lemonade-server-beta
```

This will:
1. Start the tray icon
2. Launch the `lemonade` server in the background
3. Show a notification when ready
4. Provide a context menu to control the server

### Headless Mode (No Tray)

```bash
lemonade-server-beta --no-tray
```

This will:
1. Start the `lemonade` server in foreground
2. Show server output in console
3. Exit when server exits or Ctrl+C

### Options

```
lemonade-server-beta [options]

Options:
  --port PORT              Server port (default: 8000)
  --ctx-size SIZE          Context size (default: 4096)
  --log-file PATH          Log file path
  --log-level LEVEL        Log level: debug, info, warning, error
  --server-binary PATH     Path to lemonade server binary
  --no-tray                Start server without tray (headless mode)
  --help, -h               Show help message
  --version, -v            Show version
```

## Features

### Implemented (Windows)

✅ System tray icon  
✅ Context menu with submenus  
✅ Menu item callbacks  
✅ Checkable menu items  
✅ Balloon notifications  
✅ Server process management  
✅ Port and context size configuration  
✅ Open URLs (documentation, chat, model manager)  
✅ Log viewer integration  
✅ Quit functionality  

### TODO

- [ ] HTTP client integration (for model list, health checks)
- [ ] Background model monitoring thread
- [ ] Background version checking thread
- [ ] Upgrade functionality
- [ ] macOS Objective-C++ implementation
- [ ] Linux GTK/AppIndicator implementation
- [ ] Signal handling for Ctrl+C in `--no-tray` mode
- [ ] Configuration persistence
- [ ] Better error handling and recovery

## Development Status

| Platform | Status | Notes |
|----------|--------|-------|
| Windows  | ✅ Functional | Full implementation with Win32 API |
| macOS    | 🚧 Stub | Needs NSStatusBar implementation |
| Linux    | 🚧 Stub | Needs GTK/AppIndicator implementation |

## Code Structure

```
tray/
├── include/lemon_tray/
│   ├── platform/
│   │   ├── tray_interface.h    # Abstract interface
│   │   ├── windows_tray.h      # Windows implementation
│   │   ├── macos_tray.h        # macOS stub
│   │   └── linux_tray.h        # Linux stub
│   ├── server_manager.h        # Server process management
│   └── tray_app.h              # Main application class
├── src/
│   ├── platform/
│   │   ├── windows_tray.cpp    # Windows implementation
│   │   ├── macos_tray.mm       # macOS stub (Obj-C++)
│   │   ├── linux_tray.cpp      # Linux stub
│   │   └── tray_factory.cpp    # Platform selection
│   ├── server_manager.cpp      # Process management
│   ├── tray_app.cpp            # Main app logic
│   └── main.cpp                # Entry point
├── CMakeLists.txt
└── README.md
```

## Testing

After building, test the application:

```bash
# Make sure lemonade server binary is available
cd build

# Test with tray (Windows example)
./Release/lemonade-server-beta.exe --server-binary ../../lemonade-router.exe

# Test without tray
./Release/lemonade-server-beta.exe --no-tray --server-binary ../../lemonade-router.exe
```

Look for:
- Tray icon appears in system tray
- Right-click shows context menu
- Server starts successfully
- Menu items are functional
- Notifications appear

## Integration with Installer

The installer should:
1. Copy both `lemonade-router.exe` and `lemonade-server-beta.exe` to installation directory
2. Create Start Menu shortcut for `lemonade-server-beta.exe`
3. Optionally create Desktop shortcut
4. Add installation directory to PATH
5. Optionally add to startup

See `TRAY_IMPLEMENTATION_PLAN.md` for details.

## Next Steps

1. **Complete HTTP Client Integration**
   - Integrate cpp-httplib for API communication
   - Implement JSON parsing for model lists
   - Add proper error handling

2. **Implement Background Services**
   - Model monitoring thread
   - Version checking thread
   - Auto-update mechanism

3. **Complete macOS Implementation**
   - Implement NSStatusBar integration
   - Add NSMenu handling
   - Test on macOS Monterey+

4. **Complete Linux Implementation**
   - Implement AppIndicator integration
   - Add GTK menu handling
   - Test on Ubuntu 20.04+

5. **Polish and Testing**
   - Add comprehensive error handling
   - Implement signal handling
   - Add unit tests
   - Cross-platform integration testing


