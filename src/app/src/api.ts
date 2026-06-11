/**
 * Lemonade API client — typed wrapper around the lemond HTTP server.
 * Handles connection management, SSE streaming for chat completions
 * and model downloads, and health polling.
 */

import { recipeOptionsForModel, samplingForModel } from './presetStore';

const DEFAULT_BASE_URL = 'http://localhost:13305';
const LS_BASE_URL = 'lemonade_base_url';
const LS_API_KEY = 'lemonade_api_key';

export interface LemonadeRequestError extends Error {
  status?: number;
  url?: string;
  endpoint?: string;
  userMessage?: string;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export function normalizeBaseUrl(raw: string): string {
  const trimmed = raw.trim();
  if (!trimmed) throw new Error('Server URL is required.');
  let parsed: URL;
  try {
    parsed = new URL(trimmed);
  } catch {
    throw new Error(`Invalid server URL: ${trimmed}`);
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new Error('Server URL must start with http:// or https://.');
  }
  return parsed.toString().replace(/\/+$/, '');
}

export function friendlyErrorMessage(err: unknown): string {
  const e = err as LemonadeRequestError;
  if (e?.userMessage) return e.userMessage;
  if (e?.message) return e.message;
  return String(err || 'Unknown error');
}

function normalizeLoadedModel(model: unknown): LoadedModel | null {
  if (!isObject(model)) return null;
  const modelName = String(model.model_name || model.name || '').trim();
  if (!modelName) return null;
  return {
    model_name: modelName,
    checkpoint: String(model.checkpoint || ''),
    recipe: String(model.recipe || ''),
    device: String(model.device || ''),
    backend_url: String(model.backend_url || ''),
    pid: Number(model.pid || 0),
    type: String(model.type || 'unknown').toLowerCase(),
    last_use: Number(model.last_use || Date.now()),
    recipe_options: isObject(model.recipe_options) ? model.recipe_options : undefined,
  };
}

function normalizeHealth(data: unknown): HealthData {
  const obj = isObject(data) ? data : {};
  const loadedRaw = Array.isArray(obj.all_models_loaded) ? obj.all_models_loaded : [];
  const loaded = loadedRaw.map(normalizeLoadedModel).filter((m): m is LoadedModel => !!m);
  return {
    status: String(obj.status || 'unknown'),
    version: String(obj.version || 'unknown'),
    model_loaded: typeof obj.model_loaded === 'string' ? obj.model_loaded : null,
    websocket_port: Number(obj.websocket_port || 0),
    all_models_loaded: loaded,
    max_models: isObject(obj.max_models) ? obj.max_models as Record<string, number> : {},
  };
}

function normalizeModels(data: unknown): ModelsData {
  const obj = isObject(data) ? data : {};
  return { data: Array.isArray(obj.data) ? obj.data as ModelInfo[] : [] };
}

function blobFromDataUrl(dataUrl: string): Blob {
  const match = /^data:([^;,]+)?(;base64)?,(.*)$/s.exec(dataUrl);
  if (!match) throw new Error('Expected an image data URL for editing.');
  const mime = match[1] || 'image/png';
  const isBase64 = Boolean(match[2]);
  const payload = match[3] || '';
  const binary = isBase64 ? atob(payload) : decodeURIComponent(payload);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return new Blob([bytes], { type: mime });
}

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
  signal?: AbortSignal;
}

export interface DownloadProgressEvent {
  id?: string;
  type?: 'model' | 'backend' | string;
  model_name?: string;
  name?: string;
  status?: 'downloading' | 'paused' | 'completed' | 'error' | 'cancelled' | string;
  running?: boolean;
  file?: string;
  file_index?: number;
  total_files?: number;
  bytes_downloaded?: number;
  bytes_total?: number;
  percent?: number;
  total_download_size?: number;
  cumulative_bytes_downloaded?: number;
  overall_bytes_downloaded?: number;
  completed_files_bytes?: number;
  complete?: boolean;
  error?: string;
  [key: string]: unknown;
}

