import React, { useMemo, useRef, useEffect } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import 'highlight.js/styles/github-dark.css';
import 'katex/dist/katex.min.css';

interface MarkdownMessageProps {
  content: string;
  isComplete?: boolean;
}

const COPY_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path></svg>`;
const CHECK_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>`;

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content, isComplete = true }) => {
  const containerRef = useRef<HTMLDivElement>(null);

  const md = useMemo(() => {
    const instance = new MarkdownIt({
      html: false,
      linkify: true,
      typographer: true,
      breaks: true,
      highlight(str: string, lang: string) {
        const langLabel = lang || 'text';
        let highlighted: string;
        if (lang && hljs.getLanguage(lang)) {
          try { highlighted = hljs.highlight(str, { language: lang }).value; } catch { highlighted = instance.utils.escapeHtml(str); }
        } else {
          highlighted = instance.utils.escapeHtml(str);
        }
        return `<div class="code-block"><div class="code-block__header"><span class="code-block__lang">${langLabel}</span><button class="code-block__copy" title="Copy">${COPY_ICON}</button></div><pre><code>${highlighted}</code></pre></div>`;
      },
    });

    instance.use(texmath, {
      engine: katex,
      delimiters: 'dollars',
      katexOptions: { throwOnError: false, displayMode: false },
    });

    // Override fence to use our custom highlight wrapper
    instance.renderer.rules.fence = (tokens, idx) => {
      const token = tokens[idx];
      return instance.options.highlight!(token.content, token.info.trim(), '') || '';
    };

    return instance;
  }, []);

  const html = useMemo(() => md.render(content || ''), [md, content]);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    const handleClick = (e: MouseEvent) => {
      const btn = (e.target as HTMLElement).closest('.code-block__copy') as HTMLButtonElement | null;
      if (!btn) return;
      const codeBlock = btn.closest('.code-block');
      const code = codeBlock?.querySelector('code');
      if (!code) return;

      navigator.clipboard.writeText(code.textContent || '').then(() => {
        btn.innerHTML = CHECK_ICON;
        btn.classList.add('copied');
        setTimeout(() => {
          btn.innerHTML = COPY_ICON;
          btn.classList.remove('copied');
        }, 2000);
      });
    };

    container.addEventListener('click', handleClick);
    return () => container.removeEventListener('click', handleClick);
  }, [html]);

  return (
    <div
      ref={containerRef}
      className="message__content"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
};

export default MarkdownMessage;
