# Lemonade Desktop App (Tauri)

The desktop app is a **Tauri v2 thin client** for a separately running Lemonade Server (`lemond`). Its React 19 + TypeScript renderer is shared with the browser UI built from `src/web-app/`; both use the same source under `src/app/src/` and call the server over HTTP.

Tauri uses WebView2 on Windows, WKWebView on macOS, and webkit2gtk on Linux. It does not bundle Chromium or start and stop the Lemonade Server. On Windows, the installed server has its own tray/launcher process; the desktop app is opened separately. A single server can serve several GUI clients.

## State and responsibilities

- **Server (`lemond`):** downloaded and loaded models, backends, custom model definitions, persistent model load settings, and server configuration.
- **Desktop host:** native window features, deep links (`lemonade://`), local server discovery via UDP beacon, and host settings such as the saved server endpoint and an API key when the user elects to remember it. The Rust host handles `app_settings.json` through `src-tauri/src/settings.rs`.
- **React renderer:** client-local UI preferences, selected tools, Favorites, and opt-in browser chat history. Persistent history strips attached and generated media. These values are not server-side user accounts.

The app connects to the saved endpoint or discovers a local server where possible; remote servers can be specified under **Settings > Server**. Server health and other API requests use the regular HTTP client, not a separate Rust-side proxy.

## Source layout

Selected paths (not a complete file listing):

```text
src/app/
  package.json                   npm scripts and dependencies
  webpack.config.js              Renderer build and dev server (port 9123)
  playwright.config.ts           UI test server (port 4173) and projects
  src/
    index.tsx                    React entry point
    App.tsx                      Shared application shell and navigation
    i18n.tsx                     UI translations and locale management
    api.ts                       Lemonade Server HTTP client
    tauriShim.ts                 Desktop/browser host bridge
    storage.ts                   Client-local storage and migrations
    components/                  Workspace views (Chat, Models...) and dialogs
    features/                    Models, collections, router, downloads, chat
    hooks/                       Shared React hooks
    tools/                       MCP and Omni tools, tool definitions
    styles/                      CSS and theme tokens
  src-tauri/
    tauri.conf.json              Tauri build and development configuration
    src/
      lib.rs                     Native host initialization and plugins
      commands.rs                Tauri commands for the renderer
      settings.rs                Host settings persistence
      beacon.rs                  Local server discovery
  scripts/
    run-gui-tests.mjs            Runtime and Playwright test runner
    run-runtime-tests.mjs        Node runtime regression tests
    run-playwright-tests.mjs     Accessibility and functional UI tests
    run-server-tests.mjs         Real-server smoke test selection
  tests/
    a11y.spec.ts                 Accessibility tests
    features.spec.ts             Functional UI tests
    *.runtime.cjs                Focused Node regression tests
src/web-app/
  package.json                   Separate browser build dependencies
  webpack.config.js              Browser bundle configuration
test/app/
  check_app_documentation.py     Documentation consistency checks
```

The browser build has its own dependency manifest to support Debian system modules. Do not merge `src/web-app/package.json` into `src/app/package.json`. See [web UI development](web-ui.md).

## Prerequisites

- Node.js 20+ and npm.
- Rust/Cargo (via [rustup](https://rustup.rs)) for desktop builds.
- Linux: `libwebkit2gtk-4.1-dev`, `libsoup-3.0-dev`, `libjavascriptcoregtk-4.1-dev`, `librsvg2-dev`, and `libayatana-appindicator3-dev` (the repository's `setup.sh` checks for dependencies).
- Windows: WebView2 runtime and the Visual Studio/CMake toolchain for CMake builds. macOS includes WKWebView.

## Building

From the repository root, use the platform's setup script to configure the CMake presets. The CMake `tauri-app` target builds and stages the native app:

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

`npm run build` is an alias for `build:renderer:prod`: it **does not** build the Tauri desktop executable. `npm run watch:renderer` rebuilds the renderer on source changes. On Windows, the CMake target stages `lemonade-app.exe` under `build/app/`.

## Testing

Run these commands from `src/app/`:

```bash
npx playwright install chromium  # First-time Playwright browser setup

npm test                         # 1. Runtime regressions
                                 # 2. Playwright accessibility (a11y)
                                 # 3. Playwright functional tests

npm run test:a11y                # Accessibility tests only
npm run test:server              # Real-server smoke tests (requires running lemond)
npm run test:headed              # Same as npm test, with visible Playwright browsers
npm run typecheck                # TypeScript type checking
npm run test:ui-regressions      # Focused regressions (also included in npm test)
cargo test --manifest-path src-tauri/Cargo.toml  # Rust host tests
```

`npm test` runs without a Lemonade Server and uses an isolated renderer at `127.0.0.1:4173` by default. `npm run test:server` is separate: it runs the `@real-server` Playwright tests against an already running `lemond`. For accessibility expectations and manual checks, see [Accessibility](../../src/app/ACCESSIBILITY.md).

Other focused Node tests are listed in `src/app/package.json`, including `test:router-editor`, `test:mcp-runtime`, and `test:effective-settings`.

For a manual desktop integration check, start `lemond` independently, connect under **Settings > Server**, open **Models**, download and load a chat model, and send a chat message. To check an Omni Collection, use **Open custom models** in the Models toolbar, configure its component models, and verify that it is offered in the Chat model picker. Custom model definitions are registered on the server, not in per-user browser accounts.

## Keeping the App guide accurate

The [App guide](../guide/app/README.md) is split into task-focused pages so UI changes can be reviewed near the affected behavior. Run `python3 test/app/check_app_documentation.py` from the repository root when updating the interface or its documentation. The checker verifies navigation, local links, referenced npm scripts, and key UI labels. This check cannot verify every screenshot or user workflow. Before each release, manually review the guide against the current Lemonade App and run the App and real-server smoke tests.
