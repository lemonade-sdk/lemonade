# Lemonade Desktop App (Tauri)

The desktop app is a **Tauri v2 thin client** for a separately running
Lemonade Server (`lemond`). Its React 19 + TypeScript renderer is shared with
the browser UI built from `src/web-app/`; both use the same source under
`src/app/src/` and call the server over HTTP.

Tauri uses WebView2 on Windows, WKWebView on macOS, and webkit2gtk on Linux.
It does not bundle Chromium or start and stop the Lemonade Server. On Windows,
the installed server has its own tray/launcher process; the desktop app is
opened separately. A single server can serve several GUI clients.

## State and responsibilities

- **Server (`lemond`):** downloaded and loaded models, backends, custom model
  definitions, persistent model load settings, and server configuration.
- **Desktop host:** native window features, deep links (`lemonade://`), local
  server discovery via UDP beacon, and host settings such as the saved server
  endpoint and an API key when the user elects to remember it. The Rust host
  handles `app_settings.json` through `src-tauri/src/settings.rs`.
- **React renderer:** client-local UI preferences, selected tools, Favorites,
  and opt-in browser chat history. Persistent history strips attached and
  generated media. These values are not server-side user accounts.

The app connects to the saved endpoint or discovers a local server where
possible; remote servers can be specified under **Settings > Server**. Server
health and other API requests use the regular HTTP client, not a separate
Rust-side proxy.

## Source layout

```text
src/app/
  package.json                  Tauri and webpack scripts
  webpack.config.js             Renderer bundling and dev server (port 9123)
  playwright.config.ts          UI test configuration
  src/
    index.tsx                   React entry point
    tauriShim.ts                window.api / Tauri host integration
    App.tsx                     Workspaces and navigation
    api.ts                      Lemonade Server API client
    components/                 Chat, Models, Backends, Monitor, Settings
    features/                   Model, download, chat history, router logic
    hooks/                      Shared React hooks
  src-tauri/
    tauri.conf.json             Desktop build and window configuration
    src/settings.rs             Host settings persistence
    src/beacon.rs               Local server discovery
    src/lib.rs                  Tauri setup, commands and plugins
src/web-app/                   Separate browser build using shared React UI
```

The browser build has its own dependency manifest to support Debian system
modules. Do not merge `src/web-app/package.json` into `src/app/package.json`.
See [web UI development](web-ui.md).

## Prerequisites

- Node.js 20+ and npm.
- Rust/Cargo (via [rustup](https://rustup.rs)) for desktop builds.
- Linux: `libwebkit2gtk-4.1-dev`, `libsoup-3.0-dev`,
  `libjavascriptcoregtk-4.1-dev`, `librsvg2-dev`, and
  `libayatana-appindicator3-dev` (the repository's `setup.sh` checks for
  dependencies).
- Windows: WebView2 runtime and the Visual Studio/CMake toolchain for CMake
  builds. macOS includes WKWebView.

## Building

From the repository root, use the platform's setup script to configure the
CMake presets. The CMake `tauri-app` target builds and stages the native app:

```bash
cmake --build --preset default --target tauri-app  # Linux / macOS
cmake --build --preset windows --target tauri-app  # Windows
```

For direct frontend development, run the following from `src/app/`:

```bash
npm ci
npm run dev                  # Tauri desktop window + webpack hot reload
npm run dev:renderer         # Webpack renderer only, http://localhost:9123
npm run build:renderer:prod  # Production renderer in dist/renderer/
npm run build:nobundle       # Desktop executable without installer bundles
npm run build:tauri          # Desktop app with platform bundles
```

`npm run build` is an alias for `build:renderer:prod`: it **does not** build the
Tauri desktop executable. `npm run watch:renderer` rebuilds the renderer on
source changes. On Windows, the CMake target stages `lemonade-app.exe` under
`build/app/`.

## Testing

```bash
npx playwright install chromium   # first run only
npm test                         # sequential Playwright a11y + functional
npm run typecheck
npm run test:ui-regressions
cargo test --manifest-path src-tauri/Cargo.toml
```

`npm test` uses a separate renderer at `127.0.0.1:4173` by default; real-server
smoke tests can be enabled with `LEMONADE_REAL_SERVER=1 npm test`. Other
focused Node tests are listed in `src/app/package.json`, including
`test:router-editor`, `test:mcp-runtime`, and `test:effective-settings`.

For a manual desktop integration check, start `lemond` independently, connect
under **Settings > Server**, open **Models**, download and load a chat model,
and send a chat message. To check an Omni Collection, use **Open custom
models** in the Models toolbar, configure its component models, and verify
that it is offered in the Chat model picker. Custom model definitions are
registered on the server, not in per-user browser accounts.
