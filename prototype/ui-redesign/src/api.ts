/**
 * Lemonade API client — typed wrapper around the lemond HTTP server.
 * Handles connection management, SSE streaming for chat completions
 * and model downloads, and health polling.
 */

const DEFAULT_BASE_URL = 'http://localhost:13305';
const LS_BASE_URL = 'lemonade_base_url';
const LS_API_KEY = 'lemonade_api_key';

export type ConnectionStatus = 'disconnected' | 'connecting' | 'connected';

export interface HealthData {
  status: string;
  version: string;
  model_loaded: string | null;
  websocket_port: number;
  all_models_loaded: LoadedModel[];
  max_models: Record<string, number>;
}

export interface LoadedModel {
  model_name: string;
  checkpoint: string;
  recipe: string;
  device: string;
  backend_url: string;
  pid: number;
  type: string;
  last_use: number;
  recipe_options?: Record<string, unknown>;
}

export interface ModelInfo {
  id: string;
  name?: string;
  display_name?: string;
  labels?: string[];
  size?: number;
  recipes?: Record<string, unknown>[];
  [key: string]: unknown;
}

export interface ModelsData {
  data: ModelInfo[];
}

export interface ChatCompletionStats {
  content: string;
  reasoning: string;
  id: string | null;
  tps: string;
  ttft: string | null;
  tokens: number;
  reasoningTokens: number;
}

export interface LiveStreamStats {
  tps: number;
  tokens: number;
  reasoningTokens: number;
  elapsed: number;
  ttft: number | null;
}

export interface ChatCompletionCallbacks {
  onToken?: (token: string, fullContent: string) => void;
  onReasoning?: (token: string, fullReasoning: string) => void;
  onStats?: (stats: LiveStreamStats) => void;
  onDone?: (stats: ChatCompletionStats) => void;
  onToolCalls?: (toolCalls: Array<{ id: string; type: 'function'; function: { name: string; arguments: string } }>) => void;
  onError?: (err: Error) => void;
  params?: Record<string, unknown>;
  tools?: Array<Record<string, unknown>>;
  signal?: AbortSignal;
}

export interface PullCallbacks {
  onProgress?: (data: { percent?: number; [key: string]: unknown }) => void;
  onComplete?: (data: Record<string, unknown>) => void;
  onError?: (err: Error) => void;
}

export interface StatsData {
  input_tokens: number;
  output_tokens: number;
  time_to_first_token: number;
  tokens_per_second: number;
  decode_token_times: number[];
  prompt_tokens: number;
}

export interface SystemStatsData {
  cpu_percent: number | null;
  memory_gb: number | null;
  gpu_percent: number | null;
  vram_gb: number | null;
  npu_percent: number | null;
}

export interface SlotTimings {
  prompt_n: number;
  prompt_ms: number;
  prompt_per_token_ms: number;
  prompt_per_second: number;
  predicted_n: number;
  predicted_ms: number;
  predicted_per_token_ms: number;
  predicted_per_second: number;
}

export interface SlotData {
  id: number;
  n_ctx: number;
  n_decoded: number;
  n_prompt_tokens: number;
  n_prompt_tokens_processed: number;
  state: number;
  is_processing: boolean;
  model: string;
  temperature: number;
  top_k: number;
  top_p: number;
  cache_tokens: number[];
  n_cache_tokens?: number;
  timings: SlotTimings;
  prompt: string;
  truncated: boolean;
  stopped_eos: boolean;
  stopped_word: boolean;
  stopped_limit: boolean;
}

/** Get usable cache token count — prefers n_cache_tokens (number) over cache_tokens array length */
export function getCacheTokenCount(s: SlotData): number {
  if (typeof s.n_cache_tokens === 'number') return s.n_cache_tokens;
  return s.cache_tokens?.length || 0;
}

export interface LogEntry {
  seq: number;
  timestamp: string;
  severity: string;
  tag: string;
  line: string;
}

export interface LogStreamCallbacks {
  onConnected?: () => void;
  onDisconnected?: () => void;
  onError?: (message: string) => void;
  onSnapshot?: (entries: LogEntry[]) => void;
  onEntry?: (entry: LogEntry) => void;
}

