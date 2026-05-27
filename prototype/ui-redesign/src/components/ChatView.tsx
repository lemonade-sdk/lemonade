import React, { useState, useRef, useCallback, useEffect } from 'react';
import api, { ChatMessage, ChatCompletionStats, LoadedModel } from '../api';
import MarkdownMessage from './MarkdownMessage';
import { useChatStreaming, ToolCallEntry } from '../hooks/useChatStreaming';

interface Message {
  role: 'user' | 'assistant';
  content: string;
  images?: string[];  // base64 data URLs for user messages with images
  thinking?: string;
  stats?: ChatCompletionStats;
  toolCalls?: ToolCallEntry[];
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
  // Strip base64 images before persisting — they're too large for localStorage
  const stripped = convos.map(c => ({
    ...c,
    messages: c.messages.map(m => m.images ? { ...m, images: undefined } : m),
  }));
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(stripped)); } catch { /* ignore */ }
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

const TOOLS_KEY = 'lemonade_use_tools';
const MAX_IMAGE_DIM = 1024;
const MAX_IMAGES = 4;

/** Resize and compress an image file to base64 data URL */
async function imageToBase64(file: File | Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      const img = new Image();
      img.onload = () => {
        // Downscale if needed
        let { width, height } = img;
        if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
          const scale = MAX_IMAGE_DIM / Math.max(width, height);
          width = Math.round(width * scale);
          height = Math.round(height * scale);
        }
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d')!;
        ctx.drawImage(img, 0, 0, width, height);
        resolve(canvas.toDataURL('image/jpeg', 0.85));
      };
      img.onerror = reject;
      img.src = reader.result as string;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

