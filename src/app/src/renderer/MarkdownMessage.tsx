import React, { useMemo, useEffect, useRef } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import 'katex/dist/katex.min.css';

interface MarkdownMessageProps {
  content: string;
}

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content }) => {
  const containerRef = useRef<HTMLDivElement>(null);

  const md = useMemo(() => {
    const mdInstance = new MarkdownIt({
      html: false,
      linkify: true,
      typographer: true,
      breaks: true,
      highlight: function (str, lang) {
        if (lang && hljs.getLanguage(lang)) {
          try {
            return hljs.highlight(str, { language: lang }).value;
          } catch (__) {
            // ignore
          }
        }
        return ''; // use external default escaping
      }
    });

    // Add math support with KaTeX
    mdInstance.use(texmath, {
      engine: katex,
      delimiters: 'dollars',
      katexOptions: {
        throwOnError: false,
        displayMode: false
      }
    });

    return mdInstance;
  }, []);

  const htmlContent = useMemo(() => {
    return md.render(content);
  }, [content, md]);

  useEffect(() => {
    // Add click handlers for links to open in external browser
    const container = containerRef.current;
    if (!container) return;

    const handleLinkClick = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (target.tagName === 'A') {
        e.preventDefault();
        const href = target.getAttribute('href');
        if (href && window.api) {
          window.api.openExternal(href);
        }
      }
    };

    container.addEventListener('click', handleLinkClick);
    return () => {
      container.removeEventListener('click', handleLinkClick);
    };
  }, [htmlContent]);

  return (
    <div
      ref={containerRef}
      className="markdown-content"
      dangerouslySetInnerHTML={{ __html: htmlContent }}
    />
  );
};

export default MarkdownMessage;