export interface LogStreamHandle {
  close: () => void;
}

export type ChatMessageContent = string | null | Array<
  | { type: 'text'; text: string }
  | { type: 'image_url'; image_url: { url: string } }
>;

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content: ChatMessageContent;
  tool_calls?: Array<{
    id: string;
    type: 'function';
    function: { name: string; arguments: string };
  }>;
  tool_call_id?: string;
}

type StatusListener = (status: ConnectionStatus) => void;

class LemonadeAPI {
  private _status: ConnectionStatus = 'disconnected';
  private _listeners: StatusListener[] = [];
  private _modelsChangedListeners: Array<() => void> = [];
  private _healthData: HealthData | null = null;
  private _modelsData: ModelsData | null = null;
  private _systemInfoData: Record<string, unknown> | null = null;
  private _pollTimer: ReturnType<typeof setInterval> | null = null;

  // ── Config (persisted in localStorage) ──────────────────────────

  get baseUrl(): string {
    return (localStorage.getItem(LS_BASE_URL) || DEFAULT_BASE_URL).replace(/\/+$/, '');
  }

  set baseUrl(url: string) {
    localStorage.setItem(LS_BASE_URL, url.replace(/\/+$/, ''));
  }

  get apiKey(): string {
    return localStorage.getItem(LS_API_KEY) || '';
  }

  set apiKey(key: string) {
    if (key) localStorage.setItem(LS_API_KEY, key);
    else localStorage.removeItem(LS_API_KEY);
  }

  // ── Connection status ───────────────────────────────────────────

  get status(): ConnectionStatus { return this._status; }
  get isConnected(): boolean { return this._status === 'connected'; }
  get healthData(): HealthData | null { return this._healthData; }
  get modelsData(): ModelsData | null { return this._modelsData; }
  get systemInfoData(): Record<string, unknown> | null { return this._systemInfoData; }

  get loadedModels(): LoadedModel[] {
    return this._healthData?.all_models_loaded || [];
  }

  get allModels(): ModelInfo[] {
    return this._modelsData?.data || [];
  }

  onStatusChange(fn: StatusListener): () => void {
    this._listeners.push(fn);
    return () => { this._listeners = this._listeners.filter(f => f !== fn); };
  }

  onModelsChanged(fn: () => void): () => void {
    this._modelsChangedListeners.push(fn);
    return () => { this._modelsChangedListeners = this._modelsChangedListeners.filter(f => f !== fn); };
  }

  private _notifyModelsChanged(): void {
    this._modelsChangedListeners.forEach(fn => { try { fn(); } catch {} });
  }

  private _setStatus(s: ConnectionStatus): void {
    if (this._status === s) return;
    this._status = s;
    this._listeners.forEach(fn => { try { fn(s); } catch {} });
  }

  // ── Fetch wrapper ───────────────────────────────────────────────

  private _headers(extra?: Record<string, string>): Record<string, string> {
    const h: Record<string, string> = { ...(extra || {}) };
    if (this.apiKey) h['Authorization'] = `Bearer ${this.apiKey}`;
    return h;
  }

  private async _fetch(path: string, opts: RequestInit & { body?: unknown } = {}): Promise<Response> {
    const url = `${this.baseUrl}${path}`;
    const headers = this._headers(opts.headers as Record<string, string>);

    let processedOpts = { ...opts };
    if (opts.body && typeof opts.body === 'object' &&
        !(opts.body instanceof FormData) &&
        !(opts.body instanceof ReadableStream)) {
      headers['Content-Type'] = 'application/json';
      processedOpts = { ...opts, body: JSON.stringify(opts.body) };
    }

    const resp = await fetch(url, { ...processedOpts, headers } as RequestInit);
    if (!resp.ok) {
      const text = await resp.text().catch(() => '');
      const err = new Error(text || resp.statusText || `HTTP ${resp.status}`);
      (err as any).status = resp.status;
      throw err;
    }
    return resp;
  }

  private async _json<T = unknown>(path: string, opts?: RequestInit & { body?: unknown }): Promise<T> {
    const resp = await this._fetch(path, opts);
    return resp.json() as Promise<T>;
  }

  // ── Endpoints ───────────────────────────────────────────────────

