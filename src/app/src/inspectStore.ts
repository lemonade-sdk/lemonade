import { useState, useEffect } from 'react';
import api from './api';

export interface CritiqueItem {
  category: 'clarity' | 'constraints' | 'redundancy' | 'token_efficiency' | 'formatting';
  severity: 'low' | 'medium' | 'high';
  finding: string;
  rationale: string;
}

export interface OptimizedPromptData {
  critique: CritiqueItem[];
  parameter_diff: {
    temperature: {
      suggested: number;
      rationale: string;
    };
    system_vs_user_split: boolean;
  };
  optimized_prompt: {
    system_instructions: string | null;
    user_prompt: string;
  };
  key_improvements: string[];
}

export interface OtelSpanAttributeValue {
  stringValue?: string;
  intValue?: string;
  doubleValue?: string;
  boolValue?: boolean;
}

export interface OtelSpanAttribute {
  key: string;
  value: OtelSpanAttributeValue;
}

export interface OtelSpanStatus {
  code: number;
  message?: string;
}

export interface OtelSpan {
  traceId: string;
  spanId: string;
  parentSpanId?: string;
  name?: string;
  startTimeUnixNano?: string;
  endTimeUnixNano?: string;
  attributes?: OtelSpanAttribute[];
  status?: OtelSpanStatus;
}

export interface Trace {
  id: string;
  traceId: string;
  spanId: string;
  kind: 'LLM' | 'EMBEDDING' | 'RERANKER';
  operation: string;
  status: 'ok' | 'slow' | 'error';
  model: string;
  timestamp: string;
  startTimeMs: number;
  synthetic?: boolean;

  // metrics
  ttft?: number;      // ms
  tps?: number;       // tok/s
  prompt?: number;     // Input tokens
  completion?: number; // Output tokens
  dur: number;        // ms
  queue?: number;     // waterfall segment 1: Queue time
  prefill?: number;   // waterfall segment 2: Prefill time (TTFT)

  // sampling
  temp?: number;
  topP?: number;
  topK?: number;
  max?: number;

  // content
  messages: Array<{ role: 'system' | 'user' | 'assistant'; content: string; redacted?: boolean; thinking?: string; tokens?: number }>;
  output: string;

  // identity
  sessionId?: string;
  userId?: string;
  recipe?: string;
  backend?: string;
  device?: string;
  checkpoint?: string;

  // diagnostics / improve
  diag?: { level: 'warn' | 'danger' | 'info'; title: string; detail: string };
  improveData?: OptimizedPromptData | null;
  improveRawOutput?: string;
}

export interface InspectState {
  capturing: boolean;
  captureReady: 'disconnected' | 'connecting' | 'ready' | 'unsupported';
  traces: Trace[];
  selectedTraceId: string | null;
  searchQuery: string;
  filterKind: 'All' | 'LLM' | 'EMBEDDING' | 'RERANKER' | 'Errors';
  toast: string | null;
}

// LocalStorage keys and helper functions
const LOCAL_STORAGE_TRACES_KEY = 'lemonade_inspect_traces';
const LOCAL_STORAGE_CAPTURING_KEY = 'lemonade_inspect_capturing';
const MAX_STORED_TRACES = 100;

function safeGetLocalStorage(key: string): string | null {
  try {
    return typeof localStorage !== 'undefined' ? localStorage.getItem(key) : null;
  } catch {
    return null;
  }
}

function safeSetLocalStorage(key: string, value: string): void {
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(key, value);
  } catch {}
}

function safeRemoveLocalStorage(key: string): void {
  try {
    if (typeof localStorage !== 'undefined') localStorage.removeItem(key);
  } catch {}
}

// Global store with LocalStorage persistence
class InspectStoreClass {
  private listeners = new Set<() => void>();
  private state: InspectState = {
    capturing: false,
    captureReady: 'disconnected',
    traces: [],
    selectedTraceId: null,
    searchQuery: '',
    filterKind: 'All',
    toast: null,
  };
  private ws: WebSocket | null = null;
  private reconnectTimeout: ReturnType<typeof setTimeout> | null = null;
  private currentWsUrl: string = '';
  private autoSelectNextTrace = false;
  private connectionGeneration = 0;

  constructor() {
    this.loadFromLocalStorage();

    // Start connection
    this.connect();

    // Auto-reconnect if api config (port, host, or api key) changes
    api.onStatusChange(() => {
      this.connect();
    });

    api.onSessionHeadersFailed = () => {
      if (this.state.capturing) {
        this.setState({ captureReady: 'unsupported' });
        api.sessionHeadersEnabled = false;
        this.disconnect();
        this.showToast('Capture mode is unsupported by this server/proxy');
      }
    };
  }

