// Tauri shim: installs `window.api` on the renderer so the existing React
// renderer (originally written against Electron's contextBridge) keeps working
// unchanged.
//
// When running inside Tauri, we detect `window.__TAURI_INTERNALS__` and bind
// each `window.api.*` method to the corresponding Tauri `invoke()` / event
// `listen()` call. In pure-web mode (served by lemond's HTTP server), this
// module does nothing — the server-injected mock in src/cpp/server/server.cpp
// wins. The dynamic imports below are aliased to a no-op stub by
// src/web-app/webpack.config.js so the web-app build doesn't drag in
// @tauri-apps/* packages.
//
// Event channel names mirror the constants in src/app/src-tauri/src/events.rs.
// Keep them in sync.

type NavData = { view?: string; model?: string };

const EVT_SETTINGS_UPDATED = 'settings-updated';
const EVT_CONNECTION_SETTINGS_UPDATED = 'connection-settings-updated';
const EVT_SERVER_PORT_UPDATED = 'server-port-updated';
const EVT_MAXIMIZE_CHANGE = 'maximize-change';
const EVT_NAVIGATE = 'navigate';

declare global {
  interface Window {
    __TAURI_INTERNALS__?: unknown;
  }
}

function isTauri(): boolean {
  return typeof window !== 'undefined' && typeof window.__TAURI_INTERNALS__ !== 'undefined';
}

async function installTauriApi(): Promise<void> {
  const { invoke } = await import('@tauri-apps/api/core');
  const { listen } = await import('@tauri-apps/api/event');
  const { getCurrentWindow } = await import('@tauri-apps/api/window');
  const { openUrl } = await import('@tauri-apps/plugin-opener');
  const { writeText } = await import('@tauri-apps/plugin-clipboard-manager');

  // Fire-and-forget invoke wrapper used by every void window-control method.
  // The .catch logs the failure with the command name for debuggability.
  const fire = (cmd: string, args?: Record<string, unknown>) => () => {
    invoke(cmd, args).catch((e) => console.warn(cmd, e));
  };

  // Subscribe to a Tauri event, matching the Electron callback shape (handler
  // receives just the payload, not the whole event envelope).
  //
  // The `cancelled` flag handles the unmount-before-listen-resolves race: if
  // the caller invokes the returned cleanup before `listen()` has returned its
  // unlisten handle, we set the flag and run the unlisten immediately when the
  // promise eventually resolves. Without this, React effects that mount and
  // unmount synchronously would leak the underlying subscription.
  function on<T>(channel: string, cb: (payload: T) => void): () => void {
    let unlisten: (() => void) | null = null;
    let cancelled = false;
    listen<T>(channel, (event) => cb(event.payload)).then((fn) => {
      if (cancelled) {
        fn();
      } else {
        unlisten = fn;
      }
    });
    return () => {
      cancelled = true;
      if (unlisten) {
        unlisten();
        unlisten = null;
      }
    };
  }

  // Resolved synchronously at install time so consumers can read it as a
  // plain string field rather than a promise.
  let platformCache = 'unknown';
  try {
    platformCache = (await invoke<string>('get_platform')) || 'unknown';
  } catch (err) {
    console.warn('get_platform failed', err);
  }

  const api = {
    isWebApp: false,
    platform: platformCache,

    minimizeWindow: fire('minimize_window'),
    maximizeWindow: fire('maximize_window'),
    closeWindow: fire('close_window'),
    zoomIn: fire('zoom_in'),
    zoomOut: fire('zoom_out'),
    updateMinWidth: (width: number) => fire('update_min_width', { width })(),

    onMaximizeChange: (callback: (isMaximized: boolean) => void) => {
      // Prime with current state, then subscribe.
      getCurrentWindow().isMaximized().then(callback).catch(() => {});
      return on<boolean>(EVT_MAXIMIZE_CHANGE, callback);
    },

    // Frameless-window edge resize. webkit2gtk does not draw resize handles on
    // borderless windows, so the renderer paints its own invisible regions and
    // calls this on mousedown. `getCurrentWindow().startResizeDragging` accepts
    // the same direction strings the renderer uses (Left, TopRight, etc.).
    startResizeDragging: (direction: string) => {
      getCurrentWindow()
        // The Tauri API uses a tagged enum at the type level but accepts the
        // bare string at runtime; cast to keep the shim free of @tauri-apps types.
        .startResizeDragging(direction as never)
        .catch((e: unknown) => console.warn('startResizeDragging', e));
    },

    writeClipboard: async (text: string) => {
      await writeText(String(text));
    },
    openExternal: (url: string) => {
      openUrl(url).catch((e) => console.warn('openExternal', e));
    },

    getSettings: () => invoke('get_app_settings'),
    saveSettings: (settings: unknown) => invoke('save_app_settings', { payload: settings }),
    onSettingsUpdated: (callback: (settings: unknown) => void) =>
      on<unknown>(EVT_SETTINGS_UPDATED, callback),

    discoverServerPort: () => invoke<number | null>('discover_server_port'),
    getServerPort: () => invoke<number>('get_server_port'),
    getServerBaseUrl: () => invoke<string | null>('get_server_base_url'),
    getServerAPIKey: () => invoke<string>('get_server_api_key'),
    onServerPortUpdated: (callback: (port: number) => void) =>
      on<number>(EVT_SERVER_PORT_UPDATED, callback),
    onConnectionSettingsUpdated: (
      callback: (baseURL: string, apiKey: string) => void,
    ) =>
      on<{ base_url: string; api_key: string }>(
        EVT_CONNECTION_SETTINGS_UPDATED,
        (payload) => callback(payload.base_url, payload.api_key),
      ),

    // Server version, system stats, and system info are NOT exposed via
    // window.api anymore. The renderer fetches /health, /system-stats, and
    // /system-info directly via serverConfig.fetch — see StatusBar.tsx and
    // AboutModal.tsx. The old Rust proxy (system_info.rs) was redundant.
    getLocalMarketplaceUrl: () => invoke<string | null>('get_local_marketplace_url'),
    signalReady: fire('renderer_ready'),
    onNavigate: (callback: (data: NavData) => void) => on<NavData>(EVT_NAVIGATE, callback),
  };

  (window as unknown as { api: typeof api }).api = api;
}

if (typeof window !== 'undefined' && isTauri() && !(window as unknown as { api?: unknown }).api) {
  installTauriApi().catch((err) => {
    console.error('Failed to install Tauri API shim', err);
  });
}

export {};
