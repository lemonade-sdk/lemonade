declare module '*.svg' {
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

interface Window {
  api: {
    platform: string;
    minimizeWindow: () => void;
    maximizeWindow: () => void;
    closeWindow: () => void;
    openExternal: (url: string) => void;
    onMaximizeChange: (callback: (isMaximized: boolean) => void) => void;
    updateMinWidth: (width: number) => void;
    readUserModels?: () => Promise<Record<string, unknown>>;
    addUserModel?: (payload: {
      name: string;
      checkpoint: string;
      recipe: string;
      mmproj?: string;
      reasoning?: boolean;
      vision?: boolean;
    }) => Promise<unknown>;
    watchUserModels?: (callback: () => void) => void | (() => void);
  };
}