const ChatView: React.FC<ChatViewProps> = ({ currentModel, loadedModels, onModelSelect, onRefresh }) => {
  const [conversations, setConversations] = useState<Conversation[]>(loadConversations);
  const [activeId, setActiveId] = useState<string | null>(loadActiveId);
  const [inputValue, setInputValue] = useState('');
  const [pendingImages, setPendingImages] = useState<string[]>([]);
  const [railExpanded, setRailExpanded] = useState(true);
  const [useTools, setUseTools] = useState(() => {
    try { return localStorage.getItem(TOOLS_KEY) === 'true'; } catch { return false; }
  });
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const thinkingContentRef = useRef<HTMLDivElement>(null);
  const thinkingSticky = useRef(true);
  const scrollRafRef = useRef<number>(0);

  const updateConversation = useCallback((id: string, updater: (c: Conversation) => Conversation) => {
    setConversations(prev => prev.map(c => c.id === id ? updater(c) : c));
  }, []);

  // Streaming hook — owns token buffer, flush interval, abort controllers
  const handleStreamDone = useCallback((convoId: string, stats: ChatCompletionStats, toolCalls?: ToolCallEntry[]) => {
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, {
        role: 'assistant',
        content: stats.content,
        thinking: stats.reasoning || undefined,
        toolCalls,
        stats,
      }],
      updatedAt: Date.now(),
    }));
  }, [updateConversation]);

  const handleStreamError = useCallback((convoId: string, message: string) => {
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, { role: 'assistant', content: `Error: ${message}` }],
      updatedAt: Date.now(),
    }));
  }, [updateConversation]);

  const streaming = useChatStreaming(handleStreamDone, handleStreamError);

  // Derived: is the CURRENT conversation streaming?
  const currentStream = activeId ? streaming.getStream(activeId) : undefined;
  const isStreaming = !!currentStream;
  const streamingContent = currentStream?.content || '';
  const streamingThinking = currentStream?.thinking || '';
  const streamingToolStatus = currentStream?.toolStatus || '';
  const streamingToolCalls = currentStream?.toolCalls || [];
  const currentLiveStats = activeId ? streaming.getLiveStats(activeId) : undefined;

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

  const scrollToBottom = useCallback(() => {
    if (scrollRafRef.current) cancelAnimationFrame(scrollRafRef.current);
    scrollRafRef.current = requestAnimationFrame(() => {
      if (threadRef.current) {
        threadRef.current.scrollTop = threadRef.current.scrollHeight;
      }
    });
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
    const partial = streaming.stop(activeId);
    if (partial) {
      updateConversation(activeId, c => ({
        ...c,
        messages: [...c.messages, {
          role: 'assistant' as const,
          content: partial.content,
          thinking: partial.thinking,
        }],
        updatedAt: Date.now(),
      }));
    }
  }, [activeId, streaming, updateConversation]);

  const handleSend = async (overrideText?: string) => {
    const text = (overrideText ?? inputValue).trim();
    if ((!text && pendingImages.length === 0) || isStreaming) return;
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

    const images = pendingImages.length > 0 ? [...pendingImages] : undefined;
    const userMessage: Message = { role: 'user', content: text, images };
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, userMessage],
      model: currentModel,
      updatedAt: Date.now(),
    }));

    setInputValue('');
    setPendingImages([]);
    thinkingSticky.current = true;

    // Build chat history from the conversation's current messages + new user message
    const currentMessages = (conversations.find(c => c.id === convoId)?.messages || []);
    const chatMessages: ChatMessage[] = currentMessages.map(m => {
      if (m.images?.length) {
        return {
          role: m.role,
          content: [
            { type: 'text' as const, text: m.content },
            ...m.images.map(url => ({ type: 'image_url' as const, image_url: { url } })),
          ],
        };
      }
      return { role: m.role, content: m.content };
    });

    // Add the new user message
    if (images?.length) {
      chatMessages.push({
        role: 'user' as const,
        content: [
          { type: 'text' as const, text },
          ...images.map(url => ({ type: 'image_url' as const, image_url: { url } })),
        ],
      });
    } else {
      chatMessages.push({ role: 'user' as const, content: text });
    }

    await streaming.send(convoId, currentModel, chatMessages, useTools);
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  // ── Image handling ──────────────────────────────────────────

  const addImages = useCallback(async (files: File[]) => {
    const imageFiles = files.filter(f => f.type.startsWith('image/'));
    if (imageFiles.length === 0) return;
    const remaining = MAX_IMAGES - pendingImages.length;
    const toProcess = imageFiles.slice(0, remaining);
    const encoded = await Promise.all(toProcess.map(imageToBase64));
    setPendingImages(prev => [...prev, ...encoded].slice(0, MAX_IMAGES));
  }, [pendingImages.length]);

  const handlePaste = useCallback((e: React.ClipboardEvent) => {
    const items = e.clipboardData?.items;
    if (!items) return;
    const files: File[] = [];
    for (let i = 0; i < items.length; i++) {
      if (items[i].type.startsWith('image/')) {
        const file = items[i].getAsFile();
        if (file) files.push(file);
      }
    }
    if (files.length > 0) {
      e.preventDefault();
      addImages(files);
    }
  }, [addImages]);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const files = Array.from(e.dataTransfer.files);
    addImages(files);
  }, [addImages]);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
  }, []);

  const handleFileSelect = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const files = Array.from(e.target.files || []);
    addImages(files);
    e.target.value = '';
  }, [addImages]);

  const removeImage = useCallback((index: number) => {
    setPendingImages(prev => prev.filter((_, i) => i !== index));
  }, []);

  // ── Option select from assistant messages ───────────────────

  const handleOptionSelect = useCallback((text: string) => {
    handleSend(text);
  }, [handleSend]);

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
                {streaming.streamingConvoIds.has(c.id) && (
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
                <MessageBubble key={i} message={msg} currentModel={currentModel} onOptionSelect={handleOptionSelect} />
              ))}

              {isStreaming && (
                <article className="message message--assistant">
                  <div className="message__avatar">
                    {(currentModel || 'A').charAt(0).toUpperCase()}
                  </div>
                  <div className="message__body">
                    <div className="message__author">{currentModel || 'Assistant'}</div>
                    {streamingThinking && (
                      <details className="message__thinking" open={streaming.thinkingExpanded}>
                        <summary>Thinking…</summary>
                        <div
                          className="message__thinking-content"
                          ref={thinkingContentRef}
                          onScroll={handleThinkingScroll}
                        >{streamingThinking}</div>
                      </details>
                    )}
                    {streamingToolCalls.length > 0 && <ToolCallsDisplay calls={streamingToolCalls} />}
                    {streamingContent ? (
                      <MarkdownMessage content={streamingContent} isComplete={false} onOptionSelect={handleOptionSelect} />
                    ) : !streamingThinking ? (
                      <div className="message__content">
                        <span className="streaming-cursor" aria-hidden="true" />
                      </div>
                    ) : null}
                    {streamingContent && <span className="streaming-cursor" aria-hidden="true" />}
                    {currentLiveStats && (
                      <div className="message__live-stats">
                        <span>{currentLiveStats.tps.toFixed(1)} tok/s</span>
                        {currentLiveStats.ttft != null && <span>{(currentLiveStats.ttft / 1000).toFixed(2)}s TTFT</span>}
                        <span>{currentLiveStats.tokens + currentLiveStats.reasoningTokens} tokens</span>
                        <span>{(currentLiveStats.elapsed / 1000).toFixed(1)}s</span>
                      </div>
                    )}
                  </div>
                </article>
              )}
            </div>
          )}
        </div>
      </div>

      {/* Composer */}
      <div className="composer" onDrop={handleDrop} onDragOver={handleDragOver}>
        <div className="composer__toolbar">
          {loadedModels.length > 1 && (
            <div className="composer__model-picker">
              <span className="composer__model-label">Model</span>
              <select
                className="composer__model-select"
                value={currentModel || ''}
                onChange={e => onModelSelect(e.target.value)}
              >
                {loadedModels.map(m => (
                  <option key={m.model_name} value={m.model_name}>{m.model_name}</option>
                ))}
              </select>
            </div>
          )}
          <button
            className={`composer__tools-toggle ${useTools ? 'composer__tools-toggle--active' : ''}`}
            onClick={() => {
              const next = !useTools;
              setUseTools(next);
              try { localStorage.setItem(TOOLS_KEY, String(next)); } catch { /* ignore */ }
            }}
            title={useTools ? 'Lemonade tools enabled — click to disable' : 'Enable lemonade tools (model management via chat)'}
            aria-pressed={useTools}
          >
            🛠 Tools {useTools ? 'ON' : 'OFF'}
          </button>
        </div>
        {streamingToolStatus && (
          <div className="composer__tool-status">
            <span className="composer__tool-status-dot" />
            {streamingToolStatus}
          </div>
        )}
        {pendingImages.length > 0 && (
          <div className="composer__images">
            {pendingImages.map((src, i) => (
              <div key={i} className="composer__image-thumb">
                <img src={src} alt={`Attachment ${i + 1}`} />
                <button className="composer__image-remove" onClick={() => removeImage(i)} aria-label="Remove image">×</button>
              </div>
            ))}
          </div>
        )}
        <div className="composer__bar">
          <button
            className="composer__attach"
            onClick={() => fileInputRef.current?.click()}
            disabled={!currentModel || isStreaming || pendingImages.length >= MAX_IMAGES}
            title="Attach image"
            aria-label="Attach image"
          >
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <path d="M21.44 11.05l-9.19 9.19a6 6 0 01-8.49-8.49l9.19-9.19a4 4 0 015.66 5.66l-9.2 9.19a2 2 0 01-2.83-2.83l8.49-8.48" />
            </svg>
          </button>
          <input
            ref={fileInputRef}
            type="file"
            accept="image/*"
            multiple
            style={{ display: 'none' }}
            onChange={handleFileSelect}
          />
          <textarea
            ref={inputRef}
            className="composer__input"
            placeholder={currentModel ? `Message ${currentModel}…` : 'Connect to a server to start…'}
            value={inputValue}
            onChange={e => setInputValue(e.target.value)}
            onKeyDown={handleKeyDown}
            onPaste={handlePaste}
            disabled={!currentModel || isStreaming}
            rows={1}
          />
          {isStreaming ? (
            <button className="composer__stop" onClick={handleStop} aria-label="Stop generating" title="Stop">■</button>
          ) : (
            <button
              className="composer__send"
              onClick={() => handleSend()}
              disabled={(!inputValue.trim() && pendingImages.length === 0) || !currentModel}
              aria-label="Send"
            >↑</button>
          )}
        </div>
        <div className="composer__hint">⏎ to send · Shift+⏎ for newline · Paste or drop images</div>
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

