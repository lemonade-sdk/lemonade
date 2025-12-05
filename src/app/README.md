# Lemonade Desktop App

A desktop GUI for interacting with the Lemonade Server.

## Overview

This app provides a native desktop experience for managing models and chatting with LLMs running on `lemonade-router`. It connects to the server via HTTP API and offers a modern, resizable panel-based interface.

**Key Features:**
- Model management (list, pull, load/unload)
- Chat interface with markdown/code rendering and LaTeX support
- Real-time server log viewer
- Persistent layout and inference settings
- Custom frameless window with zoom controls

## Code Structure

```
src/app/
├── main.js                 # Electron main process
├── preload.js              # IPC bridge (contextIsolation)
├── webpack.config.js       # Bundler config
├── package.json            # Dependencies and build scripts
│
├── src/
│   └── renderer/           # React UI (TypeScript)
│       ├── App.tsx         # Root component, layout orchestration
│       ├── TitleBar.tsx    # Custom window controls
│       ├── ModelManager.tsx # Model list and actions
│       ├── ChatWindow.tsx  # LLM chat interface
│       ├── LogsWindow.tsx  # Server log viewer
│       ├── CenterPanel.tsx # Welcome/info panel
│       ├── SettingsModal.tsx # Inference parameters
│       └── utils/          # API helpers and config
```

```

## Architecture

```
┌─────────────────────────────────────────────────┐
│         Electron Main Process (main.js)         │
│         Window management, IPC, settings        │
├─────────────────────────────────────────────────┤
│         Preload Script (contextIsolation)       │
│         Exposes safe IPC bridge to renderer     │
├─────────────────────────────────────────────────┤
│         React Renderer (TypeScript)             │
│         UI components, state management         │
├─────────────────────────────────────────────────┤
│         HTTP API                                │
│         Communicates with lemonade-router       │
└─────────────────────────────────────────────────┘
```

## Prerequisites

- **Node.js** v18 or higher
- **npm** (comes with Node.js)

## Building

```bash
cd src/app

# Install dependencies
npm install

# Development (with DevTools)
npm run dev

# Production build
npm run build
```

## Development Scripts

```bash
npm start              # Build and run
npm run dev            # Build and run with DevTools
npm run build          # Production build (creates installer)
npm run build:renderer # Build renderer only (webpack)
npm run watch:renderer # Watch mode for renderer
```