  expectIncomingTrace() {
    this.autoSelectNextTrace = true;
  }

  cancelExpectIncomingTrace() {
    this.autoSelectNextTrace = false;
  }

  private loadFromLocalStorage() {
    const savedCapturing = safeGetLocalStorage(LOCAL_STORAGE_CAPTURING_KEY);
    if (savedCapturing !== null) {
      this.state.capturing = savedCapturing === 'true';
    }
    this.state.captureReady = this.state.capturing ? 'connecting' : 'disconnected';
    api.sessionHeadersEnabled = false;

    const savedTraces = safeGetLocalStorage(LOCAL_STORAGE_TRACES_KEY);
    if (savedTraces) {
      try {
        const parsed = JSON.parse(savedTraces);
        if (Array.isArray(parsed)) {
          this.state.traces = parsed
            .filter((t: any) => t && typeof t.id === 'string')
            .map((t: any) => ({
              id: t.id,
              traceId: t.traceId || '',
              spanId: t.spanId || '',
              kind: t.kind || 'LLM',
              operation: t.operation || '',
              status: t.status || 'ok',
              model: t.model || '',
              timestamp: t.timestamp || '',
              startTimeMs: typeof t.startTimeMs === 'number' ? t.startTimeMs : Date.now(),
              synthetic: !!t.synthetic,
              ttft: typeof t.ttft === 'number' ? t.ttft : undefined,
              tps: typeof t.tps === 'number' ? t.tps : undefined,
              prompt: typeof t.prompt === 'number' ? t.prompt : undefined,
              completion: typeof t.completion === 'number' ? t.completion : undefined,
              dur: typeof t.dur === 'number' ? t.dur : 0,
              queue: typeof t.queue === 'number' ? t.queue : undefined,
              prefill: typeof t.prefill === 'number' ? t.prefill : undefined,
              temp: typeof t.temp === 'number' ? t.temp : undefined,
              topP: typeof t.topP === 'number' ? t.topP : undefined,
              topK: typeof t.topK === 'number' ? t.topK : undefined,
              max: typeof t.max === 'number' ? t.max : undefined,
              messages: Array.isArray(t.messages) ? t.messages.map((m: any) => ({
                role: m.role || 'user',
                content: typeof m.content === 'string' ? m.content : '',
                redacted: !!m.redacted,
                thinking: m.thinking,
                tokens: m.tokens
              })) : [],
              output: typeof t.output === 'string' ? t.output : '',
              sessionId: t.sessionId,
              userId: t.userId,
              recipe: t.recipe,
              backend: t.backend,
              device: t.device,
              checkpoint: t.checkpoint,
              diag: t.diag,
              improveData: t.improveData,
              improveRawOutput: t.improveRawOutput
            }));
          if (this.state.traces.length > 0) {
            this.state.selectedTraceId = this.state.traces[0].id;
          }
        }
      } catch (e) {
        console.error('Failed to parse saved traces from localStorage:', e);
      }
    }
  }

