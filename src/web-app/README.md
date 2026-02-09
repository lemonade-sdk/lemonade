# Web App Build Directory

This directory contains a web-only build configuration for the Lemonade React app, optimized for browser deployment without Electron dependencies.

## Structure

This directory uses **symlinks** to share source code with the main Electron app while maintaining separate build configuration:

- `src/` → symlink to `../app/src` (shared React source code)
- `assets/` → symlink to `../app/assets` (shared assets)
- `package.json` - Web-only dependencies (no Electron)
- `webpack.config.js` - Browser-targeted webpack config (`target: 'web'`)
- `tsconfig.json` - TypeScript configuration
- `node_modules/` - Separate build dependency tree (~95MB vs ~300MB with Electron)
- `dist/renderer/` - Build output (copied to `build/resources/app/`)

## Why This Approach?

The web-app directory allows building the React app **without installing Electron**, which:
- Reduces installation size by ~200MB (Electron + electron-builder)
- Speeds up CI/CD pipelines for server deployments
- Maintains separate `package-lock.json` for reproducible web builds
- Avoids file renaming during build process
- Keeps Electron app configuration unchanged

## Building

```bash
cmake --build --preset default web-app
```

## Key Differences from Electron App

| Feature | web-app | app |
|---------|---------|-----|
| Webpack target | `web` | `electron-renderer` |
| Dependencies | No Electron | Includes Electron |
| node_modules size | ~95MB | ~300MB |
| Output | `web-app/dist/renderer/` | `app/dist/renderer/` |
| Purpose | Browser via `/app` endpoint | Desktop application |
| API | Mock `window.api` (injected by server) | Real Electron IPC |

## Webpack Configuration

The `webpack.config.js` differs from the Electron app's configuration:
- `target: 'web'` instead of `target: 'electron-renderer'`
- `resolve.modules` points to web-app's node_modules
- `transpileOnly: true` for faster builds (skips type checking)

## Maintenance

When adding new source files or changing the React app:
- Edit files in `app/src/` - changes are automatically reflected via symlinks
- Update dependencies in both `app/package.json` and `web-app/package.json` as needed
- Electron-specific features should be gated with `if (window.api)` checks
