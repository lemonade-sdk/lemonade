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
      theme: 'neutral',
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

/* ── Mermaid Block — proper React component with its own SVG state ── */

const MermaidBlock: React.FC<{ source: string; isComplete: boolean }> = ({ source, isComplete }) => {
  const [svg, setSvg] = useState<string | null>(null);
  const [error, setError] = useState(false);
  const [showSource, setShowSource] = useState(false);

  useEffect(() => {
    if (!isComplete || !source.trim()) return;
    ensureMermaidInit();
    let cancelled = false;

    (async () => {
      try {
        const id = `mm-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`;
        const { svg: rendered } = await mermaid.render(id, source);
        if (!cancelled) setSvg(rendered);
      } catch {
        if (!cancelled) setError(true);
      }
    })();

    return () => { cancelled = true; };
  }, [source, isComplete]);

  const blockClass = svg
    ? 'mermaid-block mermaid-block--rendered'
    : error
      ? 'mermaid-block mermaid-block--error'
      : 'mermaid-block mermaid-block--pending';

  return (
    <div className={blockClass}>
      {!svg && !error && <div className="mermaid-block__loading">Loading diagram…</div>}
      {error && <div className="mermaid-block__error">Diagram syntax error</div>}
      {svg && (
        <div
          className="mermaid-block__diagram"
          style={showSource ? { display: 'none' } : undefined}
          dangerouslySetInnerHTML={{ __html: svg }}
        />
      )}
      <div className="mermaid-block__actions">
        <button
          className="mermaid-block__toggle"
          onClick={() => setShowSource(s => !s)}
        >
          {showSource ? 'Show diagram' : 'Show source'}
        </button>
      </div>
      <pre
        className="mermaid-block__source"
        style={showSource || error ? undefined : { display: 'none' }}
      >
        <code>{source}</code>
      </pre>
    </div>
  );
};

/* ── Segment type for content splitting ── */

interface Segment {
  type: 'text' | 'mermaid';
  content: string;
}

const MERMAID_FENCE = /```mermaid\s*\n([\s\S]*?)```/gi;

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content, isComplete = true, onOptionSelect }) => {
  const containerRef = useRef<HTMLDivElement>(null);

  const md = useMemo(() => {
    const instance = new MarkdownIt({
      html: true,
      linkify: true,
      typographer: true,
      breaks: true,
      highlight(str: string, lang: string) {
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

  // Split content into text and mermaid segments
  const segments = useMemo((): Segment[] => {
    const result: Segment[] = [];
    const c = content || '';
    let last = 0;
    let match: RegExpExecArray | null;
    const re = new RegExp(MERMAID_FENCE.source, 'gi');
    while ((match = re.exec(c)) !== null) {
      if (match.index > last) {
        result.push({ type: 'text', content: c.slice(last, match.index) });
      }
      result.push({ type: 'mermaid', content: match[1] });
      last = match.index + match[0].length;
    }
    if (last < c.length) {
      result.push({ type: 'text', content: c.slice(last) });
    }
    if (result.length === 0 && c) {
      result.push({ type: 'text', content: c });
    }
    return result;
  }, [content]);

  // Render text segments to sanitized HTML
  const segmentHtmls = useMemo(() => {
    const map = new Map<number, string>();
    segments.forEach((seg, i) => {
      if (seg.type === 'text') {
        map.set(i, DOMPurify.sanitize(md.render(seg.content), PURIFY_CONFIG));
      }
    });
    return map;
  }, [md, segments]);

  // Handle copy buttons and option buttons
  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    const handleClick = (e: MouseEvent) => {
      const target = e.target as HTMLElement;

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
  }, [onOptionSelect]);

  return (
    <div ref={containerRef} className="message__content">
      {segments.map((seg, i) =>
        seg.type === 'mermaid' ? (
          <MermaidBlock key={`m-${i}`} source={seg.content} isComplete={isComplete} />
        ) : (
          <div key={`t-${i}`} dangerouslySetInnerHTML={{ __html: segmentHtmls.get(i) || '' }} />
        )
      )}
    </div>
  );
};

export default MarkdownMessage;
