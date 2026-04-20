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

declare global {
  interface Window {
    api: {
      writeClipboard?: (text: string) => Promise<void>;
      isWebApp?: boolean;  // Explicit flag to indicate web mode (vs Tauri desktop)
      platform: string;
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
      getLocalMarketplaceUrl?: () => Promise<string | null>;
      signalReady?: () => void;
      onNavigate?: (callback: (data: { view?: string; model?: string }) => void) => void | (() => void);
    };
  }
}

export {};
