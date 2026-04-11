// Tauri shim: installs `window.api` on the renderer so the existing React code
// (written against the Electron contextBridge surface) keeps working unchanged.
//
// When running inside Tauri, we detect `window.__TAURI_INTERNALS__` and bind each
// `window.api.*` method to the corresponding Tauri `invoke()` / event `listen()`
// call. When running in the browser (pure web mode, served by lemond's HTTP
// server), this module does nothing — the server-injected mock in
// src/cpp/server/server.cpp wins.
//
// Intentionally no external imports in the browser branch: if `window.api` was
// already set by the server-side mock, we leave it alone so we don't trip the
// web-app integration.

type NavData = { view?: string; model?: string };

declare global {
  interface Window {
    __TAURI_INTERNALS__?: unknown;
  }
}

function isTauri(): boolean {
  return typeof window !== 'undefined' && typeof window.__TAURI_INTERNALS__ !== 'undefined';
}

async function installTauriApi(): Promise<void> {
  // Dynamic imports keep the @tauri-apps modules in their own webpack chunk
  // so they're only loaded when the shim runs in a Tauri webview. The
  // `src/web-app/` build aliases these modules to a stub (see
  // src/web-app/webpack.config.js) because the web-app's dependency tree
  // intentionally excludes @tauri-apps/*; the isTauri() guard below ensures
  // we never execute the stub path at runtime in pure-web mode.
  const { invoke } = await import('@tauri-apps/api/core');
  const { listen, emit } = await import('@tauri-apps/api/event');
  const { getCurrentWindow } = await import('@tauri-apps/api/window');
  const { openUrl } = await import('@tauri-apps/plugin-opener');
  const { writeText } = await import('@tauri-apps/plugin-clipboard-manager');

  // Resolve platform synchronously-ish via a one-shot invoke at install time.
  // We cache it on the window so accessors are sync.
  let platformCache = 'unknown';
  try {
    platformCache = (await invoke<string>('get_platform')) || 'unknown';
  } catch (err) {
    // Non-fatal
    console.warn('get_platform failed', err);
  }

  // Helper to subscribe to a Tauri event, matching the Electron callback shape
  // (handler receives just the payload, not the whole event envelope).
  function on<T>(channel: string, cb: (payload: T) => void): () => void {
    let unlisten: (() => void) | null = null;
    listen<T>(channel, (event) => cb(event.payload)).then((fn) => {
      unlisten = fn;
    });
    return () => {
      if (unlisten) unlisten();
    };
  }

  const api = {
    isWebApp: false,
    platform: platformCache,

    // --- Window controls ---
    minimizeWindow: () => {
      invoke('minimize_window').catch((e) => console.warn('minimize_window', e));
    },
    maximizeWindow: () => {
      invoke('maximize_window').catch((e) => console.warn('maximize_window', e));
    },
    closeWindow: () => {
      invoke('close_window').catch((e) => console.warn('close_window', e));
    },
    onMaximizeChange: (callback: (isMaximized: boolean) => void) => {
      // Prime with current state, then subscribe.
      getCurrentWindow()
        .isMaximized()
        .then(callback)
        .catch(() => {});
      return on<boolean>('maximize-change', callback);
    },
    updateMinWidth: (width: number) => {
      invoke('update_min_width', { width }).catch((e) =>
        console.warn('update_min_width', e),
      );
    },
    zoomIn: () => {
      invoke('zoom_in').catch((e) => console.warn('zoom_in', e));
    },
    zoomOut: () => {
      invoke('zoom_out').catch((e) => console.warn('zoom_out', e));
    },

    // --- Clipboard / external ---
    writeClipboard: async (text: string) => {
      await writeText(String(text));
    },
    openExternal: (url: string) => {
      openUrl(url).catch((e) => console.warn('openExternal', e));
    },

    // --- Settings ---
    getSettings: () => invoke('get_app_settings'),
    saveSettings: (settings: unknown) => invoke('save_app_settings', { payload: settings }),
    onSettingsUpdated: (callback: (settings: unknown) => void) =>
      on<unknown>('settings-updated', callback),

    // --- Server discovery / connection ---
    getVersion: () => invoke<string>('get_version'),
    discoverServerPort: () => invoke<number | null>('discover_server_port'),
    getServerPort: () => invoke<number>('get_server_port'),
    getServerBaseUrl: () => invoke<string | null>('get_server_base_url'),
    getServerAPIKey: () => invoke<string>('get_server_api_key'),
    onServerPortUpdated: (callback: (port: number) => void) =>
      on<number>('server-port-updated', callback),
    onConnectionSettingsUpdated: (
      callback: (baseURL: string, apiKey: string) => void,
    ) =>
      on<{ base_url: string; api_key: string }>(
        'connection-settings-updated',
        (payload) => callback(payload.base_url, payload.api_key),
      ),

    // --- System info/stats ---
    getSystemStats: () => invoke('get_system_stats'),
    getSystemInfo: () => invoke('get_system_info'),

    // --- Misc ---
    getLocalMarketplaceUrl: () => invoke<string | null>('get_local_marketplace_url'),
    signalReady: () => {
      invoke('renderer_ready').catch((e) => console.warn('renderer_ready', e));
    },
    onNavigate: (callback: (data: NavData) => void) => on<NavData>('navigate', callback),
  };

  // Install globally. If the server already wrote a mock (web mode), we would
  // not reach this function at all — `isTauri()` would be false.
  (window as unknown as { api: typeof api }).api = api;

  // Proactively drain any pending protocol nav that fired before this script loaded.
  try {
    await emit('shim-ready');
  } catch {
    // Ignore — the shim-ready event is a no-op if nothing is listening.
  }
}

if (typeof window !== 'undefined' && isTauri() && !(window as unknown as { api?: unknown }).api) {
  installTauriApi().catch((err) => {
    console.error('Failed to install Tauri API shim', err);
  });
}

export {};
