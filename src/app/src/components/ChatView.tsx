import React, { useState, useRef, useCallback, useEffect } from 'react';
import api, { ChatMessage, ChatCompletionStats, LoadedModel } from '../api';
import MarkdownMessage from './MarkdownMessage';

interface Message {
  role: 'user' | 'assistant';
  content: string;
  thinking?: string;
  stats?: ChatCompletionStats;
}

interface Conversation {
  id: string;
  title: string;
  model: string | null;
  messages: Message[];
  updatedAt: number;
}

const STORAGE_KEY = 'lemonade_conversations';
const ACTIVE_KEY = 'lemonade_active_conversation';

function generateId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function loadConversations(): Conversation[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (Array.isArray(parsed)) return parsed;
    }
  } catch { /* ignore */ }
  return [];
}

function saveConversations(convos: Conversation[]) {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(convos)); } catch { /* ignore */ }
}

function loadActiveId(): string | null {
  try { return localStorage.getItem(ACTIVE_KEY); } catch { return null; }
}

function saveActiveId(id: string | null) {
  try {
    if (id) localStorage.setItem(ACTIVE_KEY, id);
    else localStorage.removeItem(ACTIVE_KEY);
  } catch { /* ignore */ }
}

function deriveTitle(messages: Message[]): string {
  const first = messages.find(m => m.role === 'user');
  if (!first) return 'New conversation';
  const text = first.content.slice(0, 50);
  return text.length < first.content.length ? text + '…' : text;
}

function timeAgo(ts: number): string {
  const diff = Date.now() - ts;
  if (diff < 60000) return 'just now';
  if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
  if (diff < 86400000) return `${Math.floor(diff / 3600000)}h ago`;
  return `${Math.floor(diff / 86400000)}d ago`;
}

interface ChatViewProps {
  currentModel: string | null;
  loadedModels: LoadedModel[];
  onModelSelect: (model: string) => void;
  onRefresh: () => void;
}

