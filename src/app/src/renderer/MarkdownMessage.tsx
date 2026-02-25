import React, { useMemo, useEffect, useRef } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import 'katex/dist/katex.min.css';

interface MarkdownMessageProps {
  content: string;
  isComplete?: boolean;
}

const COPY_ICON_SVG = `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path></svg>`;

const CHECK_ICON_SVG = `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>`;

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content, isComplete = true }) => {
  const containerRef = useRef<HTMLDivElement>(null);
  const copyTimeoutIdRef = useRef<NodeJS.Timeout | null>(null);

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
    const rendered = md.render(content);

    if (!isComplete) {
      return rendered;
    }

    // Use DOM manipulation to wrap each <pre> block with a wrapper div and add copy button
    const tempContainer = document.createElement('div');
    tempContainer.innerHTML = rendered;

    const preElements = tempContainer.querySelectorAll('pre');
    preElements.forEach((pre) => {
      const wrapper = document.createElement('div');
      wrapper.className = 'code-block-wrapper';

      const button = document.createElement('button');
      button.className = 'code-copy-button';
      button.title = 'Copy code';
      button.innerHTML = COPY_ICON_SVG;

      const parent = pre.parentNode;
      if (!parent) {
        return;
      }

      parent.insertBefore(wrapper, pre);
      wrapper.appendChild(button);
      wrapper.appendChild(pre);
    });

    const result = tempContainer.innerHTML;
    // Clean up tempContainer to help GC
    tempContainer.innerHTML = '';
    return result;
  }, [content, md, isComplete]);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    // Add click handlers for links to open in external browser
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

    // Add click handlers for copy buttons
    const handleCopyClick = async (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      const button = target.closest('.code-copy-button') as HTMLButtonElement;
      if (!button) return;

      const wrapper = button.closest('.code-block-wrapper');
      const preElement = wrapper?.querySelector('pre');
      const codeElement = preElement?.querySelector('code') as HTMLElement | null;
      if (!codeElement) return;
      const codeText = codeElement.textContent || '';

      try {
        await navigator.clipboard.writeText(codeText);
        button.innerHTML = CHECK_ICON_SVG;
        button.classList.add('copied');
        
        // Clear any existing timeout before setting a new one
        if (copyTimeoutIdRef.current) {
          clearTimeout(copyTimeoutIdRef.current);
        }
        
        copyTimeoutIdRef.current = setTimeout(() => {
          // Check if button still exists in DOM before modifying
          if (button.isConnected) {
            button.innerHTML = COPY_ICON_SVG;
            button.classList.remove('copied');
          }
          copyTimeoutIdRef.current = null;
        }, 2000);
      } catch (err) {
        console.error('Failed to copy code:', err);
      }
    };

    container.addEventListener('click', handleLinkClick);
    container.addEventListener('click', handleCopyClick);
    return () => {
      container.removeEventListener('click', handleLinkClick);
      container.removeEventListener('click', handleCopyClick);
      // Clean up the timeout when component unmounts
      if (copyTimeoutIdRef.current) {
        clearTimeout(copyTimeoutIdRef.current);
      }
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
