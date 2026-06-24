# Web App Build Directory

This directory contains a web-only build configuration for the Lemonade React app, optimized for browser deployment without the Tauri host or Rust toolchain.

## Structure

The shared React source lives once in `../app/src/`. This directory only contains the web-app's own build configuration:

- `package.json` - Web-only dependencies (no Tauri, no Rust crates)
- `webpack.config.js` - Browser-targeted webpack config; `entry` and `HtmlWebpackPlugin.template` point at `../app/src/...` directly
- `tsconfig.json` - TypeScript configuration; `include` covers `../app/src/**/*`
- `tauri-stub.js` - No-op stub aliased in for the `@tauri-apps/*` specifiers used by the shared renderer's `tauriShim.ts`
- `BuildWebApp.cmake` - Stages both `src/app/` and `src/web-app/` side-by-side under `build/web-app-staging/` and runs webpack from there
- `node_modules/` - Separate build dependency tree (created on demand by `npm install`, only in non-system-packages mode)
- `dist/renderer/` - Local build output (the CMake target writes to `build/resources/web-app/` instead)

> **Why no symlinks?** Earlier revisions used OS-level symlinks (`src/` → `../app/src`) to share source. Those broke Windows checkouts unless `core.symlinks=true` AND developer mode were both enabled. Webpack's relative `entry`/`template` paths achieve the same result with no checkout-time hazard.

## Why This Approach?

The separation exists because of a **hard invariant**: the native Debian package for `lemond` must build using only npm modules that ship in Debian's repositories (resolved from `/usr/share/nodejs`, `/usr/lib/nodejs`, `/usr/share/javascript`). The old Electron desktop app pulled in dependencies Debian does not package, which made it impossible to build the browser UI from the same `package.json` under distro rules.

This directory's `package.json` is therefore curated to contain **only** dependencies Debian provides, and `webpack.config.js` honors `USE_SYSTEM_NODEJS_MODULES=true` to resolve everything from system module paths when built inside a Debian source package. The `@tauri-apps/*` specifiers the shared renderer imports (for `tauriShim.ts`) are aliased to the no-op `tauri-stub.js` at build time so the web build doesn't require the Tauri CLI or a Rust toolchain either.

**This split is an invariant, not an accident to be tidied up.** `src/web-app/package.json` must not be consolidated with `src/app/package.json` — doing so would break reproducible distro packaging. Additions to the shared renderer that introduce new npm dependencies must either (a) pick a module already in Debian or (b) be placed behind a runtime check and excluded from the web-app code path.

Secondary benefits of the split:
- CI jobs that only need the browser UI don't install the Tauri CLI or Rust
- `lemond` can serve `/app` from any build host without requiring the desktop build
- The `.deb` and `.rpm` server packages ship the browser UI without dragging in Rust

## Building

```bash
cmake --build --preset default --target web-app
```

## Key Differences from the Tauri Desktop App

| Feature | web-app | app (Tauri) |
|---------|---------|-------------|
| Webpack target | `web` | `web` (Tauri uses a standard webview) |
| Dependencies | Node.js + webpack | Node.js + webpack + Rust + webkit2gtk (Linux) |
| Output | `web-app/dist/renderer/` | `app/dist/renderer/` (renderer) + `app/src-tauri/target/release/lemonade-app` (binary) |
| Purpose | Browser via `/app` endpoint | Desktop application |
| window.api | Mock injected by `lemond` (`src/cpp/server/server.cpp`) | Installed by `tauriShim.ts` → Tauri `invoke()` |

Both builds share the same 55+ React files under `src/app/src/renderer/`. The renderer checks `window.api?.isWebApp` to differentiate the two modes at runtime.

## Webpack Configuration

The `webpack.config.js` here differs from the Tauri app's in a few ways, all of which serve the Debian-packaging constraint:
- `resolve.modules` optionally resolves from `/usr/share/nodejs`, `/usr/lib/nodejs`, and `/usr/share/javascript` when `USE_SYSTEM_NODEJS_MODULES=true`
- A KaTeX system-overlay code path is wired in for Debian's packaged `katex` (which does not include the bundled fonts/CSS the shared renderer expects)
- `buffer` and `process/browser` polyfills are resolved lazily (`try { require.resolve(...) }`) because Debian may not ship them
- `resolve.alias` rewrites every `@tauri-apps/*` specifier to `tauri-stub.js` so the web build works without the Tauri packages installed
- `transpileOnly: true` for faster builds (skips type checking)

Since Tauri v2 uses a normal webview (not an Electron renderer), **both** the Tauri app and the web app now use `webpack target: 'web'`.

## Maintenance

When adding new source files or changing the React app:
- Edit files in `src/app/src/` - both builds pick up the change because webpack's `entry`/`template` resolve to that directory directly, and the CMake `web-app` target tracks both trees in its `WEB_APP_SOURCES` glob.
- Update dependencies in both `src/app/package.json` and `src/web-app/package.json` as needed (the split is required for Debian native packaging — see "Why This Approach?" above).
- Tauri-specific features (settings persistence, UDP discovery, window controls) should be gated with `if (window.api && !window.api.isWebApp)` or similar runtime checks.