export interface PullVariant {
  name: string;
  primary_file: string;
  files: string[];
  sharded: boolean;
  size_bytes: number;
}

export interface PullVariantsResult {
  checkpoint: string;
  recipe: string;
  repo_kind: string;
  suggested_name: string;
  suggested_labels: string[];
  mmproj_files: string[];
  variants: PullVariant[];
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

export interface RealtimeTranscriptionCallbacks {
  onConnected?: () => void;
  onDisconnected?: () => void;
  onError?: (message: string) => void;
  onSpeechEvent?: (event: 'started' | 'stopped') => void;
  onTranscription?: (text: string, isFinal: boolean) => void;
  onAudioBufferCleared?: () => void;
}

export interface RealtimeTranscriptionHandle {
  sendAudio: (base64Audio: string) => void;
  commitAudio: () => void;
  clearAudio: () => void;
  close: () => void;
  isConnected: () => boolean;
}

export type LemonadeRequestInit = Omit<RequestInit, 'body'> & { body?: unknown };

export type ChatMessageContent = string | null | Array<
  | { type: 'text'; text: string }
  | { type: 'image_url'; image_url: { url: string } }
  | { type: 'input_audio'; input_audio: { data: string; format: string } }
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
  private _lastConnectionError: string | null = null;
  private _listeners: StatusListener[] = [];
  private _modelsChangedListeners: Array<() => void> = [];
  private _healthData: HealthData | null = null;
  private _modelsData: ModelsData | null = null;
  private _systemInfoData: Record<string, unknown> | null = null;
  private _pollTimer: ReturnType<typeof setInterval> | null = null;
  private _sessionApiKey = '';

  // ── Config (persisted in localStorage) ──────────────────────────

  get baseUrl(): string {
    try {
      return normalizeBaseUrl(localStorage.getItem(LS_BASE_URL) || DEFAULT_BASE_URL);
    } catch {
      return DEFAULT_BASE_URL;
    }
  }

  set baseUrl(url: string) {
    localStorage.setItem(LS_BASE_URL, normalizeBaseUrl(url));
  }

  get apiKey(): string {
    return this._sessionApiKey || localStorage.getItem(LS_API_KEY) || '';
  }

  set apiKey(key: string) {
    this._sessionApiKey = key;
    if (key) localStorage.setItem(LS_API_KEY, key);
    else localStorage.removeItem(LS_API_KEY);
  }

  setSessionApiKey(key: string): void {
    this._sessionApiKey = key;
    if (!key) localStorage.removeItem(LS_API_KEY);
  }

  clearStoredApiKey(): void {
    localStorage.removeItem(LS_API_KEY);
  }

  // ── Connection status ───────────────────────────────────────────

  get status(): ConnectionStatus { return this._status; }
  get lastConnectionError(): string | null { return this._lastConnectionError; }
  get isConnected(): boolean { return this._status === 'connected'; }
  get healthData(): HealthData | null { return this._healthData; }
  get modelsData(): ModelsData | null { return this._modelsData; }
  get systemInfoData(): Record<string, unknown> | null { return this._systemInfoData; }

  get loadedModels(): LoadedModel[] {
    return Array.isArray(this._healthData?.all_models_loaded) ? this._healthData!.all_models_loaded : [];
  }

