// Stub module used only by the src/web-app/ webpack build.
//
// The shared renderer imports @tauri-apps/* modules via dynamic imports in
// src/app/src/renderer/tauriShim.ts. The Tauri desktop app build resolves
// them normally. The web-app build does NOT install @tauri-apps/* packages
// (it's meant to be a minimal browser-only bundle), so its webpack config
// aliases every @tauri-apps/* specifier to this stub.
//
// The stub is unreachable at runtime: tauriShim.ts only calls into these
// modules when `window.__TAURI_INTERNALS__` is present, which it never is in
// pure-web mode. Exporting no-ops keeps webpack happy at build time.

function noop() {}
function asyncNoop() { return Promise.resolve(); }

module.exports = {
  // @tauri-apps/api/core
  invoke: asyncNoop,

  // @tauri-apps/api/event
  listen: () => Promise.resolve(noop),
  emit: asyncNoop,
  once: () => Promise.resolve(noop),

  // @tauri-apps/api/window
  getCurrentWindow: () => ({
    isMaximized: () => Promise.resolve(false),
    minimize: asyncNoop,
    maximize: asyncNoop,
    unmaximize: asyncNoop,
    toggleMaximize: asyncNoop,
    close: asyncNoop,
    setZoom: asyncNoop,
  }),

  // @tauri-apps/plugin-opener
  openUrl: asyncNoop,

  // @tauri-apps/plugin-clipboard-manager
  writeText: asyncNoop,
  readText: () => Promise.resolve(''),
};
