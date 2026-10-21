import React, { useMemo, useRef, useEffect } from 'react';
import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js';
import texmath from 'markdown-it-texmath';
import katex from 'katex';
import '../styles/hljs-styles.css';
import DOMPurify from 'dompurify';
import 'katex/dist/katex.min.css';
import { copyTextToClipboard } from '../clipboard';
import { useI18n } from '../i18n';

interface MarkdownMessageProps {
  content: string;
  onOptionSelect?: (text: string) => void;
}

const COPY_LABEL = 'Copy';
const COPIED_LABEL = 'Copied';
const COPY_FAILED_LABEL = 'Copy failed';

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
    'data-option', 'placeholder', 'type', 'value',
    // SVG attrs
    'd', 'fill', 'stroke', 'stroke-width', 'viewBox', 'xmlns', 'x', 'y', 'rx', 'ry',
    'cx', 'cy', 'r', 'x1', 'y1', 'x2', 'y2', 'points', 'transform',
    'font-size', 'text-anchor', 'dominant-baseline', 'clip-path', 'marker-end',
    'marker-start', 'stroke-dasharray', 'stroke-linecap', 'stroke-linejoin', 'opacity',
  ],
  ALLOW_DATA_ATTR: true,
};

const MarkdownMessage: React.FC<MarkdownMessageProps> = ({ content, onOptionSelect }) => {
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

  const html = useMemo(
    () => DOMPurify.sanitize(md.render(content || ''), PURIFY_CONFIG),
    [md, content],
  );

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
      <div dangerouslySetInnerHTML={{ __html: html }} />
    </div>
  );
};

export default React.memo(MarkdownMessage, (previous, next) => (
  previous.content === next.content
  && previous.onOptionSelect === next.onOptionSelect
));
