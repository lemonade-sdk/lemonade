import React, { useState, useRef, useCallback, useEffect } from 'react';
import api, { ChatMessage, ChatCompletionStats, LoadedModel } from '../api';
import MarkdownMessage from './MarkdownMessage';

interface Message {
  role: 'user' | 'assistant';
  content: string;
  thinking?: string;
  stats?: ChatCompletionStats;
}

interface ChatViewProps {
  currentModel: string | null;
  loadedModels: LoadedModel[];
  onModelSelect: (model: string) => void;
  onRefresh: () => void;
}

const ChatView: React.FC<ChatViewProps> = ({ currentModel, loadedModels, onModelSelect, onRefresh }) => {
  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [isStreaming, setIsStreaming] = useState(false);
  const [streamingContent, setStreamingContent] = useState('');
  const [streamingThinking, setStreamingThinking] = useState('');
  const [thinkingExpanded, setThinkingExpanded] = useState(false);
  const [railExpanded, setRailExpanded] = useState(true);
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const abortControllerRef = useRef<AbortController | null>(null);

  const scrollToBottom = useCallback(() => {
    if (threadRef.current) {
      threadRef.current.scrollTop = threadRef.current.scrollHeight;
    }
  }, []);

  useEffect(() => {
    scrollToBottom();
  }, [messages, streamingContent, streamingThinking, scrollToBottom]);

  const handleNewChat = useCallback(() => {
    if (isStreaming && abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
    setMessages([]);
    setIsStreaming(false);
    setStreamingContent('');
    setStreamingThinking('');
    inputRef.current?.focus();
  }, [isStreaming]);

  const handleStop = useCallback(() => {
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
      if (streamingContent || streamingThinking) {
        const partialMessage: Message = {
          role: 'assistant',
          content: streamingContent || '(stopped)',
          thinking: streamingThinking || undefined,
        };
        setMessages(prev => [...prev, partialMessage]);
      }
      setIsStreaming(false);
      setStreamingContent('');
      setStreamingThinking('');
    }
  }, [streamingContent, streamingThinking]);

  const handleSend = async () => {
    const text = inputValue.trim();
    if (!text || isStreaming) return;
    if (!api.isConnected || !currentModel) return;

    const userMessage: Message = { role: 'user', content: text };
    setMessages(prev => [...prev, userMessage]);
    setInputValue('');
    setIsStreaming(true);
    setStreamingContent('');
    setStreamingThinking('');
    setThinkingExpanded(false);

    const controller = new AbortController();
    abortControllerRef.current = controller;

    const chatMessages: ChatMessage[] = [
      ...messages.map(m => ({ role: m.role, content: m.content })),
      { role: 'user' as const, content: text },
    ];

    await api.chatCompletion(currentModel, chatMessages, {
      onReasoning: (_token, fullReasoning) => {
        setStreamingThinking(fullReasoning);
        if (!thinkingExpanded) setThinkingExpanded(true);
      },
      onToken: (_token, full) => {
        setStreamingContent(full);
        setThinkingExpanded(false);
      },
      onDone: (stats) => {
        setMessages(prev => [...prev, {
          role: 'assistant',
          content: stats.content,
          thinking: stats.reasoning || undefined,
          stats,
        }]);
        setIsStreaming(false);
        setStreamingContent('');
        setStreamingThinking('');
        abortControllerRef.current = null;
      },
      onError: (err) => {
        if (err.name === 'AbortError') return;
        setMessages(prev => [...prev, { role: 'assistant', content: `Error: ${err.message}` }]);
        setIsStreaming(false);
        setStreamingContent('');
        setStreamingThinking('');
        abortControllerRef.current = null;
      },
      signal: controller.signal,
    });
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  const hasMessages = messages.length > 0 || isStreaming;

  return (
    <div className={`chat ${railExpanded ? 'rail-expanded' : ''}`}>
      {/* Conversation rail */}
      <aside className="rail">
        <div className="rail__head">
          <button
            className="rail__toggle"
            onClick={() => setRailExpanded(!railExpanded)}
            aria-label="Toggle conversations"
          >
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round">
              <line x1="3" y1="4" x2="13" y2="4" />
              <line x1="3" y1="8" x2="13" y2="8" />
              <line x1="3" y1="12" x2="13" y2="12" />
            </svg>
          </button>
          <span className="rail__title">Conversations</span>
        </div>

        <div className="rail__new-wrap">
          <button className="rail__new" onClick={handleNewChat} aria-label="New chat">
            <span className="rail__new-icon" aria-hidden="true">
              <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                <line x1="7" y1="2.5" x2="7" y2="11.5" />
                <line x1="2.5" y1="7" x2="11.5" y2="7" />
              </svg>
            </span>
            <span className="rail__new-label">New chat</span>
          </button>
        </div>

        <ul className="rail__list" role="listbox">
          {messages.length > 0 && (
            <li className="rail__item is-active" role="option">
              <span className="rail__item-title">
                {messages[0]?.content.slice(0, 40) || 'New conversation'}
              </span>
              <span className="rail__item-meta">
                <span className="rail__model-badge">
                  {(currentModel || '').split('-')[0]?.toLowerCase() || 'llm'}
                </span>
                <span>just now</span>
              </span>
            </li>
          )}
        </ul>
      </aside>

      {/* Main pane */}
      <div className="chat__main" ref={threadRef}>
        <div className="chat__inner">
          {!hasMessages ? (
            <EmptyState
              loadedModels={loadedModels}
              currentModel={currentModel}
              onModelSelect={onModelSelect}
              onChipClick={(text) => setInputValue(text)}
            />
          ) : (
            <div className="thread">
              {messages.map((msg, i) => (
                <MessageBubble key={i} message={msg} currentModel={currentModel} />
              ))}

              {isStreaming && (
                <article className="message message--assistant">
                  <div className="message__avatar">
                    {(currentModel || 'A').charAt(0).toUpperCase()}
                  </div>
                  <div className="message__body">
                    <div className="message__author">{currentModel || 'Assistant'}</div>
                    {streamingThinking && (
                      <details className="message__thinking" open={thinkingExpanded}>
                        <summary>Thinking…</summary>
                        <div className="message__thinking-content">{streamingThinking}</div>
                      </details>
                    )}
                    {streamingContent ? (
                      <MarkdownMessage content={streamingContent} isComplete={false} />
                    ) : !streamingThinking ? (
                      <div className="message__content">
                        <span className="streaming-cursor" aria-hidden="true" />
                      </div>
                    ) : null}
                    {streamingContent && <span className="streaming-cursor" aria-hidden="true" />}
                  </div>
                </article>
              )}
            </div>
          )}
        </div>
      </div>

      {/* Composer */}
      <div className="composer">
        <div className="composer__bar">
          <textarea
            ref={inputRef}
            className="composer__input"
            placeholder={currentModel ? `Message ${currentModel}…` : 'Connect to a server to start…'}
            value={inputValue}
            onChange={e => setInputValue(e.target.value)}
            onKeyDown={handleKeyDown}
            disabled={!currentModel || isStreaming}
            rows={1}
          />
          {isStreaming ? (
            <button className="composer__stop" onClick={handleStop} aria-label="Stop generating" title="Stop">■</button>
          ) : (
            <button
              className="composer__send"
              onClick={handleSend}
              disabled={!inputValue.trim() || !currentModel}
              aria-label="Send"
            >↑</button>
          )}
        </div>
        <div className="composer__hint">⏎ to send · Shift+⏎ for newline</div>
      </div>
    </div>
  );
};

/* ─── Empty state ─────────────────────────────────────── */

interface EmptyStateProps {
  loadedModels: LoadedModel[];
  currentModel: string | null;
  onModelSelect: (model: string) => void;
  onChipClick: (text: string) => void;
}

const EmptyState: React.FC<EmptyStateProps> = ({ loadedModels, currentModel, onModelSelect, onChipClick }) => (
  <>
    <div className="hero">
      <h1 className="hero__title">What's on your mind?</h1>
      <p className="hero__subtitle">
        {loadedModels.length > 0
          ? `${loadedModels.length} model${loadedModels.length > 1 ? 's' : ''} loaded. Pick a thread or start fresh.`
          : 'Connect to a server and load a model to begin.'}
      </p>

      <div className="chips" role="list">
        <button className="chip" role="listitem" onClick={() => onChipClick('Summarize this document for me')}>
          <span className="chip__icon" aria-hidden="true">📄</span>
          Summarize a doc
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Review this code and suggest improvements')}>
          <span className="chip__icon" aria-hidden="true">💻</span>
          Code review
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Explain this concept simply')}>
          <span className="chip__icon" aria-hidden="true">💡</span>
          Explain something
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Help me write a function that')}>
          <span className="chip__icon" aria-hidden="true">⚡</span>
          Write code
        </button>
      </div>
    </div>

    {loadedModels.length > 0 && (
      <>
        <div className="section-label">
          <span>Loaded right now</span>
          <span className="section-label__rule" />
        </div>
        <div className="active-models">
          {loadedModels.map(m => (
            <div className="active-card" key={m.model_name}>
              <div className="active-card__head">
                <div>
                  <div className="active-card__name">{m.model_name}</div>
                  <div className="active-card__meta">{m.recipe} · {m.checkpoint || 'default'}</div>
                </div>
                <span className="active-card__device">{m.device}</span>
              </div>
              <div className="active-card__badges">
                <span className={`cap-badge cap-badge--${m.type === 'llm' ? 'chat' : m.type}`}>{m.type}</span>
              </div>
              {currentModel === m.model_name ? (
                <span className="active-card__status">● Active in chat</span>
              ) : m.type === 'llm' ? (
                <button className="active-card__action" onClick={() => onModelSelect(m.model_name)}>
                  Switch to ▸
                </button>
              ) : null}
            </div>
          ))}
        </div>
      </>
    )}
  </>
);

/* ─── Message bubble ──────────────────────────────────── */

const MessageBubble: React.FC<{ message: Message; currentModel: string | null }> = ({ message, currentModel }) => {
  const [thinkingOpen, setThinkingOpen] = useState(false);

  if (message.role === 'user') {
    return (
      <article className="message message--user">
        <div className="message__avatar">Y</div>
        <div className="message__body">
          <div className="message__author">You</div>
          <div className="message__content"><p>{message.content}</p></div>
        </div>
      </article>
    );
  }

  return (
    <article className="message message--assistant">
      <div className="message__avatar">
        {(currentModel || 'A').charAt(0).toUpperCase()}
      </div>
      <div className="message__body">
        <div className="message__author">{currentModel || 'Assistant'}</div>
        {message.thinking && (
          <details
            className="message__thinking"
            open={thinkingOpen}
            onToggle={e => setThinkingOpen((e.target as HTMLDetailsElement).open)}
          >
            <summary>Thought for {message.stats?.reasoningTokens || '?'} tokens</summary>
            <div className="message__thinking-content">{message.thinking}</div>
          </details>
        )}
        <MarkdownMessage content={message.content} />
        {message.stats && (
          <div className="message__metrics">
            <span>{message.stats.tps} tok/s</span>
            {message.stats.ttft && <span>{(Number(message.stats.ttft) / 1000).toFixed(2)}s TTFT</span>}
            <span>{message.stats.tokens} tokens</span>
          </div>
        )}
      </div>
    </article>
  );
};

export default ChatView;
