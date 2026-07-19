import React, { useState } from 'react';
import { type Trace } from '../../inspectStore';
import MarkdownMessage from '../MarkdownMessage';
import { Icon } from '../Icon';

interface MessagesTabProps {
  selectedTrace: Trace;
  formatTokens: (num: number) => string;
  handleCopyFull: (text: string, label: string) => void;
}

interface MessageCardProps {
  m: Trace['messages'][number];
  idx: number;
  formatTokens: (num: number) => string;
  handleCopyFull: (text: string, label: string) => void;
}

function stripLeadingThinking(content: string): string {
  const match = /^\s*<think>[\s\S]*?<\/think>([\s\S]*)/.exec(content);
  return match ? match[1].trim() : content;
}

function MessageCard({ m, idx, formatTokens, handleCopyFull }: MessageCardProps) {
  const [isCollapsed, setIsCollapsed] = useState(false);
  const [cardRenderMode, setCardRenderMode] = useState<'rendered' | 'raw'>('raw');
  const [thinkingCollapsed, setThinkingCollapsed] = useState(true);

  return (
    <div className={`message-card ${isCollapsed ? 'collapsed' : ''}`}>
      <div className="message-card__header">
        <button
          type="button"
          className="message-card__header-toggle"
          aria-expanded={!isCollapsed}
          onClick={() => setIsCollapsed(!isCollapsed)}
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

        {!isCollapsed && (
          <div className="message-card__toggle-segmented toolbar-segmented">
            <button
              type="button"
              className={cardRenderMode === 'rendered' ? 'active' : ''}
              onClick={() => setCardRenderMode('rendered')}
            >
              Rendered
            </button>
            <button
              type="button"
              className={cardRenderMode === 'raw' ? 'active' : ''}
              onClick={() => setCardRenderMode('raw')}
            >
              Raw
            </button>
          </div>
        )}

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
          {cardRenderMode === 'rendered' ? (
            <div className="fade-in">
              <MarkdownMessage content={m.thinking ? stripLeadingThinking(m.content) : m.content} />
            </div>
          ) : (
            <pre className="raw-text-body fade-in">{m.content}</pre>
          )}

          {m.thinking && (
            <div className="reasoning-block">
              <button
                type="button"
                className="reasoning-block__header"
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  gap: 'var(--space-1)',
                  background: 'none',
                  border: 'none',
                  padding: 0,
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                  textAlign: 'left',
                  width: '100%',
                  outline: 'none'
                }}
                onClick={() => setThinkingCollapsed(!thinkingCollapsed)}
                aria-expanded={!thinkingCollapsed}
              >
                <span style={{
                  display: 'inline-flex',
                  transform: thinkingCollapsed ? 'rotate(-90deg)' : 'none',
                  transition: 'transform 0.15s ease'
                }}>
                  <Icon name="chevron-down" size={10} />
                </span>
                <span>Reasoning Output</span>
              </button>
              {!thinkingCollapsed && (
                <div className="reasoning-block__body fade-in" style={{ marginTop: 'var(--space-2)' }}>
                  {m.thinking}
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

export default function MessagesTab({
  selectedTrace,
  formatTokens,
  handleCopyFull
}: MessagesTabProps) {
  const [viewMode, setViewMode] = useState<'rendered' | 'raw'>('rendered');
  const [copyDropdownOpen, setCopyDropdownOpen] = useState(false);
  const [copyFormat, setCopyFormat] = useState<'messages' | 'openinference'>('messages');

  const copyFormattedPayload = (format: 'messages' | 'openinference') => {
    setCopyFormat(format);
    setCopyDropdownOpen(false);

    if (format === 'messages') {
      const payload = selectedTrace.messages.map((m) => ({
        role: m.role,
        content: m.content,
        ...(m.thinking ? { thinking: m.thinking } : {})
      }));
      handleCopyFull(JSON.stringify(payload, null, 2), 'Messages JSON');
    } else {
      const payload = {
        input: {
          value: selectedTrace.messages.map((m) => ({
            role: m.role,
            content: m.content,
            ...(m.thinking ? { thinking: m.thinking } : {})
          }))
        },
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
    <div id="panel-messages" role="tabpanel" aria-labelledby="tab-messages" className="tab-pane fade-in flex-col gap-12" style={{ height: '100%' }}>
      <div className="messages-toolbar">
        <div className="messages-toolbar__left">
          <span className="messages-toolbar__title">CONVERSATION</span>
          <div className="toolbar-segmented">
            <button
              type="button"
              aria-pressed={viewMode === 'rendered'}
              className={viewMode === 'rendered' ? 'active' : ''}
              onClick={() => setViewMode('rendered')}
            >
              Rendered
            </button>
            <button
              type="button"
              aria-pressed={viewMode === 'raw'}
              className={viewMode === 'raw' ? 'active' : ''}
              onClick={() => setViewMode('raw')}
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

      {viewMode === 'rendered' ? (
        <div className="messages-list">
          {selectedTrace.messages.map((m, idx) => (
            <MessageCard
              key={`${selectedTrace.id}:${idx}`}
              m={m}
              idx={idx}
              formatTokens={formatTokens}
              handleCopyFull={handleCopyFull}
            />
          ))}
        </div>
      ) : (
        <pre
          className="raw-text-body fade-in"
          style={{
            padding: 'var(--space-4)',
            background: 'var(--surface-1)',
            border: '1px solid var(--border-subtle)',
            borderRadius: 'var(--radius-lg)',
            overflow: 'auto',
            fontFamily: 'monospace',
            fontSize: 'var(--text-sm)',
            whiteSpace: 'pre-wrap',
            wordBreak: 'break-all',
            margin: 0,
            flexGrow: 1
          }}
        >
          <code>
            {JSON.stringify(
              selectedTrace.messages.map(m => ({
                role: m.role,
                content: m.content,
                ...(m.thinking ? { thinking: m.thinking } : {}),
                ...(m.tokens ? { tokens: m.tokens } : {})
              })),
              null,
              2
            )}
          </code>
        </pre>
      )}
    </div>
  );
}