  subscribe(listener: () => void) {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  getState() {
    return this.state;
  }

  setState(next: Partial<InspectState>) {
    const prevState = this.state;
    this.state = { ...this.state, ...next };

    // If traces changed, persist to localStorage
    if (next.traces !== undefined && next.traces !== prevState.traces) {
      try {
        const pruned = next.traces.slice(0, MAX_STORED_TRACES);
        this.state.traces = pruned;
        if (pruned.length === 0) {
          safeRemoveLocalStorage(LOCAL_STORAGE_TRACES_KEY);
        } else {
          safeSetLocalStorage(LOCAL_STORAGE_TRACES_KEY, JSON.stringify(pruned));
        }
      } catch (e) {
        console.error('Failed to save traces to localStorage:', e);
      }
    }

    // If capturing changed, persist to localStorage and start/stop capture.
    // If callers set capturing=true while it is already true, still repair a missing
    // socket. This keeps tests and recovery paths deterministic after a dropped WS.
    if (next.capturing !== undefined) {
      if (next.capturing !== prevState.capturing) {
        safeSetLocalStorage(LOCAL_STORAGE_CAPTURING_KEY, String(next.capturing));
        if (next.capturing) {
          this.state.captureReady = 'connecting';
          api.sessionHeadersEnabled = false;
          this.connect();
        } else {
          this.state.captureReady = 'disconnected';
          api.sessionHeadersEnabled = false;
          this.disconnect();
        }
      } else if (next.capturing && !this.hasActiveSocket()) {
        this.connect();
      }
    }

    this.listeners.forEach((l) => l());
  }

  showToast(message: string) {
    this.setState({ toast: message });
    setTimeout(() => {
      if (this.state.toast === message) {
        this.setState({ toast: null });
      }
    }, 2400);
  }

  toggleCapture() {
    const nextVal = !this.state.capturing;
    this.setState({ capturing: nextVal });
    this.showToast(nextVal ? 'Auto-capture enabled' : 'Auto-capture paused');
  }

  clearSession() {
    this.setState({ traces: [], selectedTraceId: null });
    this.showToast('Session cleared');
  }

  selectTrace(id: string | null) {
    this.setState({ selectedTraceId: id });
  }

  setSearchQuery(q: string) {
    this.setState({ searchQuery: q });
  }

  setFilterKind(k: 'All' | 'LLM' | 'EMBEDDING' | 'RERANKER' | 'Errors') {
    this.setState({ filterKind: k });
  }

  addTrace(trace: Trace) {
    if (!this.state.capturing) return;

    // Deduplicate trace by id to avoid key collisions on reconnects
    const exists = this.state.traces.some((t) => t.id === trace.id);
    if (exists) return;

    // Add to start of list (slicing and persistence are handled in setState)
    const updated = [trace, ...this.state.traces];
    const selectId = this.autoSelectNextTrace ? trace.id : (this.state.selectedTraceId || trace.id);

    if (this.autoSelectNextTrace) {
      this.autoSelectNextTrace = false;
    }

    this.setState({
      traces: updated,
      selectedTraceId: selectId
    });
  }

  // WebSocket Connection
  private hasActiveSocket() {
    return !!this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING);
  }

  private connect(force = false) {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }

    if (!this.state.capturing) {
      return;
    }

    const baseUrl = api.baseUrl;
    const apiKey = api.apiKey;

    // We derive websocket URL from api.baseUrl
    let wsUrl = baseUrl.replace(/^http/, 'ws') + '/spans/stream';
    const params = new URLSearchParams();
    params.set('client_session_id', api.clientSessionId);
    wsUrl += `?${params.toString()}`;

    // Skip redundant automatic connection attempts, but allow explicit reconnect()
    // to replace the socket even if the current socket is still open.
    if (!force && this.ws && this.currentWsUrl === wsUrl && this.hasActiveSocket()) {
      return;
    }

    this.currentWsUrl = wsUrl;
    const generation = ++this.connectionGeneration;

    if (this.ws) {
      try {
        this.ws.onopen = null;
        this.ws.onmessage = null;
        this.ws.onclose = null;
        this.ws.onerror = null;
        this.ws.close();
      } catch {}
      this.ws = null;
    }

    try {
      const socket = new WebSocket(wsUrl);
      this.ws = socket;

      const isCurrentSocket = () => this.ws === socket && this.connectionGeneration === generation;

      socket.onopen = () => {
        if (!isCurrentSocket()) return;
        if (socket.readyState !== WebSocket.OPEN) return;
        try {
          socket.send(JSON.stringify({
            type: 'auth',
            token: apiKey,
            client_session_id: api.clientSessionId
          }));
        } catch (err) {
          console.error('Failed to authenticate inspect WebSocket:', err);
          try {
            socket.close();
          } catch {}
        }
      };

      socket.onmessage = (event) => {
        if (!isCurrentSocket()) return;
        try {
          const raw = JSON.parse(event.data);
          if (raw && raw.type === 'error') {
            console.error('Inspect WebSocket auth/connection error:', raw.error?.message || raw);
            this.showToast(raw.error?.message || 'Authentication failed');
            return;
          }
          if (raw && raw.type === 'auth.ok') {
            this.setState({ captureReady: 'ready' });
            api.sessionHeadersEnabled = true;
            return;
          }
          const parsed = parseOtelSpan(raw);
          if (parsed) {
            this.addTrace(parsed);
          }
        } catch (e) {
          console.error('Failed to parse incoming trace event:', e);
        }
      };

      socket.onclose = () => {
        if (!isCurrentSocket()) return;
        this.ws = null;
        if (!this.state.capturing) return;
        if (this.state.captureReady !== 'unsupported') {
          this.setState({ captureReady: 'connecting' });
        }
        api.sessionHeadersEnabled = false;
        if (this.reconnectTimeout) {
          clearTimeout(this.reconnectTimeout);
        }
        this.reconnectTimeout = setTimeout(() => this.connect(), 5000);
      };

      socket.onerror = () => {
        if (!isCurrentSocket()) return;
        try {
          socket.close();
        } catch {}
      };
    } catch (err) {
      console.error('Failed to instantiate inspect WebSocket:', err);
      if (!this.state.capturing) return;
      this.ws = null;
      if (this.reconnectTimeout) {
        clearTimeout(this.reconnectTimeout);
      }
      this.reconnectTimeout = setTimeout(() => this.connect(), 5000);
    }
  }

  disconnect() {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }
    this.connectionGeneration++;
    this.currentWsUrl = '';
    this.setState({ captureReady: 'disconnected' });
    api.sessionHeadersEnabled = false;
    if (this.ws) {
      try {
        this.ws.onopen = null;
        this.ws.onmessage = null;
        this.ws.onclose = null;
        this.ws.onerror = null;
        this.ws.close();
      } catch {}
      this.ws = null;
    }
  }

  // Force reconnect when settings change (e.g. key changed)
  reconnect() {
    this.connect(true);
  }
}

