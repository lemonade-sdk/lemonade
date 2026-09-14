# Lemonade Desktop App (Tauri)

A native desktop GUI for interacting with the Lemonade Server.

For the end-user workflow, see the [GUI3 app guide](../guide/gui3.md).

## Overview

This app provides a native desktop experience for managing models and chatting with LLMs running on `lemond`. It connects to the server via HTTP API and offers a modern, resizable panel-based interface.

It is built with **Tauri v2**, which embeds the operating system's native webview — WebView2 on Windows, WKWebView on macOS, and webkit2gtk on Linux — instead of bundling Chromium. The renderer is a standard React 19 + TypeScript application served by webpack and shared with the browser-only `src/web-app/` build.

**Key Features:**
- Model management (list, pull, load/unload)
- Chat interface with markdown/code rendering and LaTeX support
- Real-time server log viewer
- Persistent client preferences and server-owned model load settings
- Custom frameless window controls
- `lemonade://` deep-link protocol handler
- UDP beacon discovery to find a running `lemond` server on the local machine

## Deployment Topology

The Tauri desktop app is a **thin client** for a separately-running `lemond` server. A single `lemond` can be driven by multiple clients at once, including clients running on other machines.

Consequences that callers of this code need to know about:

- **Per-client local state.** Layout sizes, theme, base URL, an optionally remembered API key, chat history, and request-time chat sampling belong to the GUI client. Native host settings live in `~/.config/lemonade/app_settings.json`; browser-capable preferences use scoped web storage.
- **Server-owned configuration.** Persistent per-model load options from **Models > Configuration**, memory and eviction settings, model directories, and cloud-provider definitions are read from and written to `lemond`. Multiple clients connected to one server therefore share them. Cloud API keys remain environment-backed or ephemeral in server memory and are never written to `config.json`.
- **The desktop app does not manage `lemond`'s lifecycle.** The server is started independently — on Windows by `LemonadeServer.exe` (auto-started via the startup folder, tray icon always visible), on Linux/macOS by the user or a service. The Tauri app is opened on demand and must not add itself to autostart, spawn `lemond` as a subprocess, or assume `lemond` is on the same machine.
- **Discovery is best-effort local + explicit remote.** `beacon.rs` listens for a UDP broadcast emitted by a local `lemond` to auto-populate the base URL. For remote-server use, the user sets `baseURL` + `apiKey` in settings and the client talks to that endpoint directly.

## Publishing GUI3 Beta packages

The manual [`GUI3 Beta Build`](https://github.com/lemonade-sdk/lemonade/blob/main/.github/workflows/gui3_beta_build.yml)
workflow builds Windows, macOS, and Linux packages. Run it from the branch
that should be tested, set **Publish the GUI-only packages as a GitHub
prerelease** to `true`, and optionally change the artifact label. The workflow
uploads short-lived Actions artifacts for every run and, when publishing is
enabled, creates a numbered GitHub prerelease containing only the GUI-only
packages.

The published packages intentionally exclude `lemond`, the CLI, and model
resources. This lets testers extract the package beside an existing Lemonade
installation without replacing its server or competing for port `13305`. The
beta GUI still talks to that existing server, so server-owned changes are
shared; use a separate server process and port only when an isolated test
environment is required.

## Code Structure

```
src/app/
├── package.json                   # Webpack + Tauri CLI devDependencies
├── webpack.config.js              # Bundler config (target: web)
├── tsconfig.json                  # TypeScript config
├── assets/                        # Icons, logos
│
├── src/
│   ├── index.tsx                  # Renderer entry (imports tauriShim first)
│   ├── tauriShim.ts               # Installs window.api → Tauri invoke() bridge
│   ├── App.tsx                    # Root component and workspace orchestration
│   ├── api.ts                     # HTTP client and normalized server state
│   ├── modelConfiguration.ts      # Client sampling and load-option resolution
│   ├── components/                # Chat, Models, Backends, Monitor, Settings
│   ├── features/                  # Feature-specific state and helpers
│   ├── hooks/                     # Shared React hooks
│   ├── styles/                    # Tokens and renderer styles
│   └── tools/                     # MCP and Omni tool definitions
│
└── src-tauri/                     # Rust host (Tauri backend)
    ├── Cargo.toml                 # Rust dependencies
    ├── tauri.conf.json            # Window config, bundle settings, plugins
    ├── build.rs                   # tauri_build::build()
    ├── capabilities/default.json  # Tauri permissions
    ├── icons/                     # Generated app icons (32x32/128x128/ico/icns)
    └── src/
        ├── main.rs                # Entry point (binary)
        ├── lib.rs                 # Tauri builder, plugin wiring, deep-link routing
        ├── commands.rs            # #[tauri::command] handlers (window, settings, port)
        ├── events.rs              # Tauri event channel name constants
        ├── settings.rs            # app_settings.json read/write + sanitize
        ├── beacon.rs              # UDP beacon listener (single bound socket)
        ├── tray_launcher.rs       # macOS-only tray auto-start helper
        └── webview_shim.rs        # Per-platform webview hooks (mic permission, link interception)
```

> Server API calls are not proxied through Rust. The renderer uses `api.ts` to call the configured `lemond` endpoint directly; `App.tsx` owns the shared health, model, and system-information lifecycle.

## Architecture

```
┌────────────────────────────────────────────────┐
│  Tauri Rust Host (src-tauri/)                  │
│  Window mgmt, IPC commands, background tasks   │
│  UDP beacon listener, settings file I/O        │
├────────────────────────────────────────────────┤
│  tauriShim.ts (installs window.api in webview) │
│  Maps window.api.* → invoke() / listen()       │
├────────────────────────────────────────────────┤
│  React Renderer (TypeScript)                   │
│  Source lives in src/app/src/; the web-app     │
│  build (src/web-app/) reuses it via webpack    │
│  relative entry/template paths — no symlinks.  │
├────────────────────────────────────────────────┤
│  HTTP API → lemond (C++ server)                │
└────────────────────────────────────────────────┘
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

# Production build with platform bundles (macOS .app, Linux .deb/.rpm, Windows MSI/NSIS)
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

## Testing custom Omni Models

The custom Omni Model UI (see [Register a custom Omni Model from the desktop app](../guide/configuration/custom-models.md#register-a-custom-omni-model-from-the-desktop-app)) is implemented by `src/components/ModelManager.tsx` and `src/features/customModels/customModelStore.ts`.

### Manual desktop smoke test

Use the desktop app to verify the user-facing flow end to end:

1. Start the Lemonade desktop app.
2. Download at least one chat-capable LLM in **Model Manager**.
3. Optionally download one image model, one edit-capable image model, one vision model, one transcription model, and one speech model.
4. Open **Models**, select **Open custom models**, and choose **Omni Collection**.
5. Save an Omni Collection with only a planner LLM and verify it appears as `user.<name>` in the chat model picker.
6. Edit the Omni Model to add optional role models and save again.
7. Select the Omni Model in chat and run prompts that trigger the configured tools, such as image generation, speech synthesis, audio transcription, or image analysis.
8. Export the Omni Model JSON, delete the Omni Model, import the JSON, and verify it reappears.
9. Delete one component model and verify the now-stale Omni Model is hidden from the picker until the component is registered again.

## Testing the Rust host

Unit tests live alongside the Rust modules and cover settings sanitization, beacon parsing, and deep-link URL parsing:

```bash
cargo test --manifest-path src-tauri/Cargo.toml
```
