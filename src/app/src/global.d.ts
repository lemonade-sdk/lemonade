import type { AppSettings } from './renderer/utils/appSettings';

export type ResizeDirection =
  | 'Left'
  | 'Right'
  | 'Top'
  | 'Bottom'
  | 'TopLeft'
  | 'TopRight'
  | 'BottomLeft'
  | 'BottomRight';

declare module '*.svg' {
  const content: string;
  export default content;
}

declare module '../../assets/*.svg' {
  const content: string;
  export default content;
}

declare module 'markdown-it-texmath' {
  import MarkdownIt from 'markdown-it';

  interface TexmathOptions {
    engine?: any;
    delimiters?: 'dollars' | 'brackets' | 'gitlab' | 'kramdown';
    katexOptions?: any;
  }

  function texmath(md: MarkdownIt, options?: TexmathOptions): void;

  export = texmath;
}

export interface RemoteDevice {
  hostname: string;
  baseUrl: string;
  isLocal: boolean;
}

declare global {
  interface Window {
    api: {
      writeClipboard?: (text: string) => Promise<void>;
      isWebApp?: boolean;  // Explicit flag to indicate web mode (vs Tauri desktop)
      platform: string;
      // OS hostname of THIS machine, populated synchronously at shim install
      // time. Sent as X-Lemonade-Client-Hostname when chatFetch targets a
      // peer Lemonade server so the host owner's authorization prompt has
      // a friendly label to show.
      localHostname?: string;
      minimizeWindow: () => void;
      maximizeWindow: () => void;
      closeWindow: () => void;
      openExternal: (url: string) => void;
      onMaximizeChange: (callback: (isMaximized: boolean) => void) => void;
      updateMinWidth: (width: number) => void;
      zoomIn: () => void;
      zoomOut: () => void;
      // Frameless windows on webkit2gtk get no edge resize handles from the OS,
      // so the renderer paints invisible 6-px regions on each edge/corner and
      // calls this from their mousedown handler. The Tauri shim forwards to
      // `getCurrentWindow().startResizeDragging(direction)`. No-op in web mode.
      startResizeDragging?: (direction: ResizeDirection) => void;
      getSettings?: () => Promise<AppSettings>;
      saveSettings?: (settings: AppSettings) => Promise<AppSettings>;
      onSettingsUpdated?: (callback: (settings: AppSettings) => void) => void | (() => void);
      discoverServerPort?: () => Promise<number | null>;
      getServerPort?: () => Promise<number>;
      // Returns the configured server base URL or null if using localhost discovery
      getServerBaseUrl?: () => Promise<string | null>;
      getServerAPIKey?: () => Promise<string | null>;
      onServerPortUpdated?: (callback: (port: number) => void) => void | (() => void);
      onConnectionSettingsUpdated?: (callback: (baseURL: string, apiKey: string) => void) => void | (() => void);
      // LAN-discovered Lemonade peers. The Tauri host listens for the same
      // UDP beacon `lemond` broadcasts and exposes the registry here so the
      // renderer can offer a "Run on" picker for chat. Returns at minimum the
      // local server (when the listener has seen its own beacon). Web app
      // returns an empty list and never emits updates.
      listRemoteDevices?: () => Promise<RemoteDevice[]>;
      onRemoteDevicesUpdated?: (callback: (devices: RemoteDevice[]) => void) => void | (() => void);
      getLocalMarketplaceUrl?: () => Promise<string | null>;
      signalReady?: () => void;
      onNavigate?: (callback: (data: { view?: string; model?: string }) => void) => void | (() => void);
    };
  }
}

export {};