export const inspectStore = new InspectStoreClass();

if (typeof window !== 'undefined') {
  (window as any).inspectStore = inspectStore;
}

// Custom hook to use store
export function useInspectStore() {
  const [state, setState] = useState(() => inspectStore.getState());

  useEffect(() => {
    return inspectStore.subscribe(() => {
      setState(inspectStore.getState());
    });
  }, []);

  return state;
}

// OTEL attribute parser helper
function parseOtelSpan(span: OtelSpan): Trace | null {
  if (!span || !span.traceId) return null;

  const attrs: Record<string, string | number | boolean> = {};
  if (Array.isArray(span.attributes)) {
    for (const attr of span.attributes) {
      if (attr && attr.key && attr.value) {
        const val = attr.value.stringValue ?? attr.value.intValue ?? attr.value.doubleValue ?? attr.value.boolValue;
        if (val !== undefined) {
          attrs[attr.key] = val;
        }
      }
    }
  }

  // Extract kind
  let kind: Trace['kind'] = 'LLM';
  const spanKindAttr = attrs['openinference.span.kind'];
  if (spanKindAttr === 'EMBEDDING') kind = 'EMBEDDING';
  else if (spanKindAttr === 'RERANKER') kind = 'RERANKER';

  // Parse timestamps
  const startNano = parseFloat(span.startTimeUnixNano ?? '0');
  const endNano = parseFloat(span.endTimeUnixNano ?? '0');
  const durationMs = (endNano - startNano) / 1000000;

  // Extract model name
  const model = String(attrs['llm.model_name'] || attrs['embedding.model_name'] || attrs['reranker.model_name'] || attrs['gen_ai.request.model'] || '');

  // Extract inputs/messages
  const messages: Trace['messages'] = [];

  // Search for message array in attributes (support OpenInference & OTel GenAI)
  let i = 0;
  while (true) {
    const role = attrs[`llm.input_messages.${i}.message.role`] || attrs[`gen_ai.input.messages.${i}.role`] || attrs[`llm.input_messages.${i}.role`];
    const content = attrs[`llm.input_messages.${i}.message.content`] || attrs[`gen_ai.input.messages.${i}.content`] || attrs[`llm.input_messages.${i}.content`];

    if (role === undefined && content === undefined) {
      break;
    }
    messages.push({ role: (role === 'system' || role === 'assistant' ? role : 'user'), content: String(content || '') });
    i++;
  }

  // If no message array was found but we have input.value, use that
  if (messages.length === 0 && attrs['input.value']) {
    messages.push({ role: 'user', content: String(attrs['input.value']) });
  }

  // Extract output
  let output = attrs['output.value'] || attrs['gen_ai.output.messages.0.content'] || '';
  if (kind === 'EMBEDDING') {
    output = '[Embedding vector returned]';
  } else if (kind === 'RERANKER') {
    output = '[Re-ranked indices returned]';
  }

  // Parse prompt/completion token count
  const promptRaw = attrs['llm.usage.prompt_tokens'] ?? attrs['gen_ai.usage.input_tokens'];
  const promptTokens = promptRaw !== undefined ? Number(promptRaw) : undefined;

  const completionRaw = attrs['llm.usage.completion_tokens'] ?? attrs['gen_ai.usage.output_tokens'];
  const completionTokens = completionRaw !== undefined ? Number(completionRaw) : undefined;

  // Parse perf metrics
  const tpsRaw = attrs['llm.performance.tokens_per_second'];
  const tps = tpsRaw !== undefined ? Number(tpsRaw) : undefined;
  const ttftRaw = attrs['llm.performance.time_to_first_token'];
  const ttft = ttftRaw !== undefined ? Number(ttftRaw) * 1000 : undefined;

  // Waterfall segments
  const queueRaw = attrs['lemon.queue_time_ms'];
  const queueTime = queueRaw !== undefined ? Number(queueRaw) : undefined;
  const prefillTime = ttft; // TTFT is prefill time

  // Sampling
  const tempRaw = attrs['llm.config.temperature'];
  const temp = tempRaw !== undefined ? Number(tempRaw) : undefined;
  const topPRaw = attrs['llm.config.top_p'];
  const topP = topPRaw !== undefined ? Number(topPRaw) : undefined;
  const topKRaw = attrs['llm.config.top_k'];
  const topK = topKRaw !== undefined ? Number(topKRaw) : undefined;
  const maxRaw = attrs['llm.config.max_tokens'];
  const max = maxRaw !== undefined ? Number(maxRaw) : undefined;

  // Session & User
  const sessionIdRaw = attrs['openinference.session.id'] || attrs['gen_ai.conversation.id'];
  const userIdRaw = attrs['openinference.user.id'];
  const sessionId = sessionIdRaw === undefined ? undefined : String(sessionIdRaw);
  const userId = userIdRaw === undefined ? undefined : String(userIdRaw);

  // Diagnostics (if any error in status)
  let status: Trace['status'] = 'ok';
  let diag: Trace['diag'] = undefined;
  if (span.status && span.status.code === 2) {
    status = 'error';
    diag = {
      level: 'danger',
      title: 'Execution Error',
      detail: span.status.message || 'Unknown error occurred during inference'
    };
  } else if (durationMs > 10000) {
    status = 'slow';
    diag = {
      level: 'warn',
      title: 'High Latency',
      detail: `Request took ${Math.round(durationMs / 1000)}s to complete.`
    };
  }

  // Extract thinking/reasoning if present (like <think> blocks)
  if (kind === 'LLM' && output && typeof output === 'string') {
    const thinkMatch = /<think>([\s\S]*?)<\/think>([\s\S]*)/.exec(output);
    if (thinkMatch) {
      // Create assistant message with thinking
      const thinking = thinkMatch[1].trim();
      const content = thinkMatch[2].trim();
      messages.push({ role: 'assistant', content, thinking });
    } else {
      messages.push({ role: 'assistant', content: output });
    }
  } else if (kind === 'LLM' && output) {
    messages.push({ role: 'assistant', content: String(output) });
  }

  return {
    id: span.spanId || generateId(),
    traceId: span.traceId,
    spanId: span.spanId,
    kind,
    operation: span.name || (kind === 'LLM' ? 'chat.completions' : kind === 'EMBEDDING' ? 'embeddings' : 'reranking'),
    status,
    model,
    timestamp: new Date(Math.round(startNano / 1000000)).toLocaleTimeString(),
    startTimeMs: Math.round(startNano / 1000000),
    ttft,
    tps,
    prompt: promptTokens,
    completion: completionTokens,
    dur: Math.round(durationMs),
    queue: queueTime,
    prefill: prefillTime,
    temp,
    topP,
    topK,
    max,
    messages,
    output: String(output || ''),
    sessionId,
    userId,
    recipe: attrs['llm.recipe'] !== undefined || attrs['recipe'] !== undefined
      ? String(attrs['llm.recipe'] ?? attrs['recipe'])
      : undefined,
    backend: attrs['llm.backend'] !== undefined || attrs['backend'] !== undefined
      ? String(attrs['llm.backend'] ?? attrs['backend'])
      : undefined,
    device: attrs['llm.device_type'] !== undefined || attrs['device_type'] !== undefined
      ? String(attrs['llm.device_type'] ?? attrs['device_type'])
      : undefined,
    checkpoint: attrs['llm.checkpoint'] !== undefined || attrs['checkpoint'] !== undefined
      ? String(attrs['llm.checkpoint'] ?? attrs['checkpoint'])
      : undefined,
    diag
  };
}

function generateId(): string {
  return Math.random().toString(36).substring(2, 11);
}