  get allModels(): ModelInfo[] {
    return Array.isArray(this._modelsData?.data) ? this._modelsData!.data : [];
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

  private async _fetch(path: string, opts: LemonadeRequestInit = {}): Promise<Response> {
    const endpoint = path.startsWith('/') ? path : `/${path}`;
    const url = `${this.baseUrl}${endpoint}`;
    const extraHeaders = opts.headers instanceof Headers
      ? Object.fromEntries(opts.headers.entries())
      : (Array.isArray(opts.headers) ? Object.fromEntries(opts.headers) : (opts.headers as Record<string, string> | undefined));
    const headers = this._headers(extraHeaders);
    const method = (opts.method || 'GET').toUpperCase();

    let processedOpts: LemonadeRequestInit = { ...opts };
    if (opts.body && typeof opts.body === 'object' &&
        !(opts.body instanceof FormData) &&
        !(opts.body instanceof ReadableStream) &&
        !(opts.body instanceof Blob)) {
      headers['Content-Type'] = headers['Content-Type'] || 'application/json';
      processedOpts = { ...opts, body: JSON.stringify(opts.body) };
    }

    let resp: Response;
    try {
      resp = await fetch(url, { ...processedOpts, headers } as RequestInit);
    } catch (cause) {
      const err = new Error(`${method} ${url} could not be reached. ${cause instanceof Error ? cause.message : String(cause)}`) as LemonadeRequestError;
      err.url = url;
      err.endpoint = endpoint;
      err.userMessage = `Could not reach ${url}. Check that lemond is running and the URL is correct.`;
      throw err;
    }

    if (!resp.ok) {
      const text = await resp.text().catch(() => '');
      let serverMessage = text.trim();
      try {
        const parsed = JSON.parse(text);
        serverMessage = parsed?.error?.message || parsed?.message || serverMessage;
      } catch { /* plain text response */ }
      const statusText = resp.statusText || `HTTP ${resp.status}`;
      const err = new Error(`${method} ${url} failed with ${resp.status} ${statusText}${serverMessage ? `: ${serverMessage}` : ''}`) as LemonadeRequestError;
      err.status = resp.status;
      err.url = url;
      err.endpoint = endpoint;
      err.userMessage = `${url} returned ${resp.status} ${statusText}${serverMessage ? ` — ${serverMessage}` : ''}`;
      throw err;
    }
    return resp;
  }

  private async _json<T = unknown>(path: string, opts?: LemonadeRequestInit): Promise<T> {
    const resp = await this._fetch(path, opts);
    return resp.json() as Promise<T>;
  }

  private _buildWebSocketUrl(path: string, port?: number, query?: URLSearchParams): string {
    const url = new URL(this.baseUrl);
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    if (port !== undefined) url.port = String(port);
    url.pathname = path.startsWith('/') ? path : `/${path}`;

    const params = new URLSearchParams(query);
    if (this.apiKey) params.set('api_key', this.apiKey);
    url.search = params.toString();
    return url.toString();
  }

  private _openRealtimeSocket(
    wsUrl: string,
    model: string,
    callbacks: RealtimeTranscriptionCallbacks,
    timeoutMs = 5000,
  ): Promise<RealtimeTranscriptionHandle> {
    return new Promise((resolve, reject) => {
      let opened = false;
      let settled = false;
      const socket = new WebSocket(wsUrl);
      const send = (msg: object) => {
        if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(msg));
      };
      const handle: RealtimeTranscriptionHandle = {
        sendAudio: base64Audio => send({ type: 'input_audio_buffer.append', audio: base64Audio }),
        commitAudio: () => send({ type: 'input_audio_buffer.commit' }),
        clearAudio: () => send({ type: 'input_audio_buffer.clear' }),
        close: () => socket.close(1000, 'OK'),
        isConnected: () => socket.readyState === WebSocket.OPEN,
      };

      const timer = window.setTimeout(() => {
        if (settled) return;
        settled = true;
        socket.close();
        reject(new Error(`WebSocket connect timeout: ${wsUrl}`));
      }, timeoutMs);

      socket.addEventListener('open', () => {
        opened = true;
        if (!settled) {
          settled = true;
          window.clearTimeout(timer);
          resolve(handle);
        }
        send({ type: 'session.update', session: { model } });
      });

      socket.addEventListener('message', event => {
        try {
          const msg = JSON.parse(event.data);
          switch (msg.type) {
            case 'session.created':
              callbacks.onConnected?.();
              break;
            case 'input_audio_buffer.speech_started':
              callbacks.onSpeechEvent?.('started');
              break;
            case 'input_audio_buffer.speech_stopped':
              callbacks.onSpeechEvent?.('stopped');
              break;
            case 'input_audio_buffer.cleared':
              callbacks.onAudioBufferCleared?.();
              break;
            case 'conversation.item.input_audio_transcription.delta':
              if (typeof msg.delta === 'string') callbacks.onTranscription?.(msg.delta, false);
              break;
            case 'conversation.item.input_audio_transcription.completed':
              if (typeof msg.transcript === 'string') callbacks.onTranscription?.(msg.transcript, true);
              break;
            case 'error':
              callbacks.onError?.(msg.error?.message || 'Server error');
              break;
          }
        } catch (err) {
          callbacks.onError?.(`Invalid realtime payload: ${String(err)}`);
        }
      });

      socket.addEventListener('error', () => {
        if (!opened && !settled) {
          settled = true;
          window.clearTimeout(timer);
          reject(new Error(`WebSocket connect failed: ${wsUrl}`));
          return;
        }
        if (opened) callbacks.onError?.('WebSocket error');
      });

      socket.addEventListener('close', event => {
        if (!opened && !settled) {
          settled = true;
          window.clearTimeout(timer);
          reject(new Error(`WebSocket closed before opening: ${wsUrl}`));
          return;
        }
        if (opened && event.code !== 1000) {
          callbacks.onError?.(`WebSocket closed (code=${event.code}).`);
        }
        callbacks.onDisconnected?.();
      });
    });
  }