  async health(): Promise<HealthData> {
    const data = await this._json<HealthData>('/api/v1/health');
    this._healthData = data;
    this._setStatus('connected');
    return data;
  }

  async models(showAll = true): Promise<ModelsData> {
    const qs = showAll ? '?show_all=true' : '';
    const data = await this._json<ModelsData>(`/api/v1/models${qs}`);
    this._modelsData = data;
    return data;
  }

  async modelDetail(id: string): Promise<ModelInfo> {
    return this._json<ModelInfo>(`/api/v1/models/${encodeURIComponent(id)}`);
  }

  async loadModel(modelName: string, recipeOptions?: Record<string, unknown>): Promise<unknown> {
    const body: Record<string, unknown> = { model_name: modelName, ...recipeOptions };
    const result = await this._json('/api/v1/load', { method: 'POST', body });
    this._notifyModelsChanged();
    return result;
  }

  async unloadModel(modelName?: string): Promise<unknown> {
    const body = modelName ? { model_name: modelName } : {};
    const result = await this._json('/api/v1/unload', { method: 'POST', body });
    this._notifyModelsChanged();
    return result;
  }

  async deleteModel(modelName: string): Promise<unknown> {
    const result = await this._json('/api/v1/delete', {
      method: 'POST',
      body: { model_name: modelName },
    });
    this._notifyModelsChanged();
    return result;
  }

  async systemInfo(): Promise<Record<string, unknown>> {
    const data = await this._json<Record<string, unknown>>('/api/v1/system-info');
    this._systemInfoData = data;
    return data;
  }

  // ── Dashboard data ──────────────────────────────────────────────

  async stats(): Promise<StatsData> {
    return this._json<StatsData>('/api/v1/stats');
  }

  async systemStats(): Promise<SystemStatsData> {
    return this._json<SystemStatsData>('/api/v1/system-stats');
  }

  async slots(): Promise<SlotData[]> {
    return this._json<SlotData[]>('/api/v1/slots');
  }

  // ── Log level ───────────────────────────────────────────────────

  async getLogLevel(): Promise<string> {
    const data = await this._json<Record<string, unknown>>('/internal/config');
    return (data.log_level as string) || 'info';
  }

  async setLogLevel(level: string): Promise<{ status: string; level: string }> {
    return this._json<{ status: string; level: string }>('/api/v1/log-level', {
      method: 'POST',
      body: { level },
    });
  }

  // ── Log stream (WebSocket) ──────────────────────────────────────