/* ── Tool call indicator ─────────────────────────────────── */

const TOOL_LABELS: Record<string, string> = {
  list_models: 'List models',
  get_model_info: 'Get model info',
  load_model: 'Load model',
  unload_model: 'Unload model',
  get_loaded_models: 'Get loaded models',
  get_server_health: 'Server health',
  pull_model: 'Pull model',
  delete_model: 'Delete model',
  get_system_info: 'System info',
  list_backends: 'List backends',
  install_backend: 'Install backend',
  ask_question: 'Asking you',
};

const ToolCallsDisplay: React.FC<{ calls: ToolCallEntry[] }> = ({ calls }) => {
  if (calls.length === 0) return null;
  return (
    <div className="message__tool-calls">
      {calls.map((tc, i) => (
        <details key={i} className={`message__tool-call message__tool-call--${tc.status}`}>
          <summary>
            <span className="message__tool-call-icon">{tc.status === 'running' ? '⏳' : tc.status === 'error' ? '❌' : '✅'}</span>
            <span className="message__tool-call-name">{TOOL_LABELS[tc.name] || tc.name}</span>
            {tc.args && <span className="message__tool-call-args">{tc.args}</span>}
          </summary>
          {tc.result && <div className="message__tool-call-result">{tc.result}</div>}
        </details>
      ))}
    </div>
  );
};

/* ── Message bubble ──────────────────────────────────────── */

const MessageBubble: React.FC<{ message: Message; currentModel: string | null; onOptionSelect?: (text: string) => void }> = ({ message, currentModel, onOptionSelect }) => {
  const [thinkingOpen, setThinkingOpen] = useState(false);

  if (message.role === 'user') {
    return (
      <article className="message message--user">
        <div className="message__avatar">Y</div>
        <div className="message__body">
          <div className="message__author">You</div>
          {message.images && message.images.length > 0 && (
            <div className="message__images">
              {message.images.map((src, i) => (
                <img key={i} src={src} alt={`Attached image ${i + 1}`} className="message__image" />
              ))}
            </div>
          )}
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
        {message.toolCalls && <ToolCallsDisplay calls={message.toolCalls} />}
        <MarkdownMessage content={message.content} onOptionSelect={onOptionSelect} />
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
