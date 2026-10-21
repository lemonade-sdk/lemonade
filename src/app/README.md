# Lemonade App

The Lemonade App uses a shared **React 19 + TypeScript + webpack** frontend. The same UI runs in the **Tauri desktop app** and in the **browser app** served by `lemond`. The server owns models, backends, and persistent model configuration; the GUI owns its connection, presentation, and optional local chat history.

For user-facing instructions, see the [Lemonade App guide](../../docs/guide/app/README.md). For Tauri and platform build details, see the [desktop app developer guide](../../docs/dev/app.md).

## Prerequisites

- Node.js 20+ and npm (install dependencies with `npm ci`).
- A running Lemonade Server (`lemond`) for end-to-end testing. The default endpoint is `http://localhost:13305`; it can be changed under **Settings > Server**.
- Rust/Cargo and platform-specific dependencies only for Tauri desktop builds; see the [desktop app developer guide](../../docs/dev/app.md#prerequisites).

Run the following commands from `src/app/`.

## Development

```bash
npm ci
npm run dev:renderer
```

This starts the webpack development server at **http://localhost:9123** with hot reloading. It does not launch the desktop app. To develop the desktop app and its renderer together, use `npm run dev` (Tauri).

The GUI calls the real server's `/api/v1/...` endpoints. A renderer running on port 9123 is not itself `lemond`; check **Settings > Server** if it connects to the wrong instance. Browser builds served by `lemond` use the server origin when no saved endpoint overrides it.

## Building

| Command | Result |
| --- | --- |
| `npm run build` | Production renderer build (alias for `build:renderer:prod`) in `dist/renderer/`. |
| `npm run build:tauri` | Tauri desktop bundles (requires Rust and platform dependencies). |
| `npm run build:nobundle` | Tauri executable without an installer bundle. |
| `npm run watch:renderer` | Rebuild the renderer when sources change. |

Tauri build commands use the Lemonade release version from `tools/version.py`
(or `LEMONADE_VERSION`) rather than the prototype package version. On
Windows, the executable's numeric and text versions use that same
release version.

The CMake integration also provides a `tauri-app` target, for example `cmake --build --preset windows --target tauri-app` after configuring the repository. On Windows, this stages `lemonade-app.exe` under `build/app/`.

**Browser distribution is built separately** under `src/web-app/`, reusing `src/app/src/`. Its dependency manifest and build configuration must remain separate for Debian's system-module packaging. See the [web UI developer guide](../../docs/dev/web-ui.md).

## Testing

```bash
# Run from src/app/
npx playwright install chromium  # First-time Playwright browser setup

npm test                         # 1. Runtime regressions
                                 # 2. Playwright accessibility (a11y)
                                 # 3. Playwright functional tests

npm run test:server              # Real-server smoke tests (requires running lemond)
npm run test:headed              # Same as npm test, but with visible Playwright browsers
npm run typecheck                # TypeScript type checking
npm run test:ui-regressions      # Focused UI regression tests (also included in npm test)
```

`npm test` uses an isolated, non-HMR renderer at `127.0.0.1:4173` by default. The test runner executes the accessibility and functional suites sequentially to avoid shared-browser-state collisions. `PLAYWRIGHT_BASE_URL` can target another test URL; a loopback URL still starts the managed test server.

For accessibility expectations and focused tests, see [Accessibility](ACCESSIBILITY.md).

Additional focused runtime tests are listed in `package.json`, including `test:mcp-runtime`, `test:effective-settings`, and `test:storage`.

## Features and ownership

The UI provides Chat (including supported multimodal modes), model discovery and server-side registration, model loading and configuration, backend management, the Apps marketplace, Monitor, and Settings. Model settings are sent to the server; client-local Favorites and request-time Chat settings are not server-wide configuration.

- **Server-owned:** downloaded and loaded models, custom model definitions, backend installations, persistent load settings, and server configuration.
- **Client-owned:** server endpoint, theme and layout, Favorites, request-time Chat preferences, and optional chat history. Chat history is stored locally only when enabled in **Settings > Chat**; attachments and generated media are not saved as part of the persistent history.
- **Authentication:** an optional API key controls access to the configured server.

Multiple GUI clients may connect to the same server. Server-side changes can be visible to all of them, while each client's local preferences remain independent.

## Source layout

```text
src/
  index.tsx          React entry point
  App.tsx            Workspaces, routing and application shell
  api.ts             Lemonade Server API client
  components/        Chat, Models, Backends, Monitor and Settings UI
  features/          Chat history, models, custom models, downloads, etc.
  hooks/             React hooks
  storage.ts         Local storage and legacy data migration
  tauriShim.ts       Desktop host integration
src-tauri/           Native Tauri host
scripts/             Test runner and supporting utilities
tests/               Playwright and focused runtime tests
webpack.config.js    Renderer bundler and dev-server configuration
playwright.config.ts Playwright projects and isolated test server
```

## Troubleshooting

- **Connection fails:** check `lemond` and its URL/API key in **Settings > Server**. For a local instance, try `curl http://localhost:13305/api/v1/health`.
- **Port 9123 is busy:** stop the other renderer instance or run `npm run dev:renderer -- --port 9000`. Tauri's configured development URL remains port 9123, so update that configuration if you change ports while developing the desktop app.
- **Playwright cannot start:** run `npx playwright install chromium`, check whether port 4173 is occupied, and clear an unintended `PLAYWRIGHT_BASE_URL` override.
- **Changes are not visible:** hard-refresh the renderer or restart `npm run dev:renderer`.