  connectLogStream(callbacks: LogStreamCallbacks, afterSeq?: number | null): LogStreamHandle {
    const health = this._healthData;
    if (!health?.websocket_port) {
      callbacks.onError?.('No WebSocket port available');
      return { close: () => {} };
    }

    // Build WS URL from the HTTP base URL
    const baseUrl = new URL(this.baseUrl);
    const wsProto = baseUrl.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${wsProto}//${baseUrl.hostname}:${health.websocket_port}/logs/stream`;

    let socket: WebSocket;
    try {
      socket = new WebSocket(wsUrl);
    } catch (err) {
      callbacks.onError?.(`WebSocket connection failed: ${err}`);
      return { close: () => {} };
    }

    socket.addEventListener('open', () => {
      socket.send(JSON.stringify({
        type: 'logs.subscribe',
        after_seq: afterSeq ?? null,
      }));
      callbacks.onConnected?.();
    });

    socket.addEventListener('message', (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === 'logs.snapshot') {
          callbacks.onSnapshot?.(msg.entries ?? []);
        } else if (msg.type === 'logs.entry' && msg.entry) {
          callbacks.onEntry?.(msg.entry);
        } else if (msg.type === 'error') {
          callbacks.onError?.(msg.error?.message || 'Server error');
        }
      } catch {}
    });

    socket.addEventListener('error', () => {
      callbacks.onError?.('WebSocket error');
    });

    socket.addEventListener('close', () => {
      callbacks.onDisconnected?.();
    });

    return { close: () => socket.close(1000, 'OK') };
  }

  // ── Backend management ──────────────────────────────────────────

  async installBackend(
    recipe: string,
    backend: string,
    callbacks?: { onProgress?: (data: Record<string, unknown>) => void; onComplete?: () => void; onError?: (err: Error) => void },
  ): Promise<void> {
    try {
      const resp = await this._fetch('/api/v1/install', {
        method: 'POST',
        body: { recipe, backend, stream: true, subscribe: true },
      });
      const reader = resp.body!.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        const lines = buf.split('\n');
        buf = lines.pop()!;
        for (const line of lines) {
          if (!line.startsWith('data: ')) continue;
          try {
            const d = JSON.parse(line.slice(6));
            callbacks?.onProgress?.(d);
          } catch {}
        }
      }
      callbacks?.onComplete?.();
    } catch (err) {
      callbacks?.onError?.(err as Error);
    }
  }

  async uninstallBackend(recipe: string, backend: string): Promise<unknown> {
    return this._json('/api/v1/uninstall', {
      method: 'POST',
      body: { recipe, backend },
    });
  }

  // ── SSE: Pull (model download) ──────────────────────────────────

  async pullModel(modelName: string, callbacks: PullCallbacks = {}): Promise<void> {
    const { onProgress, onComplete, onError } = callbacks;
    try {
      const resp = await this._fetch('/api/v1/pull', {
        method: 'POST',
        body: { model: modelName, stream: true },
      });
      const reader = resp.body!.getReader();
      const dec = new TextDecoder();
      let buf = '';

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        const lines = buf.split('\n');
        buf = lines.pop()!;

        for (const line of lines) {
          if (line.startsWith('event:')) continue;
          if (!line.startsWith('data: ')) continue;
          try {
            const d = JSON.parse(line.slice(6));
            if (d.percent !== undefined) onProgress?.(d);
            else onComplete?.(d);
          } catch {}
        }
      }
      onComplete?.({});
      this._notifyModelsChanged();
    } catch (err) {
      onError?.(err as Error);
    }
  }

  // ── SSE: Chat completions ───────────────────────────────────────

  async chatCompletion(
    model: string,
    messages: ChatMessage[],
    callbacks: ChatCompletionCallbacks = {}
  ): Promise<void> {
    const { onToken, onReasoning, onStats, onDone, onToolCalls, onError, params, tools, signal } = callbacks;
    const t0 = performance.now();
    let firstTokenTime: number | null = null;
    let tokenCount = 0;
    let reasoningTokenCount = 0;

    const emitStats = () => {
      const now = performance.now();
      const elapsed = now - t0;
      const total = tokenCount + reasoningTokenCount;
      // TPS = decode rate from first token, not from request start
      const decodeTime = firstTokenTime ? (now - firstTokenTime) / 1000 : 0;
      onStats?.({
        tps: total > 0 && decodeTime > 0 ? total / decodeTime : 0,
        tokens: tokenCount,
        reasoningTokens: reasoningTokenCount,
        elapsed,
        ttft: firstTokenTime ? firstTokenTime - t0 : null,
      });
    };

    // Timer-based stats: update every 200ms so display stays live even between tokens
    const statsInterval = onStats ? setInterval(emitStats, 200) : undefined;

    try {
      const body: Record<string, unknown> = { model, messages, stream: true, ...(params || {}) };
      if (tools && tools.length > 0) body.tools = tools;
      const resp = await this._fetch('/api/v1/chat/completions', {
        method: 'POST',
        body,
        signal,
      });

      const reader = resp.body!.getReader();
      const dec = new TextDecoder();
      let buf = '';
      let full = '';
      let reasoning = '';
      let respId: string | null = null;
      const pendingToolCalls: Map<number, { id: string; type: 'function'; function: { name: string; arguments: string } }> = new Map();

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        const lines = buf.split('\n');
        buf = lines.pop()!;

        for (const line of lines) {
          const t = line.trim();
          if (!t || !t.startsWith('data: ')) continue;
          const payload = t.slice(6);
          if (payload === '[DONE]') {
            clearInterval(statsInterval);
            // If we accumulated tool calls, emit them instead of content
            if (pendingToolCalls.size > 0) {
              onToolCalls?.(Array.from(pendingToolCalls.values()));
              return;
            }
            const now = performance.now();
            const decodeTime = firstTokenTime ? (now - firstTokenTime) / 1000 : 0;
            const totalTokens = tokenCount + reasoningTokenCount;
            const tps = totalTokens > 0 && decodeTime > 0 ? (totalTokens / decodeTime).toFixed(1) : '0';
            const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
            onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
            return;
          }
          try {
            const chunk = JSON.parse(payload);
            // Detect server-side error in SSE stream
            if (chunk.error) {
              clearInterval(statsInterval);
              onError?.(new Error(chunk.error.message || 'Server streaming error'));
              return;
            }
            respId = chunk.id || respId;
            const delta = chunk.choices?.[0]?.delta;
            // Handle reasoning/thinking tokens (Qwen3.5, etc.)
            if (delta?.reasoning_content) {
              if (!firstTokenTime) firstTokenTime = performance.now();
              reasoningTokenCount++;
              reasoning += delta.reasoning_content;
              onReasoning?.(delta.reasoning_content, reasoning);
            }
            if (delta?.content) {
              if (!firstTokenTime) firstTokenTime = performance.now();
              tokenCount++;
              full += delta.content;
              onToken?.(delta.content, full);
            }
            // Accumulate tool calls (streamed incrementally)
            if (delta?.tool_calls) {
              for (const tc of delta.tool_calls) {
                const idx = tc.index ?? 0;
                if (!pendingToolCalls.has(idx)) {
                  pendingToolCalls.set(idx, {
                    id: tc.id || '',
                    type: 'function',
                    function: { name: tc.function?.name || '', arguments: '' },
                  });
                }
                const entry = pendingToolCalls.get(idx)!;
                if (tc.id) entry.id = tc.id;
                if (tc.function?.name) entry.function.name = tc.function.name;
                if (tc.function?.arguments) entry.function.arguments += tc.function.arguments;
              }
            }
          } catch {}
        }
      }
      // Stream ended without [DONE]
      clearInterval(statsInterval);
      if (pendingToolCalls.size > 0) {
        onToolCalls?.(Array.from(pendingToolCalls.values()));
      } else {
        const now = performance.now();
        const decodeTime = firstTokenTime ? (now - firstTokenTime) / 1000 : 0;
        const totalTokens = tokenCount + reasoningTokenCount;
        const tps = totalTokens > 0 && decodeTime > 0 ? (totalTokens / decodeTime).toFixed(1) : '0';
        const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
        onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
      }
    } catch (err) {
      clearInterval(statsInterval);
      onError?.(err as Error);
    }
  }

  // ── Connection management ───────────────────────────────────────

  async connect(): Promise<boolean> {
    this._setStatus('connecting');
    try {
      await this.health();
      return true;
    } catch {
      this._setStatus('disconnected');
      this._healthData = null;
      return false;
    }
  }

  async refresh(): Promise<{ health: HealthData; models: ModelsData } | null> {
    try {
      const [health, models] = await Promise.all([
        this.health(),
        this.models(true),
      ]);
      return { health, models };
    } catch {
      this._setStatus('disconnected');
      this._healthData = null;
      return null;
    }
  }

  startPolling(ms = 15000): void {
    this.stopPolling();
    this._pollTimer = setInterval(() => this.connect(), ms);
  }

  stopPolling(): void {
    if (this._pollTimer) {
      clearInterval(this._pollTimer);
      this._pollTimer = null;
    }
  }
}

// Singleton export
export const api = new LemonadeAPI();
export default api;

/* ── HuggingFace search (standalone — external API) ────────── */

export interface HFModelResult {
  id: string;           // e.g. "TheBloke/Llama-2-7B-GGUF"
  modelId: string;
  likes: number;
  downloads: number;
  tags: string[];
  lastModified: string;
  pipeline_tag?: string;
  siblings?: { rfilename: string }[];
}

export async function searchHuggingFace(
  query: string,
  signal?: AbortSignal,
): Promise<HFModelResult[]> {
  const params = new URLSearchParams({
    search: query,
    filter: 'gguf',
    sort: 'downloads',
    direction: '-1',
    limit: '20',
  });
  const resp = await fetch(
    `https://huggingface.co/api/models?${params}`,
    { signal },
  );
  if (!resp.ok) return [];
  return resp.json();
}
