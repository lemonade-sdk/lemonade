# Lemonade Build Options

## Linux Tray Configuration

### Build Options

#### `REQUIRE_LINUX_TRAY` (Default: **OFF** / auto-detect)
Enable system tray support on Linux via AppIndicator3 (GTK3 not required when using the GLib variant).

- When **OFF** (default): Tray support is auto-detected at configure time. If AppIndicator3 libraries are found, `lemonade-tray` is built as a separate executable. `lemonade-server` on Linux is always headless regardless. If dependencies are missing, only `lemonade-server` is built (headless mode).
- When **ON**: Tray support is required — the build will fail if the dependencies are not found.

Optional runtime dependencies (for tray support):
- One of (preferred first):
  - `ayatana-appindicator-glib-devel` (recommended, GTK-free — only GLib/GIO required)
  - `ayatana-appindicator3-devel` + `gtk3-devel` (Ayatana GTK3 variant)
  - `libappindicator-gtk3-devel` + `gtk3-devel` (upstream libappindicator3)
- `libnotify-devel` (optional, enables desktop notifications)

```bash
# Auto-detect (default): tray enabled if deps are found
cmake ../src/cpp

# Explicitly require tray support (fail if deps missing)
cmake -DREQUIRE_LINUX_TRAY=ON ../src/cpp
```

---

## React App Build Configuration

The CMake build system allows you to control whether the React web app and/or Tauri desktop app are built and included in the server.

### Build Options

#### `BUILD_WEB_APP` (Default: **ON**)
Build and include the React web app for browser access via the `/app` endpoint.

- When **ON**: The web app will be automatically built during CMake configuration if not already present
- When **OFF**: The `/app` endpoint will serve a minimal fallback page
- Requires: Node.js and npm

#### `BUILD_TAURI_APP` (Default: **OFF**)
Build and include the full Tauri desktop application.

- When **ON**: Enables the `tauri-app` target for building the desktop app (Rust host + React renderer)
- When **OFF**: Desktop app targets are disabled
- Requires: Node.js, npm, Rust toolchain (rustup), and Linux webkit2gtk development libraries

### Usage Examples

#### 1. Default Configuration (Web App Only)
```bash
cd build
cmake ..
ninja lemond
```

Result:
- ✅ Web app available at `http://localhost:13305/app`
- ❌ No Tauri desktop app
- Minimal build time and dependencies

#### 2. Server Only (No UI)
```bash
cd build
cmake -DBUILD_WEB_APP=OFF ..
ninja lemond
```

Result:
- ❌ No web app
- ❌ No Tauri desktop app
- Minimal fallback page at `/app`
- Fastest build time

#### 3. Both Web App and Tauri App
```bash
cd build
cmake -DBUILD_WEB_APP=ON -DBUILD_TAURI_APP=ON ..
ninja lemond
ninja tauri-app  # Build desktop app separately
```

Result:
- ✅ Web app at `http://localhost:13305/app`
- ✅ Tauri desktop app available
- Both `webapp` and `tauri-app` targets enabled

#### 4. Tauri App Only
```bash
cd build
cmake -DBUILD_WEB_APP=OFF -DBUILD_TAURI_APP=ON ..
ninja tauri-app
```

Result:
- ❌ No web app
- ✅ Tauri desktop app available
- Useful for desktop-only deployments

### Build Targets

When Node.js/npm/Cargo are available, the following targets are created:

#### `webapp` (when `BUILD_WEB_APP=ON`)
```bash
cmake --build . --target webapp
# or
ninja webapp
```

Builds just the React web app bundle using Webpack. Output goes to `build/resources/web-app/`.

#### `tauri-app` (when `BUILD_TAURI_APP=ON`)
```bash
cmake --build . --target tauri-app
# or
ninja tauri-app
```

Builds the full Tauri desktop application. This compiles the React renderer via webpack and then invokes `cargo tauri build` to compile the Rust host and link against the system WebView (WebView2 on Windows, WKWebView on macOS, webkit2gtk on Linux). Output is staged at `build/app/lemonade-app[.exe|.app]`.

### Automatic Building

During CMake configuration, if `BUILD_WEB_APP=ON` or `BUILD_TAURI_APP=ON` and the app hasn't been built yet:

1. CMake checks for existing `dist/renderer` directory
2. If not found and Node.js is available, automatically runs:
   - `npm ci` (in `src/web-app/` for web builds, `src/app/` for Tauri builds)
   - `npm run build:renderer:prod` (webpack) / `cargo tauri build` (Tauri)
3. Copies the built app to the expected location
4. If build fails or dependencies are missing, creates a fallback HTML page with instructions

### Source Directories

- **Web App**: `src/web-app/`
  - Symlinks to shared source: `src/` → `../app/src/`
  - Minimal dependencies (no Tauri, no Node runtime)
  - Target: browser (`webpack target: 'web'`)
  - Build output: `src/web-app/dist/renderer/`

- **Tauri App**: `src/app/`
  - Rust host in `src/app/src-tauri/`
  - React renderer in `src/app/src/renderer/` (shared with web-app via symlink)
  - Webpack target: `web` (Tauri hosts a standard webview, not electron-renderer)
  - Renderer build output: `src/app/dist/renderer/`
  - Cargo build output: `src/app/src-tauri/target/release/lemonade-app[.exe|.app]`

### Configuration Status

When running CMake, you'll see:

```
-- === App Build Configuration ===
--   BUILD_WEB_APP: ON
--   BUILD_TAURI_APP: OFF
-- React web app target enabled: cmake --build . --target webapp
-- Full Tauri app target disabled (BUILD_TAURI_APP=OFF)
```

### Requirements

- **Server Only**: No special requirements
- **Web App**: Node.js 18+ and npm
- **Tauri App**: Node.js 18+, npm, Rust (via https://rustup.rs), plus on Linux `libwebkit2gtk-4.1-dev`, `libsoup-3.0-dev`, `libjavascriptcoregtk-4.1-dev`, `librsvg2-dev`, `libayatana-appindicator3-dev`

### Deployment Recommendations

| Use Case | BUILD_WEB_APP | BUILD_TAURI_APP | Benefits |
|----------|--------------|-----------------|----------|
| Server deployment | ON | OFF | Web UI accessible from any browser |
| Desktop-only | OFF | ON | Standalone app, no web browser needed |
| Development | ON | OFF | Fast builds, easy testing |
| Full distribution | ON | ON | Both web and desktop access |
| Headless server | OFF | OFF | Minimal footprint, API-only |

### Troubleshooting

**"React app build failed - creating fallback page"**
- Install Node.js from https://nodejs.org/
- Run `npm install` in `src/web-app/` or `src/app/`
- Check Node.js version: `node --version` (requires 18+)

**"Tauri app build targets disabled"**
- Node.js, npm, or Cargo not found in PATH
- Install Node.js and Rust (via https://rustup.rs) and add them to your PATH
- On Linux, install the webkit2gtk development packages (see Requirements above)

**Both options OFF but still seeing an app?**
- The `/app` endpoint will serve a minimal fallback HTML page with instructions
- This is intentional to guide users on how to enable the full app
