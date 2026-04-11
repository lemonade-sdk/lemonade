# Web App Build Directory

This directory contains a web-only build configuration for the Lemonade React app, optimized for browser deployment without the Tauri host or Rust toolchain.

## Structure

This directory uses **symlinks** to share source code with the Tauri desktop app while maintaining separate build configuration:

- `src/` → symlink to `../app/src` (shared React source code)
- `assets/` → symlink to `../app/assets` (shared assets)
- `package.json` - Web-only dependencies (no Tauri, no Rust crates)
- `webpack.config.js` - Browser-targeted webpack config (`target: 'web'`)
- `tsconfig.json` - TypeScript configuration
- `node_modules/` - Separate build dependency tree
- `dist/renderer/` - Build output (copied to `build/resources/web-app/`)

## Why This Approach?

The web-app directory allows building the React app **without installing the Tauri CLI or a Rust toolchain**, which:
- Reduces dependency surface for CI jobs that only need the browser UI
- Keeps a separate `package-lock.json` for reproducible server-side builds
- Enables `lemond` to serve a full browser UI at `/app` without requiring the desktop build
- Lets the Debian / RPM packages ship the browser UI without dragging in Rust

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

The `webpack.config.js` here differs from the Tauri app's in just a few ways:
- `resolve.modules` points to web-app's local `node_modules`
- `transpileOnly: true` for faster builds (skips type checking)

Since Tauri v2 uses a normal webview (not an Electron renderer), **both** the Tauri app and the web app now use `webpack target: 'web'`.

## Maintenance

When adding new source files or changing the React app:
- Edit files in `src/app/src/` - changes are automatically reflected via symlinks
- Update dependencies in both `src/app/package.json` and `src/web-app/package.json` as needed
- Tauri-specific features (settings persistence, UDP discovery, window controls) should be gated with `if (window.api && !window.api.isWebApp)` or similar runtime checks
