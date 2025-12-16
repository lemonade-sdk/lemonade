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
      discoverServerPort?: () => Promise<number>;
      getServerPort?: () => Promise<number>;
      onServerPortUpdated?: (callback: (port: number) => void) => void | (() => void);
    };
  }
}

export {};
