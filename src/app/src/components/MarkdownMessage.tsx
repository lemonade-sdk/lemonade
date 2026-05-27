import React, { useMemo, useRef, useEffect, useState } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import DOMPurify from 'dompurify';
import mermaid from 'mermaid';
import 'highlight.js/styles/github-dark.css';
import 'katex/dist/katex.min.css';

interface MarkdownMessageProps {
  content: string;
  isComplete?: boolean;
  onOptionSelect?: (text: string) => void;
}

const COPY_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2-2v1"></path></svg>`;
const CHECK_ICON = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>`;

/* ── Mermaid init ──────────────────────────────────────────── */

let mermaidReady = false;
function ensureMermaidInit() {
  if (!mermaidReady) {
    mermaid.initialize({
      startOnLoad: false,
      theme: 'dark',
      securityLevel: 'strict',
    });
    mermaidReady = true;
  }
}

/* ── DOMPurify config ─────────────────────────────────────── */

const PURIFY_CONFIG: DOMPurify.Config = {
  ALLOWED_TAGS: [
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'br', 'hr',
    'ul', 'ol', 'li', 'dl', 'dt', 'dd',
    'table', 'thead', 'tbody', 'tfoot', 'tr', 'th', 'td', 'caption', 'colgroup', 'col',
    'div', 'span', 'a', 'img',
    'strong', 'em', 'b', 'i', 'u', 's', 'del', 'ins', 'mark', 'sub', 'sup', 'small',
    'blockquote', 'pre', 'code', 'kbd', 'var', 'samp',
    'details', 'summary', 'figure', 'figcaption',
    'abbr', 'cite', 'q', 'time', 'ruby', 'rt', 'rp',
    // Interactive options
    'button', 'input',
    // SVG basics for inline diagrams
    'svg', 'path', 'circle', 'rect', 'line', 'polyline', 'polygon', 'text', 'g', 'defs',
    'use', 'clipPath', 'marker', 'pattern', 'foreignObject', 'tspan',
  ],
  ALLOWED_ATTR: [
    'class', 'id', 'style', 'href', 'target', 'rel', 'src', 'alt', 'title', 'width', 'height',
    'colspan', 'rowspan', 'scope', 'align', 'valign',
    'open', 'datetime', 'cite',
    'data-option', 'data-mermaid-toggle', 'placeholder', 'type', 'value',
    // SVG attrs
    'd', 'fill', 'stroke', 'stroke-width', 'viewBox', 'xmlns', 'x', 'y', 'rx', 'ry',
    'cx', 'cy', 'r', 'x1', 'y1', 'x2', 'y2', 'points', 'transform',
    'font-size', 'text-anchor', 'dominant-baseline', 'clip-path', 'marker-end',
    'marker-start', 'stroke-dasharray', 'stroke-linecap', 'stroke-linejoin', 'opacity',
  ],
  ALLOW_DATA_ATTR: true,
};

/* Module-level counter — reset before each md.render() call */
let mermaidBlockCount = 0;

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content, isComplete = true, onOptionSelect }) => {
  const containerRef = useRef<HTMLDivElement>(null);
  const [mermaidSvgs, setMermaidSvgs] = useState<Record<number, string>>({});

  const md = useMemo(() => {
    const instance = new MarkdownIt({
      html: true,
      linkify: true,
      typographer: true,
      breaks: true,
      highlight(str: string, lang: string) {
        // Mermaid blocks get a placeholder div with an index marker
        if (lang.toLowerCase() === 'mermaid') {
          const idx = mermaidBlockCount++;
          const escaped = instance.utils.escapeHtml(str);
          return `<div class="mermaid-block mermaid-block--pending" data-mermaid-idx="${idx}"><div class="mermaid-block__loading">Loading diagram…</div><div class="mermaid-block__actions"><button class="mermaid-block__toggle" data-mermaid-toggle>Show source</button></div><pre class="mermaid-block__source" style="display:none"><code>${escaped}</code></pre></div>`;
        }

        // Options blocks get rendered as interactive buttons
        if (lang === 'options') {
          try {
            const parsed = JSON.parse(str);
            const question = parsed.question || '';
            const choices: string[] = parsed.choices || parsed.options || [];
            let optHtml = '<div class="options-block">';
            if (question) optHtml += `<div class="options-block__question">${instance.utils.escapeHtml(question)}</div>`;
            optHtml += '<div class="options-block__choices">';
            for (const choice of choices) {
              optHtml += `<button class="options-block__btn" data-option="${instance.utils.escapeHtml(choice)}">${instance.utils.escapeHtml(choice)}</button>`;
            }
            optHtml += '</div>';
            if (parsed.allowCustom !== false) {
              optHtml += '<div class="options-block__custom"><input class="options-block__input" placeholder="Or type your own…" /><button class="options-block__submit">Send</button></div>';
            }
            optHtml += '</div>';
            return optHtml;
          } catch {
            // Invalid JSON — fall through to normal code block
          }
        }

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

  // Base HTML from markdown (with mermaid loading placeholders)
  const baseHtml = useMemo(() => {
    mermaidBlockCount = 0;
    const raw = md.render(content || '');
    return DOMPurify.sanitize(raw, PURIFY_CONFIG);
  }, [md, content]);

  // Extract mermaid source blocks from content
  const mermaidSources = useMemo(() => {
    const sources: string[] = [];
    const regex = /```mermaid\s*\n([\s\S]*?)```/gi;
    let match;
    while ((match = regex.exec(content || '')) !== null) {
      sources.push(match[1]);
    }
    return sources;
  }, [content]);

  // Render mermaid sources to SVGs (async, stored in state)
  useEffect(() => {
    if (!isComplete || mermaidSources.length === 0) {
      if (Object.keys(mermaidSvgs).length > 0) setMermaidSvgs({});
      return;
    }
    ensureMermaidInit();
    let cancelled = false;

    (async () => {
      const newSvgs: Record<number, string> = {};
      for (let i = 0; i < mermaidSources.length; i++) {
        if (cancelled) break;
        try {
          const id = `mm-${Date.now()}-${i}`;
          const { svg } = await mermaid.render(id, mermaidSources[i]);
          newSvgs[i] = svg;
        } catch {
          // Mark as error — will be shown as error block
          newSvgs[i] = '__ERROR__';
        }
      }
      if (!cancelled) setMermaidSvgs(newSvgs);
    })();

    return () => { cancelled = true; };
  }, [mermaidSources, isComplete]);

  // Final HTML: inject rendered SVGs into the sanitized HTML (survives re-renders)
  const html = useMemo(() => {
    if (Object.keys(mermaidSvgs).length === 0) return baseHtml;

    let result = baseHtml;
    for (const [idxStr, svg] of Object.entries(mermaidSvgs)) {
      const idx = idxStr;
      if (svg === '__ERROR__') {
        // Replace loading with error message, show source, remove --pending
        result = result.replace(
          new RegExp(`(<div[^>]*data-mermaid-idx="${idx}"[^>]*class="[^"]*?)mermaid-block--pending([^"]*"[^>]*>)\\s*<div class="mermaid-block__loading">Loading diagram…</div>`),
          `$1mermaid-block--error$2<div class="mermaid-block__error">Diagram syntax error</div>`
        );
        // Show source block
        result = result.replace(
          new RegExp(`(data-mermaid-idx="${idx}"[\\s\\S]*?)<pre class="mermaid-block__source" style="display:none">`),
          `$1<pre class="mermaid-block__source">`
        );
      } else {
        // Replace loading with SVG diagram, swap --pending for --rendered
        result = result.replace(
          new RegExp(`(<div[^>]*data-mermaid-idx="${idx}"[^>]*class="[^"]*?)mermaid-block--pending([^"]*"[^>]*>)\\s*<div class="mermaid-block__loading">Loading diagram…</div>`),
          `$1mermaid-block--rendered$2<div class="mermaid-block__diagram">${svg}</div>`
        );
      }
    }
    return result;
  }, [baseHtml, mermaidSvgs]);

  // Handle copy buttons and option buttons
  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    const handleClick = (e: MouseEvent) => {
      const target = e.target as HTMLElement;

      // Mermaid source toggle
      const toggleBtn = target.closest('[data-mermaid-toggle]') as HTMLButtonElement | null;
      if (toggleBtn) {
        const block = toggleBtn.closest('.mermaid-block');
        if (!block) return;
        const source = block.querySelector('.mermaid-block__source') as HTMLElement;
        const diagram = block.querySelector('.mermaid-block__diagram') as HTMLElement;
        if (!source) return;
        const showing = source.style.display !== 'none';
        source.style.display = showing ? 'none' : '';
        if (diagram) diagram.style.display = showing ? '' : 'none';
        toggleBtn.textContent = showing ? 'Show source' : 'Show diagram';
        return;
      }

      // Copy button
      const copyBtn = target.closest('.code-block__copy') as HTMLButtonElement | null;
      if (copyBtn) {
        const codeBlock = copyBtn.closest('.code-block');
        const code = codeBlock?.querySelector('code');
        if (!code) return;
        navigator.clipboard.writeText(code.textContent || '').then(() => {
          copyBtn.innerHTML = CHECK_ICON;
          copyBtn.classList.add('copied');
          setTimeout(() => {
            copyBtn.innerHTML = COPY_ICON;
            copyBtn.classList.remove('copied');
          }, 2000);
        });
        return;
      }

      // Option button
      const optBtn = target.closest('.options-block__btn') as HTMLButtonElement | null;
      if (optBtn && onOptionSelect) {
        onOptionSelect(optBtn.getAttribute('data-option') || optBtn.textContent || '');
        return;
      }

      // Custom option submit
      const submitBtn = target.closest('.options-block__submit') as HTMLButtonElement | null;
      if (submitBtn && onOptionSelect) {
        const input = submitBtn.parentElement?.querySelector('.options-block__input') as HTMLInputElement | null;
        if (input?.value.trim()) {
          onOptionSelect(input.value.trim());
          input.value = '';
        }
        return;
      }
    };

    // Handle Enter key in custom option input
    const handleKeyDown = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (target.classList.contains('options-block__input') && e.key === 'Enter' && onOptionSelect) {
        const input = target as HTMLInputElement;
        if (input.value.trim()) {
          onOptionSelect(input.value.trim());
          input.value = '';
        }
      }
    };

    container.addEventListener('click', handleClick);
    container.addEventListener('keydown', handleKeyDown);
    return () => {
      container.removeEventListener('click', handleClick);
      container.removeEventListener('keydown', handleKeyDown);
    };
  }, [html, onOptionSelect]);

  return (
    <div
      ref={containerRef}
      className="message__content"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
};

export default MarkdownMessage;
