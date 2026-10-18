import React, { useMemo, useRef, useEffect, useState } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import '../styles/hljs-styles.css';
import DOMPurify from 'dompurify';
import mermaid from 'mermaid';
import 'katex/dist/katex.min.css';
import { copyTextToClipboard } from '../clipboard';
import { useI18n } from '../i18n';

interface MarkdownMessageProps {
  content: string;
  isComplete?: boolean;
  onOptionSelect?: (text: string) => void;
}

const COPY_LABEL = 'Copy';
const COPIED_LABEL = 'Copied';
const COPY_FAILED_LABEL = 'Copy failed';

interface MermaidFailure {
  name: string;
  message: string;
  excerpt: string;
  stack: string;
  isSyntax: boolean;
}

// Mermaid 9 and 11 may throw parser errors, unsupported-diagram errors, or
// regular JavaScript exceptions. Preserve their actual diagnostics instead of
// labeling every failure a syntax error.
function describeMermaidFailure(cause: unknown, source: string): MermaidFailure {
  const error = cause !== null && typeof cause === 'object'
    ? cause as {
      name?: unknown;
      message?: unknown;
      str?: unknown;
      stack?: unknown;
      hash?: { loc?: { first_line?: number; first_column?: number } };
    }
    : null;
  const name = typeof error?.name === 'string' ? error.name : 'Error';
  const message = typeof error?.message === 'string' && error.message
    ? error.message
    : typeof error?.str === 'string' ? error.str : String(cause);
  const match = message.match(/(?:parse error on|syntax error on|at)\s+line\s+(\d+)/i);
  const reportedLine = error?.hash?.loc?.first_line ?? (match ? Number(match[1]) : undefined);
  const line = typeof reportedLine === 'number' && Number.isInteger(reportedLine)
    ? reportedLine : undefined;
  const lines = source.split(/\r?\n/);
  const excerpt = line !== undefined && line >= 1 && line <= lines.length
    ? lines.slice(Math.max(0, line - 2), Math.min(lines.length, line + 1))
      .map((value, i) => {
        const number = Math.max(0, line - 2) + i + 1;
        return `${number === line ? '>' : ' '} ${number} | ${value}`;
      }).join('\n')
    : '';
  const column = error?.hash?.loc?.first_column;
  const location = line !== undefined
    ? `Line ${line}${typeof column === 'number' ? `, column ${column + 1}` : ''}`
    : '';
  return {
    name,
    message,
    excerpt: location ? `${location}\n${excerpt}` : excerpt,
    stack: typeof error?.stack === 'string' ? error.stack : '',
    isSyntax: Boolean(error?.hash?.loc) || /\b(?:parse|syntax|lexical)\s+error\b/i.test(message),
  };
}

/* ── Mermaid init ──────────────────────────────────────────── */

let mermaidReady = false;
function ensureMermaidInit() {
  if (!mermaidReady) {
    mermaid.initialize({
      startOnLoad: false,
      suppressErrorRendering: true,
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
  const { t } = useI18n();
  const [svg, setSvg] = useState<string | null>(null);
  const [error, setError] = useState<MermaidFailure | null>(null);
  const [copiedDiagnostic, setCopiedDiagnostic] = useState(false);
  const [showSource, setShowSource] = useState(false);

  useEffect(() => {
    setSvg(null);
    setError(null);
    setCopiedDiagnostic(false);
    if (!isComplete || !source.trim()) return;
    let cancelled = false;

    (async () => {
      try {
        ensureMermaidInit();
        const id = `mm-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`;
        const { svg: rendered } = await mermaid.render(id, source);
        if (typeof rendered !== 'string' || !rendered.trim()) {
          throw new Error('Mermaid returned no diagram markup (check installed version and render API)');
        }
        if (!cancelled) setSvg(rendered);
      } catch (cause) {
        if (cancelled) return;
        const failure = describeMermaidFailure(cause, source);
        const diagram = source.trim().split(/\r?\n/, 1)[0];
        console.error(`[Mermaid] ${diagram || 'diagram'}: ${failure.name}: ${failure.message}`, cause);
        setError(failure);
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
      {!svg && !error && <div className="mermaid-block__loading">{t('Loading diagram…')}</div>}
      {error && (
        <div className="mermaid-block__error" role="alert">
          <strong>{error.isSyntax ? t('Diagram syntax error') : t('Diagram rendering failed')}</strong>
          <pre className="mermaid-block__error-details">{`${error.name}: ${error.message}${error.excerpt ? `\n\n${error.excerpt}` : ''}`}</pre>
          <button
            type="button"
            className="mermaid-block__toggle"
            onClick={() => {
              const report = [
                `Mermaid ${error.isSyntax ? 'syntax' : 'render'} failure`,
                `${error.name}: ${error.message}`,
                error.excerpt,
                error.stack,
                `Diagram source:\n${source}`,
              ].filter(Boolean).join('\n\n');
              void copyTextToClipboard(report).then(
                () => setCopiedDiagnostic(true),
                (cause) => console.error('Could not copy Mermaid diagnostics:', cause),
              );
            }}
          >{copiedDiagnostic ? t('Copied') : t('Copy diagnostics')}</button>
        </div>
      )}
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
          {showSource ? t('Show diagram') : t('Show source')}
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
  const { t } = useI18n();
  const containerRef = useRef<HTMLDivElement>(null);

  const md = useMemo(() => {
    const instance = new MarkdownIt({
      html: true,
      linkify: true,
      typographer: true,
      breaks: true,
      highlight(str: string, lang: string): string {
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
              const customPlaceholder: string = instance.utils.escapeHtml(t('Or type your own…'));
              const sendLabel: string = instance.utils.escapeHtml(t('Send'));
              optHtml += `<div class="options-block__custom"><input class="options-block__input" placeholder="${customPlaceholder}" /><button class="options-block__submit">${sendLabel}</button></div>`;
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
        const copyLabel: string = instance.utils.escapeHtml(t(COPY_LABEL));
        const copyCodeLabel: string = instance.utils.escapeHtml(t('Copy code'));
        return `<div class="code-block"><div class="code-block__header"><span class="code-block__lang">${langLabel}</span><button class="code-block__copy" title="${copyLabel}" aria-label="${copyCodeLabel}">${copyLabel}</button></div><pre><code>${highlighted}</code></pre></div>`;
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
  }, [t]);

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
        e.preventDefault();
        e.stopPropagation();
        const codeBlock = copyBtn.closest('.code-block');
        const code = codeBlock?.querySelector('code');
        if (!code) return;

        const codeText = code.textContent || '';
        void copyTextToClipboard(codeText).then(() => {
          copyBtn.textContent = t(COPIED_LABEL);
          copyBtn.title = t(COPIED_LABEL);
          copyBtn.classList.add('copied');
          copyBtn.classList.remove('copy-failed');
        }).catch(() => {
          copyBtn.textContent = t(COPY_FAILED_LABEL);
          copyBtn.title = t('Could not copy code to the clipboard');
          copyBtn.classList.add('copy-failed');
          copyBtn.classList.remove('copied');
        }).finally(() => {
          window.setTimeout(() => {
            copyBtn.textContent = t(COPY_LABEL);
            copyBtn.title = t(COPY_LABEL);
            copyBtn.classList.remove('copied', 'copy-failed');
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
  }, [onOptionSelect, t]);

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

export default React.memo(MarkdownMessage, (previous, next) => (
  previous.content === next.content
  && (previous.isComplete ?? true) === (next.isComplete ?? true)
  && previous.onOptionSelect === next.onOptionSelect
));
