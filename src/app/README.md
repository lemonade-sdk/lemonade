# Lemonade UI Redesign Prototype

A **React 19 + TypeScript + webpack** proof-of-concept for the next-generation Lemonade UI. This prototype is built side-by-side with the existing `src/app/` and `src/web-app/` in the main codebase and now runs **real-server-first** against `lemond` at `http://localhost:13305` by default. It is designed to work both as a web app and as a desktop Tauri application — a single React codebase powering both delivery channels.

**Not production code** — this is an active design and engineering POC on branch `feat/ui-testing`. See [`.squad/decisions.md`](../../.squad/decisions.md) for the design rationale and capability-keyed presets architecture (v1.4).

## Prerequisites

- **Node.js 20+** (check `package.json` `engines` field for exact requirement)
- **npm 10+**
- Optional for UI-only review, recommended for functional testing: a running `lemond` instance at `http://localhost:13305` or a custom URL entered in Connect

## Quick Start

### Install dependencies

```bash
npm install
```

### Run the dev server

```bash
npm run dev
```

Opens at **http://localhost:8080** with hot-module reloading via webpack-dev-server.

## Build & Distribution

### Production build

```bash
npm run build
```

Outputs optimized bundles to `dist/`.

### Watch mode (for development)

```bash
npm run watch
```

Incrementally rebuilds on file changes.

## Testing

### Headless tests (CI mode)

```bash
npm test
```

Runs all UI-safe Playwright tests headless via Chromium. Real-server smoke tests are opt-in so they fail fast instead of silently passing without a running server or loaded model:

```bash
LEMONADE_REAL_SERVER=1 npm test
```

Artifacts are saved under Playwright's per-test output folders, so screenshots from repeated runs are not overwritten.

### Headed tests (visible browser)

```bash
npm run test:headed
```

Opens the browser so you can watch the tests run step-by-step.

### First run setup

On first run, Playwright may ask to install browser binaries:

```bash
npx playwright install
```

## What's Implemented

This prototype showcases the **redesigned UI** with capability-keyed presets (v1.4):

### UI Panels
- **Chat** — multi-turn conversation with streaming support, preset selector, sampling controls
- **Models** — model registry with load/unload, categorized view (Loaded / Installed / Available)
- **Backends** — device-first capability matrix, backend versions and status
- **Connect / Discover** — integration showcase and curated model feed
- **Presets** (NEW) — capability-keyed preset system with chat (Balanced, Quality, Fast, Creative, Long Context, Code) and image (Sharp, Quick) starters

### Presets v1.4 Features
- **Capability-keyed compatibility** — presets declare `applies_to: [capability]` and models declare `labels`; runtime matches by label intersection
- **Staged bindings** — when you adjust preset settings (temperature, top-p, etc.), they show "Will apply on next load" — no immediate server calls
- **Sampling wired** — temperature, top_p, top_k, repeat_penalty settings are forwarded to `/api/v1/chat/completions`
- **Advanced disclosure** — backend hint field behind an Advanced toggle for power users
- **Distinct image presets** — Steps and CFG scale controls for image generation, separate from chat sampling

## Project Structure

```
src/
  index.tsx              # React entry point
  index.html            # HTML shell
  App.tsx               # Root component
  api.ts                # API client (health, models, chat/completions, etc.)
  presetStore.ts        # Presets state & v1.4 capability-keyed data model
  components/           # React components (Chat, Models, Backends, Presets, etc.)
  hooks/                # Custom React hooks
  styles/               # CSS modules and global styles
  tools/                # Utility functions

tests/
  features.spec.ts      # Playwright test suite

webpack.config.js       # Webpack configuration (dev server, loaders, bundles)
playwright.config.ts    # Playwright configuration (baseURL, browsers, output dirs)
tsconfig.json           # TypeScript configuration
```

## Connecting to a Server

The prototype stores the Lemonade server URL in local browser state and defaults to `http://localhost:13305`. Use the Connect screen to change it; the field validates `http://` / `https://` URLs before attempting a request and shows the exact endpoint plus HTTP/network error on failure. API keys can be kept session-only or explicitly persisted.

Core API paths are normalized to `/api/v1/...` in `src/api.ts`. Mocked responses are no longer the default runtime path; use Playwright route mocks in tests when a deterministic mocked scenario is needed.

## Troubleshooting

### Port 8080 is already in use

```bash
npm run dev -- --port 9000
```

Webpack dev server will bind to the next available port, or specify one explicitly with `--port`.

### Playwright browser not found

```bash
npx playwright install chromium
```

This downloads the Chromium binary used by Playwright tests.

### Hot reload isn't working

Check that webpack-dev-server is running (it should print the URL). If you edited a file and the page didn't update, try:
- Hard refresh (Ctrl+Shift+R or Cmd+Shift+R)
- Restart `npm run dev`

### "Connection refused" when pointing at a real lemond

Verify the server is running (`lemond` or `lemonade launch`), check the URL in the Connect screen, and ensure no firewall is blocking the port. Try `curl http://localhost:13305/api/v1/health` from the terminal, or replace the port with your configured server URL.

### Test timeouts or failures

Playwright waits up to 60 seconds by default (see `playwright.config.ts`). If tests time out:
- Check that `npm run dev` is running on port 8080
- Verify network connectivity (especially for real-server tests)
- Run `npm run test:headed` to see what the browser is actually doing

## Design & Architecture Notes

- **Single codebase, dual delivery:** The same React source powers both web-served and desktop (Tauri) builds. Platform-specific code uses feature detection, not separate branches.
- **Real-server-first development:** Runtime calls go to Lemonade-compatible `/api/v1/...` endpoints. Tests that need deterministic data should mock those network routes explicitly.
- **Local client state:** Per-client settings live in browser storage only when the UI explicitly opts into persistence. The Connect screen can clear all Lemonade local/session data.
- **Presets are client-side:** Presets are not persisted to the server; they're computed locally based on the model registry and user adjustments.

## Next Steps

To integrate this prototype into the main codebase:

1. Coordinate with Kyle and the team on the next milestone (web app only vs. Tauri desktop first)
2. Move approved UI components to `src/web-app/` or `src/app/` as appropriate
3. Keep API calls aligned with the finalized `/api/v1/...` server contract and add route-level mocks only for deterministic tests
4. Update the main `CMakeLists.txt` build targets and Web app webpack if needed

See [`.squad/decisions.md`](../../.squad/decisions.md) and [`.squad/agents/mattingly/history.md`](../../.squad/agents/mattingly/history.md) for the full decision trail and learnings.