  private _openLogSocket(wsUrl: string, callbacks: LogStreamCallbacks, afterSeq?: number | null, suppressPreOpenErrors = false): Promise<WebSocket> {
    return new Promise((resolve, reject) => {
      let opened = false;
      let settled = false;
      const socket = new WebSocket(wsUrl);
      const timer = window.setTimeout(() => {
        if (settled) return;
        settled = true;
        socket.close();
        reject(new Error(`WebSocket connect timeout: ${wsUrl}`));
      }, 5000);

      socket.addEventListener('open', () => {
        opened = true;
        if (!settled) {
          settled = true;
          window.clearTimeout(timer);
          resolve(socket);
        }
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
        if (!opened && !settled) {
          settled = true;
          window.clearTimeout(timer);
          reject(new Error(`WebSocket connect failed: ${wsUrl}`));
          return;
        }
        if (!suppressPreOpenErrors || opened) callbacks.onError?.('WebSocket error');
      });

      socket.addEventListener('close', () => {
        if (!opened && !settled) {
          settled = true;
          window.clearTimeout(timer);
          reject(new Error(`WebSocket closed before opening: ${wsUrl}`));
          return;
        }
        if (opened) callbacks.onDisconnected?.();
      });
    });
  }

  // ── Endpoints ───────────────────────────────────────────────────

  async health(): Promise<HealthData> {
    const data = normalizeHealth(await this._json<unknown>('/api/v1/health'));
    this._healthData = data;
    this._lastConnectionError = null;
    this._setStatus('connected');
    return data;
  }

  async models(showAll = true): Promise<ModelsData> {
    const qs = showAll ? '?show_all=true' : '';
    const data = normalizeModels(await this._json<unknown>(`/api/v1/models${qs}`));
    this._modelsData = data;
    return data;
  }

  async modelDetail(id: string): Promise<ModelInfo> {
    return this._json<ModelInfo>(`/api/v1/models/${encodeURIComponent(id)}`);
  }

