# Lemonade Build Options

## React App Build Configuration

The CMake build system allows you to control whether the React web app and/or Electron desktop app are built and included in the server.

### Build Options

#### `BUILD_WEB_APP` (Default: **ON**)
Build and include the React web app for browser access via the `/app` endpoint.

- When **ON**: The web app will be automatically built during CMake configuration if not already present
- When **OFF**: The `/app` endpoint will serve a minimal fallback page
- Requires: Node.js and npm

#### `BUILD_ELECTRON_APP` (Default: **OFF**)
Build and include the full Electron desktop application.

- When **ON**: Enables the `electron-app` target for building the packaged desktop app
- When **OFF**: Desktop app targets are disabled
- Requires: Node.js, npm, and Electron dependencies

### Usage Examples

#### 1. Default Configuration (Web App Only)
```bash
cd build
cmake ../src/cpp
ninja lemonade-router
```

Result:
- ✅ Web app available at `http://localhost:8000/app`
- ❌ No Electron desktop app
- Minimal build time and dependencies

#### 2. Server Only (No UI)
```bash
cd build
cmake -DBUILD_WEB_APP=OFF ../src/cpp
ninja lemonade-router
```

Result:
- ❌ No web app
- ❌ No Electron app
- Minimal fallback page at `/app`
- Fastest build time

#### 3. Both Web App and Electron App
```bash
cd build
cmake -DBUILD_WEB_APP=ON -DBUILD_ELECTRON_APP=ON ../src/cpp
ninja lemonade-router
ninja electron-app  # Build desktop app separately
```

Result:
- ✅ Web app at `http://localhost:8000/app`
- ✅ Electron desktop app available
- Both `webapp` and `electron-app` targets enabled

#### 4. Electron App Only
```bash
cd build
cmake -DBUILD_WEB_APP=OFF -DBUILD_ELECTRON_APP=ON ../src/cpp
ninja electron-app
```

Result:
- ❌ No web app
- ✅ Electron desktop app available
- Useful for desktop-only deployments

### Build Targets

When Node.js/npm are available, the following targets are created:

#### `webapp` (when `BUILD_WEB_APP=ON`)
```bash
cmake --build . --target webapp
# or
ninja webapp
```

Builds just the React web app bundle using Webpack, without Electron dependencies. Output goes to `build/resources/app/`.

#### `electron-app` (when `BUILD_ELECTRON_APP=ON`)
```bash
cmake --build . --target electron-app
# or
ninja electron-app
```

Builds the full packaged Electron desktop application. This includes compiling the React app and packaging it with Electron.

### Automatic Building

During CMake configuration, if `BUILD_WEB_APP=ON` or `BUILD_ELECTRON_APP=ON` and the app hasn't been built yet:

1. CMake checks for existing `dist/renderer` directory
2. If not found and Node.js is available, automatically runs:
   - `npm install` (in `src/web-app/` for web builds)
   - `npm run build:renderer:prod`
3. Copies the built app to `build/resources/app/`
4. If build fails or Node.js is missing, creates a fallback HTML page with instructions

### Source Directories

- **Web App**: `src/web-app/`
  - Symlinks to shared source: `src/` → `../app/src/`
  - Minimal dependencies (no Electron)
  - Target: browser (`webpack target: 'web'`)
  - Build output: `src/web-app/dist/renderer/`

- **Electron App**: `src/app/`
  - Full Electron dependencies
  - Target: Electron renderer process
  - Build output: `src/app/dist/renderer/`

### Configuration Status

When running CMake, you'll see:

```
-- === App Build Configuration ===
--   BUILD_WEB_APP: ON
--   BUILD_ELECTRON_APP: OFF
-- React web app target enabled: cmake --build . --target webapp
-- Full Electron app target disabled (BUILD_ELECTRON_APP=OFF)
```

### Requirements

- **Server Only**: No special requirements
- **Web App**: Node.js 18+ and npm
- **Electron App**: Node.js 18+, npm, and Electron dependencies (~600MB)

### Deployment Recommendations

| Use Case | BUILD_WEB_APP | BUILD_ELECTRON_APP | Benefits |
|----------|--------------|-------------------|----------|
| Server deployment | ON | OFF | Web UI accessible from any browser |
| Desktop-only | OFF | ON | Standalone app, no web browser needed |
| Development | ON | OFF | Fast builds, easy testing |
| Full distribution | ON | ON | Both web and desktop access |
| Headless server | OFF | OFF | Minimal footprint, API-only |

### Troubleshooting

**"React app build failed - creating fallback page"**
- Install Node.js from https://nodejs.org/
- Run `npm install` in `src/web-app/`
- Check Node.js version: `node --version` (requires 18+)

**"app build targets disabled"**
- Node.js or npm not found in PATH
- Install Node.js or add it to your PATH

**Both options OFF but still seeing an app?**
- The `/app` endpoint will serve a minimal fallback HTML page with instructions
- This is intentional to guide users on how to enable the full app
