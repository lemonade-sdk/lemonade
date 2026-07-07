import React, { useState } from 'react';
import { type Trace } from '../../inspectStore';
import MarkdownMessage from '../MarkdownMessage';
import { Icon } from '../Icon';

interface MessagesTabProps {
  selectedTrace: Trace;
  formatTokens: (num: number) => string;
  handleCopyFull: (text: string, label: string) => void;
}

export default function MessagesTab({
  selectedTrace,
  formatTokens,
  handleCopyFull
}: MessagesTabProps) {
  // Default to Raw (renderMarkdown = false) for secure-by-default posture on telemetry
  const [renderMarkdown, setRenderMarkdown] = useState(false);
  const [collapsedMessages, setCollapsedMessages] = useState<Record<number, boolean>>({});
  const [copyDropdownOpen, setCopyDropdownOpen] = useState(false);
  const [copyFormat, setCopyFormat] = useState<'messages' | 'openinference'>('messages');

  const copyFormattedPayload = (format: 'messages' | 'openinference') => {
    setCopyFormat(format);
    setCopyDropdownOpen(false);

    if (format === 'messages') {
      const payload = selectedTrace.messages.map((m) => ({ role: m.role, content: m.content }));
      handleCopyFull(JSON.stringify(payload, null, 2), 'Messages JSON');
    } else {
      const payload = {
        input: { value: selectedTrace.messages.map((m) => ({ role: m.role, content: m.content })) },
        llm: {
          model_name: selectedTrace.model,
          usage: {
            prompt_tokens: selectedTrace.prompt,
            completion_tokens: selectedTrace.completion,
            total_tokens: (selectedTrace.prompt || 0) + (selectedTrace.completion || 0)
          }
        },
        output: { value: selectedTrace.output }
      };
      handleCopyFull(JSON.stringify(payload, null, 2), 'OpenInference JSON');
    }
  };

  return (
    <div id="panel-messages" role="tabpanel" aria-labelledby="tab-messages" className="tab-pane fade-in flex-col gap-12">
      <div className="messages-toolbar">
        <div className="messages-toolbar__left">
          <span className="messages-toolbar__title">CONVERSATION</span>
          <div className="toolbar-segmented">
            <button
              type="button"
              aria-pressed={renderMarkdown}
              className={renderMarkdown ? 'active' : ''}
              onClick={() => setRenderMarkdown(true)}
            >
              Rendered
            </button>
            <button
              type="button"
              aria-pressed={!renderMarkdown}
              className={!renderMarkdown ? 'active' : ''}
              onClick={() => setRenderMarkdown(false)}
            >
              Raw
            </button>
          </div>
        </div>

        <div className="split-button" style={{ position: 'relative' }}>
          <button
            type="button"
            className="split-button__action"
            onClick={() => copyFormattedPayload(copyFormat)}
          >
            {copyFormat === 'messages' ? 'Copy - messages[]' : 'Copy - OpenInference'}
          </button>
          <button
            type="button"
            className="split-button__caret"
            onClick={() => setCopyDropdownOpen(!copyDropdownOpen)}
            aria-label="Select copy format"
            aria-haspopup="menu"
            aria-expanded={copyDropdownOpen}
            style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }}
          >
            <Icon name="chevron-down" size={12} />
          </button>
          {copyDropdownOpen && (
            <div className="dropdown-menu" style={{ position: 'absolute', right: 0, top: '100%', zIndex: 10 }}>
              <button
                type="button"
                className={`dropdown-item ${copyFormat === 'messages' ? 'selected' : ''}`}
                onClick={() => copyFormattedPayload('messages')}
              >
                Copy Messages JSON
              </button>
              <button
                type="button"
                className={`dropdown-item ${copyFormat === 'openinference' ? 'selected' : ''}`}
                onClick={() => copyFormattedPayload('openinference')}
              >
                Copy OpenInference JSON
              </button>
            </div>
          )}
        </div>
      </div>

      <div className="messages-list">
        {selectedTrace.messages.map((m, idx) => {
          const isCollapsed = !!collapsedMessages[idx];
          return (
            <div key={idx} className={`message-card ${isCollapsed ? 'collapsed' : ''}`}>
              <div className="message-card__header">
                <button
                  type="button"
                  className="message-card__header-toggle"
                  aria-expanded={!isCollapsed}
                  onClick={() => {
                    setCollapsedMessages((prev) => ({
                      ...prev,
                      [idx]: !prev[idx],
                    }));
                  }}
                >
                  <span className={`message-card__caret ${isCollapsed ? 'collapsed' : ''}`} aria-hidden="true">
                    <Icon name="chevron-down" size={12} />
                  </span>
                  <span className={`message-card__role ${m.role}`}>
                    {m.role.toUpperCase()}
                  </span>
                  {m.redacted && <span className="redacted-pill">REDACTED</span>}
                  {m.tokens && <span className="tokens-badge">{formatTokens(m.tokens)}</span>}
                </button>
                <button
                  type="button"
                  className="message-card__copy-btn"
                  onClick={() => {
                    handleCopyFull(m.content, 'Message text');
                  }}
                  title="Copy message text"
                  aria-label={`Copy message ${idx + 1} (${m.role}) content`}
                >
                  <Icon name="copy" size={13} />
                </button>
              </div>

              {!isCollapsed && (
                <div className="message-card__body">
                  {renderMarkdown ? (
                    <div className="fade-in">
                      <MarkdownMessage content={m.content} />
                    </div>
                  ) : (
                    <pre className="raw-text-body fade-in">{m.content}</pre>
                  )}

                  {m.thinking && (
                    <div className="reasoning-block">
                      <div className="reasoning-block__header">Reasoning Output</div>
                      <div className="reasoning-block__body">{m.thinking}</div>
                    </div>
                  )}
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}