  async loadModel(modelName: string, recipeOptions?: Record<string, unknown>): Promise<unknown> {
    const stagedOptions = recipeOptionsForModel(modelName);
    const body: Record<string, unknown> = { model_name: modelName, ...(stagedOptions || {}), ...recipeOptions };
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

  // ── Capability-specific inference endpoints ────────────────────

  async imageGeneration(model: string, prompt: string, opts: Record<string, unknown> = {}): Promise<string[]> {
    const requestedSize = typeof opts.size === 'string' ? opts.size : '1024x1024';
    const data = await this._json<Record<string, any>>('/api/v1/images/generations', {
      method: 'POST',
      body: {
        ...opts,
        model,
        prompt,
        size: requestedSize,
        response_format: 'b64_json',
      },
    });
    const items = Array.isArray(data.data) ? data.data : [];
    const images = items
      .map((item: any) => item?.b64_json ? `data:image/png;base64,${item.b64_json}` : item?.url)
      .filter((url: unknown): url is string => typeof url === 'string' && url.length > 0);
    if (images.length === 0) throw new Error('Image endpoint returned no image data.');
    return images;
  }

  async imageUpscale(model: string, imageUrl: string): Promise<string> {
    const image = imageUrl.replace(/^data:image\/[^;]+;base64,/, '');
    const data = await this._json<Record<string, any>>('/api/v1/images/upscale', {
      method: 'POST',
      body: { model, image },
    });
    const item = Array.isArray(data.data) ? data.data[0] : null;
    const url = item?.b64_json ? `data:image/png;base64,${item.b64_json}` : item?.url;
    if (typeof url !== 'string' || url.length === 0) throw new Error('Upscale endpoint returned no image data.');
    return url;
  }

  async textToSpeech(model: string, input: string, voice = 'alloy', opts: Record<string, unknown> = {}): Promise<{ url: string; blob: Blob }> {
    const resp = await this._fetch('/api/v1/audio/speech', {
      method: 'POST',
      body: { ...opts, model, input, voice },
    });
    const blob = await resp.blob();
    return { blob, url: URL.createObjectURL(blob) };
  }

  async imageEdit(model: string, prompt: string, imageDataUrl: string, opts: Record<string, unknown> = {}): Promise<string[]> {
    const requestedSize = typeof opts.size === 'string' ? opts.size : '1024x1024';
    const imageBlob = blobFromDataUrl(imageDataUrl);
    const form = new FormData();
    form.append('model', model);
    form.append('prompt', prompt);
    form.append('size', requestedSize);
    form.append('response_format', 'b64_json');
    form.append('n', String(typeof opts.n === 'number' ? opts.n : 1));
    ['steps', 'cfg_scale', 'seed', 'sample_method', 'flow_shift'].forEach(key => {
      const value = opts[key];
      if (typeof value === 'number' || (typeof value === 'string' && value.trim())) {
        form.append(key, String(value));
      }
    });
    form.append('image', imageBlob, 'image.png');

    const data = await this._json<Record<string, any>>('/api/v1/images/edits', {
      method: 'POST',
      body: form,
    });
    const items = Array.isArray(data.data) ? data.data : [];
    const images = items
      .map((item: any) => item?.b64_json ? `data:image/png;base64,${item.b64_json}` : item?.url)
      .filter((url: unknown): url is string => typeof url === 'string' && url.length > 0);
    if (images.length === 0) throw new Error('Image edit endpoint returned no image data.');
    return images;
  }

  async audioTranscription(model: string, file: File): Promise<string> {
    const form = new FormData();
    form.append('file', file);
    form.append('model', model);
    const data = await this._json<Record<string, unknown>>('/api/v1/audio/transcriptions', {
      method: 'POST',
      body: form,
    });
    const text = typeof data.text === 'string' ? data.text : '';
    if (!text) throw new Error('Transcription endpoint returned no text.');
    return text;
  }

  // ── Dashboard data ──────────────────────────────────────────────

  async stats(): Promise<StatsData> {
    return this._json<StatsData>('/api/v1/stats');
  }

  async systemStats(): Promise<SystemStatsData> {
    return this._json<SystemStatsData>('/api/v1/system-stats');
  }

  async slots(): Promise<SlotData[]> {
    const data = await this._json<unknown>('/api/v1/slots');
    if (Array.isArray(data)) return data as SlotData[];
    if (isObject(data) && Array.isArray(data.slots)) return data.slots as SlotData[];
    return [];
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
    let closed = false;
    let socket: WebSocket | null = null;

    // Logs belong to the configured Lemonade API origin. Do not use
    // health.websocket_port here: on current Lemonade builds that port is used
    // by realtime audio, so subscribing to logs there creates noisy background
    // connections without ever receiving log entries.
    const wsUrl = this._buildWebSocketUrl('/logs/stream');
    this._openLogSocket(wsUrl, callbacks, afterSeq, true)
      .then(openedSocket => {
        if (closed) {
          openedSocket.close(1000, 'OK');
          return;
        }
        socket = openedSocket;
      })
      .catch(() => {
        if (!closed) callbacks.onError?.('Could not connect to log stream on the Lemonade API port.');
      });

    return {
      close: () => {
        closed = true;
        socket?.close(1000, 'OK');
      },
    };
  }

  async connectRealtimeTranscription(model: string, callbacks: RealtimeTranscriptionCallbacks = {}): Promise<RealtimeTranscriptionHandle> {
    const query = new URLSearchParams({ model });
    const mainUrl = this._buildWebSocketUrl('/v1/realtime', undefined, query);
    try {
      return await this._openRealtimeSocket(mainUrl, model, callbacks);
    } catch {
      const health = this._healthData || await this.health();
      if (!health.websocket_port) throw new Error('Server did not advertise a realtime WebSocket port.');
      const legacyUrl = this._buildWebSocketUrl('/realtime', health.websocket_port, query);
      return this._openRealtimeSocket(legacyUrl, model, callbacks);
    }
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

  // ── Persistent downloads ───────────────────────────────────────

  async downloads(): Promise<DownloadProgressEvent[]> {
    const data = await this._json<unknown>('/api/v1/downloads', { cache: 'no-store' } as LemonadeRequestInit);
    if (Array.isArray(data)) return data as DownloadProgressEvent[];
    if (isObject(data) && Array.isArray(data.downloads)) return data.downloads as DownloadProgressEvent[];
    return [];
  }

  async controlDownload(downloadId: string, action: 'pause' | 'cancel' | 'remove'): Promise<unknown> {
    const result = await this._json('/api/v1/downloads/control', {
      method: 'POST',
      body: { id: downloadId, action },
    });
    this._notifyModelsChanged();
    return result;
  }

  private _downloadModelName(download: DownloadProgressEvent): string {
    const id = typeof download.id === 'string' ? download.id : '';
    const modelName = String(download.model_name || download.name || '').trim();
    if (modelName) return modelName;
    return id.startsWith('model:') ? id.slice('model:'.length) : id;
  }

  private _isMatchingModelDownload(download: DownloadProgressEvent, modelName: string): boolean {
    const target = modelName.trim().toLowerCase();
    const candidate = this._downloadModelName(download).trim().toLowerCase();
    const id = String(download.id || '').toLowerCase();
    return candidate === target || id === `model:${target}` || id.endsWith(`:${target}`);
  }

  // ── Pull variants (HF model file discovery) ────────────────────

  async pullVariants(checkpoint: string): Promise<PullVariantsResult> {
    return this._json(`/api/v1/pull/variants?checkpoint=${encodeURIComponent(checkpoint)}`);
  }

  // ── SSE: Pull (model download) ──────────────────────────────────

  async pullModel(modelName: string, callbacks: PullCallbacks = {}, opts?: Record<string, unknown>): Promise<void> {
    const { onProgress, onComplete, onError, signal } = callbacks;
    try {
      const body: Record<string, unknown> = {
        ...(opts || {}),
        model_name: modelName,
        stream: true,
        // Let lemond own the download so progress survives F5/new tabs.
        // Browser SSE is still supported below for older servers.
        subscribe: false,
      };
      const resp = await this._fetch('/api/v1/pull', {
        method: 'POST',
        body,
        signal,
      });

      const contentType = resp.headers.get('content-type') || '';
      if (!contentType.includes('text/event-stream')) {
        const initial = await resp.json().catch(() => ({} as Record<string, unknown>)) as Record<string, unknown>;
        onProgress?.({ percent: typeof initial.percent === 'number' ? initial.percent : 0, ...initial });
        const started = performance.now();
        let lastMatch: DownloadProgressEvent | null = null;
        while (!signal?.aborted) {
          await new Promise(resolve => setTimeout(resolve, 1000));
          const downloads = await this.downloads().catch(() => []);
          const match = downloads.find(download => this._isMatchingModelDownload(download, modelName));
          if (match) {
            lastMatch = match;
            const percent = typeof match.percent === 'number' ? match.percent : undefined;
            onProgress?.({ ...match, percent });
            if (match.error || match.status === 'error') {
              throw new Error(String(match.error || `Download failed for ${modelName}.`));
            }
            if (match.status === 'cancelled') return;
            if (match.complete || match.status === 'completed') {
              onComplete?.({ ...match });
              this._notifyModelsChanged();
              return;
            }
          } else if (lastMatch && (lastMatch.complete || lastMatch.status === 'completed')) {
            onComplete?.({ ...lastMatch });
            this._notifyModelsChanged();
            return;
          } else if (!lastMatch && performance.now() - started > 3000) {
            // Some lemond versions only acknowledge a registration/pull that is immediately complete.
            onComplete?.(initial);
            this._notifyModelsChanged();
            return;
          }
        }
        return;
      }

      const reader = resp.body!.getReader();
      const dec = new TextDecoder();
      let buf = '';

      try {
        while (true) {
          if (signal?.aborted) {
            await reader.cancel();
            break;
          }
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
      } finally {
        if (signal?.aborted) {
          reader.cancel().catch(() => {});
        }
      }
      if (!signal?.aborted) {
        onComplete?.({});
        this._notifyModelsChanged();
      }
    } catch (err) {
      if ((err as Error).name === 'AbortError') return;
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
      const body: Record<string, unknown> = { model, messages, stream: true, ...samplingForModel(model), ...(params || {}) };
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

  async chatCompletionOnce(
    model: string,
    messages: ChatMessage[],
    params: Record<string, unknown> = {},
  ): Promise<string> {
    const data = await this._json<Record<string, any>>('/api/v1/chat/completions', {
      method: 'POST',
      body: { model, messages, stream: false, ...samplingForModel(model), ...params },
    });
    if (data.error) {
      throw new Error(data.error?.message || 'Chat completion failed');
    }
    const message = data.choices?.[0]?.message;
    const content = message?.content;
    if (typeof content === 'string') return content;
    if (Array.isArray(content)) {
      return content.map((part: any) => typeof part?.text === 'string' ? part.text : '').join('').trim();
    }
    return '';
  }

  // ── Connection management ───────────────────────────────────────

  async connect(): Promise<boolean> {
    this._setStatus('connecting');
    try {
      await this.health();
      return true;
    } catch (err) {
      this._lastConnectionError = friendlyErrorMessage(err);
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
    } catch (err) {
      this._lastConnectionError = friendlyErrorMessage(err);
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
  createdAt?: string;
  pipeline_tag?: string;
}

export async function searchHuggingFace(
  query: string,
  signal?: AbortSignal,
): Promise<HFModelResult[]> {
  if (typeof navigator !== 'undefined' && navigator.onLine === false) {
    throw new Error('Browser is offline; HuggingFace search is unavailable.');
  }
  const params = new URLSearchParams({
    search: query,
    filter: 'gguf',
    sort: 'downloads',
    direction: '-1',
    limit: '20',
  });
  // Variant/file details are fetched on demand via pullVariants()
  const resp = await fetch(
    `https://huggingface.co/api/models?${params}`,
    { signal },
  );
  if (!resp.ok) {
    throw new Error(`HuggingFace search failed with HTTP ${resp.status}`);
  }
  const data = await resp.json();
  return Array.isArray(data) ? data : [];
}
