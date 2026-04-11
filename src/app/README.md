# Lemonade Desktop App (Tauri)

A native desktop GUI for interacting with the Lemonade Server.

## Overview

This app provides a native desktop experience for managing models and chatting with LLMs running on `lemond`. It connects to the server via HTTP API and offers a modern, resizable panel-based interface.

It is built with **Tauri v2**, which embeds the operating system's native webview — WebView2 on Windows, WKWebView on macOS, and webkit2gtk on Linux — instead of bundling Chromium. The renderer is a standard React 19 + TypeScript application served by webpack and shared with the browser-only `src/web-app/` build.

**Key Features:**
- Model management (list, pull, load/unload)
- Chat interface with markdown/code rendering and LaTeX support
- Real-time server log viewer
- Persistent layout and inference settings
- Custom frameless window with zoom controls
- `lemonade://` deep-link protocol handler
- UDP beacon discovery to find a running `lemond` server on the local machine

## Deployment Topology

The Tauri desktop app is a **thin client** for a separately-running `lemond` server. A single `lemond` can be driven by multiple clients at once, including clients running on other machines — the Linux AppImage in particular is shipped as a remote client that points at a `lemond` elsewhere on the network.

Consequences that callers of this code need to know about:

- **Per-client local state.** All user-tunable state (inference params, layout sizes, zoom, base URL, API key) lives in `~/.cache/lemonade/app_settings.json` on the client, owned by the Rust host (`src-tauri/src/settings.rs`). It is **never** stored or proxied through `lemond`, because two clients against the same server must be able to hold different preferences.
- **The desktop app does not manage `lemond`'s lifecycle.** The server is started independently — on Windows by `LemonadeServer.exe` (auto-started via the startup folder, tray icon always visible), on Linux/macOS by the user or a service. The Tauri app is opened on demand and must not add itself to autostart, spawn `lemond` as a subprocess, or assume `lemond` is on the same machine.
- **Discovery is best-effort local + explicit remote.** `beacon.rs` listens for a UDP broadcast emitted by a local `lemond` to auto-populate the base URL. For remote-server use (AppImage or any hand-configured setup), the user sets `baseURL` + `apiKey` in settings and the client talks to that endpoint directly.

## Code Structure

```
src/app/
├── package.json                   # Webpack + Tauri CLI devDependencies
├── webpack.config.js              # Bundler config (target: web)
├── tsconfig.json                  # TypeScript config
├── assets/                        # Icons, logos
│
├── src/
│   ├── global.d.ts                # window.api type declaration
│   └── renderer/                  # React UI (TypeScript)
│       ├── index.tsx              # Renderer entry (imports tauriShim first)
│       ├── tauriShim.ts           # Installs window.api → Tauri invoke() bridge
│       ├── App.tsx                # Root component, layout orchestration
│       ├── TitleBar.tsx           # Custom window controls
│       ├── ModelManager.tsx       # Model list and actions
│       ├── ChatWindow.tsx         # LLM chat interface
│       ├── LogsWindow.tsx         # Server log viewer
│       ├── SettingsPanel.tsx      # Inference parameters
│       └── utils/                 # API helpers and config
│
└── src-tauri/                     # Rust host (Tauri backend)
    ├── Cargo.toml                 # Rust dependencies
    ├── tauri.conf.json            # Window config, bundle settings, plugins
    ├── build.rs                   # tauri_build::build()
    ├── capabilities/default.json  # Tauri permissions
    ├── icons/                     # Generated app icons (32x32/128x128/ico/icns)
    └── src/
        ├── main.rs                # Entry point (binary)
        ├── lib.rs                 # Tauri builder, plugin wiring
        ├── commands.rs            # #[tauri::command] handlers
        ├── settings.rs            # app_settings.json read/write + sanitize
        ├── beacon.rs              # UDP beacon discovery + background listener
        ├── system_info.rs         # /api/v1/system-stats + system-info proxy
        └── tray_launcher.rs       # macOS-only tray auto-start helper
```

## Architecture

```
┌──────────────────────────────────────────────┐
│  Tauri Rust Host (src-tauri/)                │
│  Window mgmt, IPC commands, background tasks │
│  UDP beacon listener, settings file I/O      │
├──────────────────────────────────────────────┤
│  tauriShim.ts (installs window.api in webview)│
│  Maps window.api.* → invoke() / listen()     │
├──────────────────────────────────────────────┤
│  React Renderer (TypeScript, shared with     │
│  src/web-app/ via symlink)                   │
├──────────────────────────────────────────────┤
│  HTTP API → lemond (C++ server)              │
└──────────────────────────────────────────────┘
```

## Prerequisites

- **Node.js** 20 or higher (webpack)
- **Rust toolchain** via [rustup](https://rustup.rs) (Rust 1.77+)
- **Linux only:** `libwebkit2gtk-4.1-dev`, `libsoup-3.0-dev`, `libjavascriptcoregtk-4.1-dev`, `librsvg2-dev`, `libayatana-appindicator3-dev` — the repo's `setup.sh` script checks for these and prompts to install them.
- **Windows only:** WebView2 runtime (pre-installed on Windows 10 1803+ and Windows 11).
- **macOS only:** No extra dependencies — WKWebView ships with the OS.

## Building

```bash
cd src/app

# Install webpack + Tauri CLI dependencies
npm ci

# Run in dev mode (opens a window, hot-reloads webpack)
npm run dev

# Production build (single binary, no OS bundles)
npm run build -- --no-bundle

# Production build with platform bundles (macOS .app, Linux AppImage, Windows MSI/NSIS)
npm run build
```

The preferred path for shipping is through CMake, which stages the Tauri output alongside the rest of the server:

```bash
cmake --build --preset default --target tauri-app      # Linux / macOS
cmake --build --preset windows --target tauri-app      # Windows
```

## Development Scripts

```bash
npm run dev                    # Tauri dev mode (window + hot-reload)
npm run build                  # Tauri production build
npm run tauri icon <path>      # Regenerate icons from a source image
npm run build:renderer         # Build just the renderer (webpack, dev mode)
npm run build:renderer:prod    # Build just the renderer (webpack, production)
npm run watch:renderer         # Webpack watch mode for the renderer only
```

## Testing the Rust host

Unit tests live alongside the Rust modules and cover settings sanitization, beacon parsing, and deep-link URL parsing:

```bash
cargo test --manifest-path src-tauri/Cargo.toml
```
