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
