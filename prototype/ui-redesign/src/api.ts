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

export interface ChatCompletionCallbacks {
  onToken?: (token: string, fullContent: string) => void;
  onReasoning?: (token: string, fullReasoning: string) => void;
  onDone?: (stats: ChatCompletionStats) => void;
  onError?: (err: Error) => void;
  params?: Record<string, unknown>;
  signal?: AbortSignal;
}

export interface PullCallbacks {
  onProgress?: (data: { percent?: number; [key: string]: unknown }) => void;
  onComplete?: (data: Record<string, unknown>) => void;
  onError?: (err: Error) => void;
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant';
  content: string;
}

type StatusListener = (status: ConnectionStatus) => void;

class LemonadeAPI {
  private _status: ConnectionStatus = 'disconnected';
  private _listeners: StatusListener[] = [];
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
    const body: Record<string, unknown> = { model_name: modelName };
    if (recipeOptions) body.recipe_options = recipeOptions;
    return this._json('/api/v1/load', { method: 'POST', body });
  }

  async unloadModel(modelName?: string): Promise<unknown> {
    const body = modelName ? { model_name: modelName } : {};
    return this._json('/api/v1/unload', { method: 'POST', body });
  }

  async systemInfo(): Promise<Record<string, unknown>> {
    const data = await this._json<Record<string, unknown>>('/api/v1/system-info');
    this._systemInfoData = data;
    return data;
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
    const { onToken, onReasoning, onDone, onError, params, signal } = callbacks;
    const t0 = performance.now();
    let firstTokenTime: number | null = null;
    let tokenCount = 0;
    let reasoningTokenCount = 0;

    try {
      const body = { model, messages, stream: true, ...(params || {}) };
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
            const elapsed = performance.now() - t0;
            const totalTokens = tokenCount + reasoningTokenCount;
            const tps = totalTokens > 0 ? (totalTokens / (elapsed / 1000)).toFixed(1) : '0';
            const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
            onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
            return;
          }
          try {
            const chunk = JSON.parse(payload);
            // Detect server-side error in SSE stream
            if (chunk.error) {
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
          } catch {}
        }
      }
      // Stream ended without [DONE]
      const elapsed = performance.now() - t0;
      const totalTokens = tokenCount + reasoningTokenCount;
      const tps = totalTokens > 0 ? (totalTokens / (elapsed / 1000)).toFixed(1) : '0';
      const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
      onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
    } catch (err) {
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
