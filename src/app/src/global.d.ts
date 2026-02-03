import type { AppSettings } from './renderer/utils/appSettings';

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
      isWebApp?: boolean;  // Explicit flag to indicate web mode (vs Electron)
      platform: string;
      minimizeWindow: () => void;
      maximizeWindow: () => void;
      closeWindow: () => void;
      openExternal: (url: string) => void;
      onMaximizeChange: (callback: (isMaximized: boolean) => void) => void;
      updateMinWidth: (width: number) => void;
      zoomIn: () => void;
      zoomOut: () => void;
      getSettings?: () => Promise<AppSettings>;
      saveSettings?: (settings: AppSettings) => Promise<AppSettings>;
      onSettingsUpdated?: (callback: (settings: AppSettings) => void) => void | (() => void);
      getVersion?: () => Promise<string>;
      discoverServerPort?: () => Promise<number | null>;
      getServerPort?: () => Promise<number>;
      // Returns the configured server base URL or null if using localhost discovery
      getServerBaseUrl?: () => Promise<string | null>;
      getServerAPIKey?: () => Promise<string | null>;
      onServerPortUpdated?: (callback: (port: number) => void) => void | (() => void);
      onConnectionSettingsUpdated?: (callback: (baseURL: string, apiKey: string) => void) => void | (() => void);
      getSystemStats?: () => Promise<{ cpu_percent: number | null; memory_gb: number; gpu_percent: number | null; vram_gb: number | null }>;
      getSystemInfo?: () => Promise<{ system: string; os: string; cpu: string; gpus: string[]; gtt_gb?: string; vram_gb?: string }>;
    };
  }
}

export {};