const ChatView: React.FC<ChatViewProps> = ({ currentModel, loadedModels, onModelSelect, onRefresh }) => {
  const [conversations, setConversations] = useState<Conversation[]>(loadConversations);
  const [activeId, setActiveId] = useState<string | null>(loadActiveId);
  const [inputValue, setInputValue] = useState('');
  // Per-conversation streaming state: { [convoId]: { content, thinking } }
  const [activeStreams, setActiveStreams] = useState<Record<string, { content: string; thinking: string }>>({});
  const [thinkingExpanded, setThinkingExpanded] = useState(false);
  const [railExpanded, setRailExpanded] = useState(true);
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const controllersRef = useRef<Map<string, AbortController>>(new Map());
  const thinkingContentRef = useRef<HTMLDivElement>(null);
  const thinkingSticky = useRef(true);

  // Derived: is the CURRENT conversation streaming?
  const currentStream = activeId ? activeStreams[activeId] : undefined;
  const isStreaming = !!currentStream;
  const streamingContent = currentStream?.content || '';
  const streamingThinking = currentStream?.thinking || '';
  const streamingConvoIds = new Set(Object.keys(activeStreams));

  const activeConvo = conversations.find(c => c.id === activeId) || null;
  const messages = activeConvo?.messages || [];

  // Persist conversations to localStorage whenever they change
  useEffect(() => {
    saveConversations(conversations);
  }, [conversations]);

  // Persist active conversation id
  useEffect(() => {
    saveActiveId(activeId);
  }, [activeId]);

  const updateConversation = useCallback((id: string, updater: (c: Conversation) => Conversation) => {
    setConversations(prev => prev.map(c => c.id === id ? updater(c) : c));
  }, []);

  const scrollToBottom = useCallback(() => {
    if (threadRef.current) {
      threadRef.current.scrollTop = threadRef.current.scrollHeight;
    }
  }, []);

  useEffect(() => {
    scrollToBottom();
  }, [messages, streamingContent, streamingThinking, scrollToBottom]);

  // Auto-scroll the thinking content box when sticky
  useEffect(() => {
    const el = thinkingContentRef.current;
    if (el && thinkingSticky.current) {
      el.scrollTop = el.scrollHeight;
    }
  }, [streamingThinking]);

  const handleThinkingScroll = useCallback(() => {
    const el = thinkingContentRef.current;
    if (!el) return;
    // "At bottom" = within 8px of the end
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 8;
    thinkingSticky.current = atBottom;
  }, []);

  const handleNewChat = useCallback(() => {
    setActiveId(null);
    inputRef.current?.focus();
  }, []);

  const handleSelectConversation = useCallback((id: string) => {
    setActiveId(id);
  }, []);

  const handleDeleteConversation = useCallback((e: React.MouseEvent, id: string) => {
    e.stopPropagation();
    setConversations(prev => prev.filter(c => c.id !== id));
    if (activeId === id) setActiveId(null);
  }, [activeId]);

  const handleStop = useCallback(() => {
    if (!activeId) return;
    const controller = controllersRef.current.get(activeId);
    if (!controller) return;
    const stream = activeStreams[activeId];

    controller.abort();
    controllersRef.current.delete(activeId);

    if (stream && (stream.content || stream.thinking)) {
      const partialMessage: Message = {
        role: 'assistant',
        content: stream.content || '(stopped)',
        thinking: stream.thinking || undefined,
      };
      updateConversation(activeId, c => ({
        ...c,
        messages: [...c.messages, partialMessage],
        updatedAt: Date.now(),
      }));
    }

    setActiveStreams(prev => {
      const next = { ...prev };
      delete next[activeId!];
      return next;
    });
  }, [activeId, activeStreams, updateConversation]);

  const handleSend = async () => {
    const text = inputValue.trim();
    if (!text || isStreaming) return;
    if (!api.isConnected || !currentModel) return;

    let convoId = activeId;

    // Create a new conversation if none is active
    if (!convoId) {
      const newConvo: Conversation = {
        id: generateId(),
        title: text.slice(0, 50) + (text.length > 50 ? '…' : ''),
        model: currentModel,
        messages: [],
        updatedAt: Date.now(),
      };
      convoId = newConvo.id;
      setConversations(prev => [newConvo, ...prev]);
      setActiveId(convoId);
    }

    const userMessage: Message = { role: 'user', content: text };
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, userMessage],
      model: currentModel,
      updatedAt: Date.now(),
    }));

    setInputValue('');
    setActiveStreams(prev => ({ ...prev, [convoId!]: { content: '', thinking: '' } }));
    setThinkingExpanded(false);
    thinkingSticky.current = true;

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    // Build chat history from the conversation's current messages + new user message
    const currentMessages = (conversations.find(c => c.id === convoId)?.messages || []);
    const chatMessages: ChatMessage[] = [
      ...currentMessages.map(m => ({ role: m.role, content: m.content })),
      { role: 'user' as const, content: text },
    ];

    const targetId = convoId; // capture for closures

    await api.chatCompletion(currentModel, chatMessages, {
      onReasoning: (_token, fullReasoning) => {
        setActiveStreams(prev => {
          if (!prev[targetId]) return prev;
          return { ...prev, [targetId]: { ...prev[targetId], thinking: fullReasoning } };
        });
        if (!thinkingExpanded) setThinkingExpanded(true);
      },
      onToken: (_token, full) => {
        setActiveStreams(prev => {
          if (!prev[targetId]) return prev;
          return { ...prev, [targetId]: { ...prev[targetId], content: full } };
        });
        setThinkingExpanded(false);
      },
      onDone: (stats) => {
        updateConversation(targetId, c => ({
          ...c,
          messages: [...c.messages, {
            role: 'assistant',
            content: stats.content,
            thinking: stats.reasoning || undefined,
            stats,
          }],
          updatedAt: Date.now(),
        }));
        setActiveStreams(prev => {
          const next = { ...prev };
          delete next[targetId];
          return next;
        });
        controllersRef.current.delete(targetId);
      },
      onError: (err) => {
        if (err.name === 'AbortError') return;
        updateConversation(targetId, c => ({
          ...c,
          messages: [...c.messages, { role: 'assistant', content: `Error: ${err.message}` }],
          updatedAt: Date.now(),
        }));
        setActiveStreams(prev => {
          const next = { ...prev };
          delete next[targetId];
          return next;
        });
        controllersRef.current.delete(targetId);
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
          {conversations.map(c => (
            <li
              className={`rail__item ${c.id === activeId ? 'is-active' : ''}`}
              key={c.id}
              role="option"
              onClick={() => handleSelectConversation(c.id)}
            >
              <span className="rail__item-title">
                {c.title || deriveTitle(c.messages)}
              </span>
              <span className="rail__item-meta">
                {streamingConvoIds.has(c.id) && (
                  <span className="rail__streaming-badge">● generating</span>
                )}
                <span className="rail__model-badge">
                  {(c.model || '').split('-')[0]?.toLowerCase() || 'llm'}
                </span>
                <span>{timeAgo(c.updatedAt)}</span>
              </span>
              <button
                className="rail__item-delete"
                onClick={(e) => handleDeleteConversation(e, c.id)}
                aria-label="Delete conversation"
                title="Delete"
              >×</button>
            </li>
          ))}
          {conversations.length === 0 && (
            <li className="rail__empty">No conversations yet</li>
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
                        <div
                          className="message__thinking-content"
                          ref={thinkingContentRef}
                          onScroll={handleThinkingScroll}
                        >{streamingThinking}</div>
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
