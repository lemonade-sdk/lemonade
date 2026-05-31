# Lemonade UI Redesign Prototype

A **React 19 + TypeScript + webpack** proof-of-concept for the next-generation Lemonade UI. This prototype is built side-by-side with the existing `src/app/` and `src/web-app/` in the main codebase and runs with **mocked APIs by default** (no `lemond` required to demo the UI). It's designed to work both as a web app and as a desktop Tauri application — a single React codebase powering both delivery channels.

**Not production code** — this is an active design and engineering POC on branch `feat/ui-testing`. See [`.squad/decisions.md`](../../.squad/decisions.md) for the design rationale and capability-keyed presets architecture (v1.4).

## Prerequisites

- **Node.js 20+** (check `package.json` `engines` field for exact requirement)
- **npm 10+**
- Optional: a running `lemond` instance if you want to point the prototype at a real server

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

Runs all Playwright tests headless via Chromium. Artifacts are saved to:
- `test-results/` — JUnit XML, JSON, and HTML reports
- `screenshots/` — screenshots on failure

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

## Pointing at a Real Server

By default, the prototype uses mocked APIs (responses without calling `lemond`). To point at a live server:

1. Check `src/api.ts` for the `BASE_URL` constant
2. Adjust it to your server (e.g., `http://localhost:8000`) or use the in-UI settings panel if implemented
3. Optionally set `LEMONADE_API_KEY` in your environment if the server requires authentication

The mock can be toggled in the code without a rebuild — see `src/api.ts` for the `USE_MOCK` flag.

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

Verify the server is running (`lemond` or `lemonade launch`), check the base URL in `api.ts`, and ensure no firewall is blocking the port. Try `curl http://localhost:8000/api/v1/health` from the terminal.

### Test timeouts or failures

Playwright waits up to 60 seconds by default (see `playwright.config.ts`). If tests time out:
- Check that `npm run dev` is running on port 8080
- Verify network connectivity (especially for real-server tests)
- Run `npm run test:headed` to see what the browser is actually doing

## Design & Architecture Notes

- **Single codebase, dual delivery:** The same React source powers both web-served and desktop (Tauri) builds. Platform-specific code uses feature detection, not separate branches.
- **Mocked-first development:** The mock API in `src/api.ts` makes the prototype runnable without `lemond`. Real `lemond` is optional for advanced testing.
- **Local client state:** Per-client settings (base URL, API key, zoom, layout preferences) live in the client's `localStorage` — never on the server.
- **Presets are client-side:** Presets are not persisted to the server; they're computed locally based on the model registry and user adjustments.

## Next Steps

To integrate this prototype into the main codebase:

1. Coordinate with Kyle and the team on the next milestone (web app only vs. Tauri desktop first)
2. Move approved UI components to `src/web-app/` or `src/app/` as appropriate
3. Wire real API calls (remove the mock layer) once the server contract is finalized
4. Update the main `CMakeLists.txt` build targets and Web app webpack if needed

See [`.squad/decisions.md`](../../.squad/decisions.md) and [`.squad/agents/mattingly/history.md`](../../.squad/agents/mattingly/history.md) for the full decision trail and learnings.
