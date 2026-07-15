/**
 * Lemonade API client — typed wrapper around the lemond HTTP server.
 * Handles connection management, SSE streaming for chat completions
 * and model downloads, and health polling.
 */

import { recipeOptionsForModel, samplingForModel, type RecipeOptions } from './presetStore';
import { COLLECTION_IMAGE_SIZE } from './features/collections/collectionImageConfig';

function detectDefaultBaseUrl(): string {
  if (typeof window !== 'undefined' && window.location) {
    // When served by lemond at /app, window.location.origin IS the API server.
    // Detect this by checking for the lemond-injected window.api shim or /app path.
    const servedByLemond = window.location.pathname.startsWith('/app')
      || (window as any).api?.getServerPort;
    if (servedByLemond && window.location.port) {
      return `${window.location.protocol}//${window.location.hostname}:${window.location.port}`;
    }
  }
  return 'http://127.0.0.1:13305';
}

const DEFAULT_BASE_URL = process.env.LEMONADE_BASE_URL || detectDefaultBaseUrl();
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

function modelInfoKey(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  return String((model as any).model_name || model.name || model.id || '').trim();
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

/** One physical file backing a model (from GET /api/v1/models/{id}/files). */
export interface ModelFileInfo {
  name: string;
  role: string;
  size_bytes: number;
  exists: boolean;
}

/** Response shape of GET /api/v1/models/{id}/files. */
export interface ModelFilesResponse {
  model_id: string;
  files: ModelFileInfo[];
}

/** Real disk usage for the model-storage drive (bytes). */
export interface StorageInfo {
  usedBytes: number;
  totalBytes: number;
}

export interface CloudProviderRow {
  name: string;
  base_url: string;
  env_var: string;
  env_var_set: boolean;
  runtime_key_set: boolean;
  models_discovered: number;
}

export interface DirectorySettings {
  modelsDir: string;
  extraModelsDir: string;
  canPersist: boolean;
}

export interface ChatCompletionStats {
  content: string;
  reasoning: string;
  id: string | null;
  tps: string;
  ttft: string | null;
  tokens: number;
  reasoningTokens: number;
  reasoningElapsedMs: number | null;
}

export interface LiveStreamStats {
  tps: number;
  tokens: number;
  reasoningTokens: number;
  reasoningElapsedMs: number | null;
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
  created_at?: number | string;
  createdAt?: number | string;
  started_at?: number | string;
  startedAt?: number | string;
  start_time?: number | string;
  startTime?: number | string;
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

function downloadPayloadStatus(data: unknown): string {
  if (!isObject(data)) return '';
  return String(data.status || '').toLowerCase();
}

function downloadPayloadErrorMessage(data: unknown): string | null {
  if (!isObject(data)) return null;
  const status = downloadPayloadStatus(data);
  const message = data.message || data.detail;
  const statusCode = Number(
    data.status_code
    ?? data.statusCode
    ?? data.http_status
    ?? data.httpStatus
    ?? data.code
    ?? data.error_code
    ?? data.errorCode
    ?? (/^\d{3}$/.test(status) ? status : NaN),
  );
  const messageText = typeof message === 'string' ? message.trim() : '';

  const rawError = data.error;
  let errorText = '';
  if (typeof rawError === 'string' && rawError.trim()) {
    errorText = rawError.trim();
  } else if (isObject(rawError)) {
    const nested = rawError.message || rawError.error || rawError.detail;
    if (typeof nested === 'string' && nested.trim()) errorText = nested.trim();
  }

  const textLooksLike404 = /(^|\D)404(\D|$)|not[ _-]?found/i.test(`${status} ${messageText} ${errorText}`);
  const failed = Boolean(errorText)
    || status === 'error'
    || status === 'failed'
    || status === 'failure'
    || status === 'not_found'
    || status === 'not-found'
    || data.ok === false
    || (Number.isFinite(statusCode) && statusCode >= 400)
    || textLooksLike404;

  if (!failed) return null;
  const detail = errorText || messageText;
  if (Number.isFinite(statusCode) && statusCode >= 400) {
    if (detail && !new RegExp(`(^|\\D)${statusCode}(\\D|$)`).test(detail)) {
      return `HTTP ${statusCode}: ${detail}`;
    }
    return detail || `Download failed with HTTP ${statusCode}.`;
  }
  return detail || 'Download failed.';
}

function downloadPayloadCompleted(data: unknown): boolean {
  if (!isObject(data)) return false;
  if (downloadPayloadErrorMessage(data)) return false;
  const status = downloadPayloadStatus(data);
  return data.complete === true || status === 'completed' || status === 'complete' || status === 'success' || status === 'done';
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

export type JobStatus = 'queued' | 'running' | 'paused' | 'interrupted' | 'completed' | 'failed';

export interface JobStep {
  id: string;
  op: string;
  status: 'pending' | 'running' | 'completed' | 'failed' | 'skipped' | string;
  duration_ms?: number;
  error?: string;
  output?: unknown;
}

export interface JobRecord {
  id: string;
  name: string;
  status: JobStatus;
  inputs?: Record<string, unknown>;
  context: Record<string, unknown>;
  cursor?: string;
  steps: JobStep[];
  created_at?: string;
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

export type LemonadeRequestInit = Omit<RequestInit, 'body'> & {
  body?: unknown;
  includeSessionHeaders?: boolean;
};

export type LemonadeContextDefault = number | 'auto';

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


type HostSettingsApi = {
  getSettings?: () => Promise<unknown>;
  saveSettings?: (settings: unknown) => Promise<unknown>;
  getServerBaseUrl?: () => Promise<string | null | undefined>;
  getServerAPIKey?: () => Promise<string | null | undefined>;
};

function getHostSettingsApi(): HostSettingsApi | null {
  if (typeof window === 'undefined') return null;
  return ((window as unknown as { api?: HostSettingsApi }).api || null);
}

async function waitForHostSettingsApi(): Promise<HostSettingsApi | null> {
  for (let i = 0; i < 20; i++) {
    const hostApi = getHostSettingsApi();
    if (hostApi) return hostApi;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  return null;
}

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

function typedStringSetting(value: string): { value: string; useDefault: boolean } {
  return { value, useDefault: value.trim().length === 0 };
}

function typedSettingString(settings: unknown, key: string): string {
  if (!isObject(settings)) return '';
  const raw = settings[key];
  if (!isObject(raw)) return '';
  return typeof raw.value === 'string' ? raw.value : '';
}

function mergeConnectionSettings(settings: unknown, baseUrl: string, apiKey: string): Record<string, unknown> {
  const current = isObject(settings) ? { ...settings } : {};
  return {
    ...current,
    baseURL: typedStringSetting(baseUrl),
    apiKey: typedStringSetting(apiKey),
  };
}

function mergeDirectorySettings(settings: unknown, modelsDir: string, extraModelsDir: string): Record<string, unknown> {
  const current = isObject(settings) ? { ...settings } : {};
  const modelsDirSetting = typedStringSetting(modelsDir);
  const extraModelsDirSetting = typedStringSetting(extraModelsDir);
  return {
    ...current,
    // Keep both camelCase and snake_case keys so the redesign can interoperate
    // with host settings bridges while the server-side naming settles.
    modelsDir: modelsDirSetting,
    models_dir: modelsDirSetting,
    extraModelsDir: extraModelsDirSetting,
    extra_models_dir: extraModelsDirSetting,
  };
}

function normalizeCloudProviderRow(value: unknown): CloudProviderRow | null {
  if (!isObject(value)) return null;
  const name = String(value.name || '').trim();
  if (!name) return null;
  return {
    name,
    base_url: typeof value.base_url === 'string' ? value.base_url : '',
    env_var: typeof value.env_var === 'string' ? value.env_var : '',
    env_var_set: value.env_var_set === true,
    runtime_key_set: value.runtime_key_set === true,
    models_discovered: typeof value.models_discovered === 'number' ? value.models_discovered : 0,
  };
}

export interface ConnectionSettingsSaveResult {
  apiKeyPersisted: boolean;
}

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
  private _hostBaseUrl: string | null = null;
  private _connectionSettingsPromise: Promise<void> | null = null;
  public readonly clientSessionId: string = (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function')
    ? crypto.randomUUID()
    : Array.from({ length: 32 }, () => Math.floor(Math.random() * 16).toString(16)).join('');
  public sessionHeadersEnabled = false;
  public onSessionHeadersFailed?: () => void;

  // ── Config ──────────────────────────────────────────────────────
  // Non-secret connection settings may be persisted in browser storage.
  // API keys are never stored in localStorage; in the desktop app they are
  // persisted through Lemonade's native settings bridge (`window.api.saveSettings`).

  get baseUrl(): string {
    try {
      const hasExplicitUrl = Boolean(this._hostBaseUrl || safeGetLocalStorage(LS_BASE_URL));
      const raw = normalizeBaseUrl(this._hostBaseUrl || safeGetLocalStorage(LS_BASE_URL) || DEFAULT_BASE_URL);
      // On mobile, window.location.hostname is the PC's LAN IP (e.g. 192.168.3.35).
      // Substitute it when the configured host is localhost/127.0.0.1 so that all
      // API calls and WebSocket connections resolve to the serving machine, not the phone.
      const parsed = new URL(raw);
      const hasBuildTimeOverride = Boolean(process.env.LEMONADE_BASE_URL);
      if (!hasExplicitUrl && !hasBuildTimeOverride && (parsed.hostname === 'localhost' || parsed.hostname === '127.0.0.1') && typeof window !== 'undefined') {
        const winHost = window.location.hostname;
        if (winHost && winHost !== 'localhost' && winHost !== '127.0.0.1' && winHost !== '[::1]' && winHost !== '::1') {
          parsed.hostname = winHost;
        }
      }
      return parsed.toString().replace(/\/+$/, '');
    } catch {
      return DEFAULT_BASE_URL;
    }
  }

  set baseUrl(url: string) {
    const normalized = normalizeBaseUrl(url);
    this._hostBaseUrl = normalized;
    safeSetLocalStorage(LS_BASE_URL, normalized);
  }

  get apiKey(): string {
    safeRemoveLocalStorage(LS_API_KEY);
    return this._sessionApiKey;
  }

  set apiKey(key: string) {
    this.setSessionApiKey(key);
  }

  get canPersistApiKey(): boolean {
    const hostApi = getHostSettingsApi();
    return Boolean(hostApi?.getSettings && hostApi?.saveSettings);
  }

  async loadConnectionSettings(): Promise<void> {
    safeRemoveLocalStorage(LS_API_KEY);
    if (this._connectionSettingsPromise) return this._connectionSettingsPromise;

    this._connectionSettingsPromise = (async () => {
      const hostApi = await waitForHostSettingsApi();
      if (!hostApi) return;

      if (hostApi.getSettings) {
        const settings = await hostApi.getSettings();
        const baseUrl = typedSettingString(settings, 'baseURL');
        const apiKey = typedSettingString(settings, 'apiKey');
        // Fallback: web-app mock provides apiUrl as a plain string (not typed setting)
        const fallbackUrl = !baseUrl.trim() && isObject(settings) && typeof (settings as any).apiUrl === 'string'
          ? (settings as any).apiUrl.trim()
          : '';
        const resolvedUrl = baseUrl.trim() || fallbackUrl;
        if (resolvedUrl) {
          this._hostBaseUrl = normalizeBaseUrl(resolvedUrl);
          safeSetLocalStorage(LS_BASE_URL, this._hostBaseUrl);
        }
        this._sessionApiKey = apiKey;
        return;
      }

      if (hostApi.getServerBaseUrl) {
        const baseUrl = await hostApi.getServerBaseUrl();
        if (typeof baseUrl === 'string' && baseUrl.trim()) {
          this._hostBaseUrl = normalizeBaseUrl(baseUrl);
          safeSetLocalStorage(LS_BASE_URL, this._hostBaseUrl);
        }
      }
      if (hostApi.getServerAPIKey) {
        const apiKey = await hostApi.getServerAPIKey();
        this._sessionApiKey = typeof apiKey === 'string' ? apiKey : '';
      }
    })().catch(err => {
      this._connectionSettingsPromise = null;
      throw err;
    });

    return this._connectionSettingsPromise;
  }

  async saveConnectionSettings(baseUrl: string, apiKey: string, rememberApiKey: boolean): Promise<ConnectionSettingsSaveResult> {
    const normalized = normalizeBaseUrl(baseUrl);
    this._hostBaseUrl = normalized;
    this._sessionApiKey = apiKey;
    safeSetLocalStorage(LS_BASE_URL, normalized);
    safeRemoveLocalStorage(LS_API_KEY);

    const hostApi = await waitForHostSettingsApi();
    if (!hostApi?.getSettings || !hostApi?.saveSettings) {
      return { apiKeyPersisted: false };
    }

    const persistedApiKey = rememberApiKey ? apiKey : '';
    const currentSettings = await hostApi.getSettings();
    await hostApi.saveSettings(mergeConnectionSettings(currentSettings, normalized, persistedApiKey));
    return { apiKeyPersisted: Boolean(persistedApiKey) };
  }

  async clearConnectionSettings(): Promise<void> {
    this._hostBaseUrl = null;
    this._sessionApiKey = '';
    safeRemoveLocalStorage(LS_BASE_URL);
    safeRemoveLocalStorage(LS_API_KEY);

    const hostApi = await waitForHostSettingsApi();
    if (hostApi?.getSettings && hostApi?.saveSettings) {
      const currentSettings = await hostApi.getSettings();
      await hostApi.saveSettings(mergeConnectionSettings(currentSettings, '', ''));
    }
  }

  async loadDirectorySettings(): Promise<DirectorySettings> {
    const hostApi = await waitForHostSettingsApi();
    if (!hostApi?.getSettings || !hostApi?.saveSettings) {
      return { modelsDir: '', extraModelsDir: '', canPersist: false };
    }
    const settings = await hostApi.getSettings();
    return {
      modelsDir: typedSettingString(settings, 'modelsDir') || typedSettingString(settings, 'models_dir'),
      extraModelsDir: typedSettingString(settings, 'extraModelsDir') || typedSettingString(settings, 'extra_models_dir'),
      canPersist: true,
    };
  }

  async saveDirectorySettings(modelsDir: string, extraModelsDir: string): Promise<DirectorySettings> {
    const hostApi = await waitForHostSettingsApi();
    if (!hostApi?.getSettings || !hostApi?.saveSettings) {
      return { modelsDir, extraModelsDir, canPersist: false };
    }
    const currentSettings = await hostApi.getSettings();
    await hostApi.saveSettings(mergeDirectorySettings(currentSettings, modelsDir, extraModelsDir));
    return { modelsDir, extraModelsDir, canPersist: true };
  }

  setSessionApiKey(key: string): void {
    this._sessionApiKey = key;
    safeRemoveLocalStorage(LS_API_KEY);
  }

  clearStoredApiKey(): void {
    safeRemoveLocalStorage(LS_API_KEY);
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

  private _headers(extra?: Record<string, string>, includeSessionHeaders?: boolean): Record<string, string> {
    const h: Record<string, string> = { ...(extra || {}) };
    if (this.apiKey) h['Authorization'] = `Bearer ${this.apiKey}`;

    if (includeSessionHeaders && this.sessionHeadersEnabled) {
      h['X-Client-Session-Id'] = this.clientSessionId;

      // Add current account session token or guest ID to scope model caches
      try {
        const raw = localStorage.getItem('lemonade_account_session_v1') || sessionStorage.getItem('lemonade_account_session_v1');
        if (raw) {
          const parsed = JSON.parse(raw) as { id?: string };
          if (parsed.id) {
            h['X-Account-Session-Id'] = parsed.id;
          } else {
            h['X-Account-Session-Id'] = 'guest';
          }
        } else {
          h['X-Account-Session-Id'] = 'guest';
        }
      } catch {
        h['X-Account-Session-Id'] = 'guest';
      }
    }

    return h;
  }

  private async _fetch(path: string, opts: LemonadeRequestInit = {}): Promise<Response> {
    const endpoint = path.startsWith('/') ? path : `/${path}`;
    const url = `${this.baseUrl}${endpoint}`;
    const extraHeaders = opts.headers instanceof Headers
      ? Object.fromEntries(opts.headers.entries())
      : (Array.isArray(opts.headers) ? Object.fromEntries(opts.headers) : (opts.headers as Record<string, string> | undefined));
    const headers = this._headers(extraHeaders, opts.includeSessionHeaders);
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
      if (opts.includeSessionHeaders && this.sessionHeadersEnabled) {
        this.sessionHeadersEnabled = false;
        this.onSessionHeadersFailed?.();
        try {
          const fallbackHeaders = this._headers(extraHeaders, false);
          if (processedOpts.body && headers['Content-Type'] === 'application/json') {
            fallbackHeaders['Content-Type'] = 'application/json';
          }
          resp = await fetch(url, { ...processedOpts, headers: fallbackHeaders } as RequestInit);
        } catch {
          const err = new Error(`${method} ${url} could not be reached. ${cause instanceof Error ? cause.message : String(cause)}`) as LemonadeRequestError;
          err.url = url;
          err.endpoint = endpoint;
          err.userMessage = `Could not reach ${url}. Check that lemond is running and the URL is correct.`;
          throw err;
        }
      } else {
        const err = new Error(`${method} ${url} could not be reached. ${cause instanceof Error ? cause.message : String(cause)}`) as LemonadeRequestError;
        err.url = url;
        err.endpoint = endpoint;
        err.userMessage = `Could not reach ${url}. Check that lemond is running and the URL is correct.`;
        throw err;
      }
    }

    if (!resp.ok) {
      if (opts.includeSessionHeaders && this.sessionHeadersEnabled && (resp.status === 400 || resp.status === 401 || resp.status === 403 || resp.status === 405)) {
        this.sessionHeadersEnabled = false;
        this.onSessionHeadersFailed?.();
        try {
          const fallbackHeaders = this._headers(extraHeaders, false);
          if (processedOpts.body && headers['Content-Type'] === 'application/json') {
            fallbackHeaders['Content-Type'] = 'application/json';
          }
          const retryResp = await fetch(url, { ...processedOpts, headers: fallbackHeaders } as RequestInit);
          if (retryResp.ok) {
            resp = retryResp;
          }
        } catch {}
      }
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
    params.set('client_session_id', this.clientSessionId);
    try {
      const raw = localStorage.getItem('lemonade_account_session_v1') || sessionStorage.getItem('lemonade_account_session_v1');
      if (raw) {
        const parsed = JSON.parse(raw) as { id?: string };
        if (parsed.id) {
          params.set('account_session_id', parsed.id);
        } else {
          params.set('account_session_id', 'guest');
        }
      } else {
        params.set('account_session_id', 'guest');
      }
    } catch {
      params.set('account_session_id', 'guest');
    }
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

  /**
   * List the physical files backing a model (main weights, mmproj, tokenizer, …).
   * Returns null when the endpoint is unreachable or the model is unknown, so the
   * UI can fall back to an empty/error state instead of throwing.
   */
  async getModelFiles(id: string): Promise<ModelFilesResponse | null> {
    try {
      const data = await this._json<ModelFilesResponse>(
        `/api/v1/models/${encodeURIComponent(id)}/files`,
      );
      if (!data || !Array.isArray(data.files)) return null;
      return data;
    } catch {
      return null;
    }
  }

  async loadModel(modelName: string, recipeOptions?: Record<string, unknown>, modelInfo?: ModelInfo | null): Promise<unknown> {
    const target = modelName.trim().toLowerCase();
    const cachedModelInfo = modelInfo || this.allModels.find(model => modelInfoKey(model).toLowerCase() === target) || null;
    const stagedOptions = recipeOptionsForModel(modelName, cachedModelInfo, recipeOptions as RecipeOptions | undefined, this._systemInfoData);
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

  /**
   * Apply a *load-time* preset change to an already-loaded model (#2356).
   *
   * Simplified design (per @fl0rianr review + Lovell): there is NO dedicated
   * update-preset endpoint and NO client-provided `mode` parameter — the UI is
   * not the source of truth for runtime capability. Load-time fields (ctx_size,
   * backend, device, model args via recipe_options) require a real reload, which
   * today is literally an unload followed by a load, exactly as `main` does.
   *
   * This helper is named `reloadModel` (rather than inlining unload→load at every
   * call site) so that if a real in-place backend reload ever lands, only this
   * method's body changes; callers and tests stay identical.
   *
   * Request-time fields (system_prompt, sampling/temperature, tools) are NOT
   * handled here — rebinding the active preset is the whole "live" operation and
   * request composition (`samplingForModel`, `systemPromptTextForPreset`) carries
   * the new values on the next generation request; nothing is POSTed.
   */
  async reloadModel(
    modelName: string,
    recipeOptions?: Record<string, unknown>,
    modelInfo?: ModelInfo | null,
  ): Promise<unknown> {
    await this.unloadModel(modelName);
    return this.loadModel(modelName, recipeOptions, modelInfo);
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
    const data = await this._json<Record<string, unknown>>(
      '/api/v1/system-info',
      { cache: 'no-store' } as LemonadeRequestInit,
    );
    this._systemInfoData = data;
    return data;
  }

  /**
   * Real used/total bytes of the drive where models are stored.
   *
   * POC LIMITATION (fl0rianr #2424): lemond is OFF LIMITS and exposes no
   * disk-usage field today (verified: /system-info and /system-stats lack it).
   * This probes system-info for a future `storage` / `disk` shape so the meter
   * lights up the moment the backend team adds it — no client changes needed.
   * Returns null when no real source is reachable, letting the UI fall back to a
   * derived estimate instead of a hardcoded mock.
   */
  async getStorageInfo(): Promise<StorageInfo | null> {
    try {
      const info = await this.systemInfo();
      const candidates = [
        (info as any).storage,
        (info as any).disk,
        (info as any).model_storage,
        (info as any).Storage,
      ];
      for (const c of candidates) {
        if (!isObject(c)) continue;
        const total = Number((c as any).total_bytes ?? (c as any).totalBytes ?? (c as any).total);
        const used = Number((c as any).used_bytes ?? (c as any).usedBytes ?? (c as any).used);
        if (Number.isFinite(total) && total > 0 && Number.isFinite(used) && used >= 0) {
          return { usedBytes: used, totalBytes: total };
        }
      }
    } catch {
      /* unreachable in the browser-served POC — fall through to null */
    }
    return null;
  }


  async cloudProviders(): Promise<CloudProviderRow[]> {
    const info = await this.systemInfo();
    const cloud = isObject(info.cloud) ? info.cloud : {};
    const providers = Array.isArray(cloud.providers) ? cloud.providers : [];
    return providers.map(normalizeCloudProviderRow).filter((row): row is CloudProviderRow => Boolean(row));
  }

  async installCloudProvider(provider: string, baseUrl: string, apiKey?: string): Promise<Record<string, unknown>> {
    const body: Record<string, string> = {
      backend: 'cloud',
      provider: provider.trim(),
      base_url: baseUrl.trim(),
    };
    if (apiKey?.trim()) body.api_key = apiKey.trim();
    const result = await this._json<Record<string, unknown>>('/api/v1/install', { method: 'POST', body });
    this._notifyModelsChanged();
    return result;
  }

  async setCloudProviderAuth(provider: string, apiKey: string): Promise<Record<string, unknown>> {
    const result = await this._json<Record<string, unknown>>('/api/v1/cloud/auth', {
      method: 'POST',
      body: { provider: provider.trim(), api_key: apiKey.trim() },
    });
    this._notifyModelsChanged();
    return result;
  }

  async clearCloudProviderAuth(provider: string): Promise<void> {
    await this._fetch(`/api/v1/cloud/auth/${encodeURIComponent(provider.trim())}`, { method: 'DELETE' });
    this._notifyModelsChanged();
  }

  async uninstallCloudProvider(provider: string): Promise<void> {
    await this._fetch('/api/v1/uninstall', {
      method: 'POST',
      body: { backend: 'cloud', provider: provider.trim() },
    });
    this._notifyModelsChanged();
  }

  // ── Capability-specific inference endpoints ────────────────────

  async imageGeneration(model: string, prompt: string, opts: Record<string, unknown> = {}): Promise<string[]> {
    const requestedSize = typeof opts.size === 'string' && opts.size.trim() ? opts.size.trim() : COLLECTION_IMAGE_SIZE;
    const data = await this._json<Record<string, any>>('/api/v1/images/generations', {
      method: 'POST',
      body: {
        ...opts,
        model,
        prompt,
        size: requestedSize,
        response_format: 'b64_json',
      },
      includeSessionHeaders: true,
    });
    const items = Array.isArray(data.data) ? data.data : [];
    const images = items
      .map((item: any) => item?.b64_json ? `data:image/png;base64,${item.b64_json}` : item?.url)
      .filter((url: unknown): url is string => typeof url === 'string' && url.length > 0);
    if (images.length === 0) throw new Error('Image endpoint returned no image data.');
    return images;
  }

  async audioGeneration(
    model: string,
    prompt: string,
    opts: Record<string, unknown> = {},
  ): Promise<{ url: string; blob: Blob; filename: string }> {
    const resp = await this._fetch('/api/v1/audio/generations', {
      method: 'POST',
      body: {
        ...opts,
        model,
        prompt,
        response_format: 'wav',
      },
      includeSessionHeaders: true,
    });
    const blob = await resp.blob();
    if (blob.size === 0) throw new Error('Audio generation endpoint returned an empty file.');
    return { blob, url: URL.createObjectURL(blob), filename: `${model}.wav` };
  }

  async model3dGeneration(
    model: string,
    image: string,
    opts: Record<string, unknown> = {},
  ): Promise<{ url: string; blob: Blob; filename: string }> {
    const resp = await this._fetch('/api/v1/3d/generations', {
      method: 'POST',
      body: {
        ...opts,
        model,
        image,
        response_format: 'glb',
      },
      includeSessionHeaders: true,
    });
    const blob = await resp.blob();
    if (blob.size === 0) throw new Error('3D generation endpoint returned an empty model.');
    return { blob, url: URL.createObjectURL(blob), filename: `${model}.glb` };
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
    const body: Record<string, unknown> = { ...opts, model, input };
    if (voice.trim()) body.voice = voice.trim();
    const resp = await this._fetch('/api/v1/audio/speech', {
      method: 'POST',
      body,
      includeSessionHeaders: true,
    });
    const blob = await resp.blob();
    if (blob.size === 0) throw new Error('Speech endpoint returned an empty audio file.');
    return { blob, url: URL.createObjectURL(blob) };
  }

  async imageEdit(model: string, prompt: string, imageDataUrl: string, opts: Record<string, unknown> = {}): Promise<string[]> {
    const requestedSize = typeof opts.size === 'string' && opts.size.trim() ? opts.size.trim() : '';
    const imageBlob = blobFromDataUrl(imageDataUrl);
    const form = new FormData();
    form.append('model', model);
    form.append('prompt', prompt);
    if (requestedSize) form.append('size', requestedSize);
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

  async audioTranscription(model: string, file: File, language?: string): Promise<string> {
    const form = new FormData();
    form.append('file', file);
    form.append('model', model);
    if (language && language.trim()) form.append('language', language.trim());
    const data = await this._json<Record<string, unknown>>('/api/v1/audio/transcriptions', {
      method: 'POST',
      body: form,
      includeSessionHeaders: true,
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

  // ── Runtime config ─────────────────────────────────────────────

  async getRuntimeConfig(): Promise<Record<string, unknown>> {
    return this._json<Record<string, unknown>>('/internal/config');
  }

  async getDefaultContextSize(): Promise<LemonadeContextDefault | undefined> {
    const data = await this.getRuntimeConfig();
    const n = Number(data.ctx_size);

    if (!Number.isFinite(n)) return undefined;
    if (n === -1) return 'auto';
    if (n > 0) return Math.round(n);

    return undefined;
  }

  // ── Log level ───────────────────────────────────────────────────

  async getLogLevel(): Promise<string> {
    const data = await this.getRuntimeConfig();
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

    const tryFallback = () => {
      // Fall back to the dedicated websocket_port from /health (legacy port ~9000).
      // This handles stale-binary scenarios where the main port doesn't yet support
      // WebSocket upgrades. /logs/stream is served on both the main port and this port.
      const fallbackPort = this._healthData?.websocket_port;
      if (!fallbackPort) {
        if (!closed) callbacks.onError?.('Could not connect to log stream.');
        return;
      }
      const fallbackUrl = this._buildWebSocketUrl('/logs/stream', fallbackPort);
      this._openLogSocket(fallbackUrl, callbacks, afterSeq)
        .then(openedSocket => {
          if (closed) { openedSocket.close(1000, 'OK'); return; }
          socket = openedSocket;
        })
        .catch(() => {
          if (!closed) callbacks.onError?.('Could not connect to log stream.');
        });
    };

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
        if (!closed) tryFallback();
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
    const completeBackendInstall = () => {
      callbacks?.onComplete?.();
      this._notifyModelsChanged();
    };

    try {
      const resp = await this._fetch('/api/v1/install', {
        method: 'POST',
        body: { recipe, backend, stream: true, subscribe: false },
        cache: 'no-store',
      });

      const contentType = resp.headers.get('content-type') || '';
      if (!contentType.includes('text/event-stream')) {
        const data = await resp.json().catch(() => ({} as Record<string, unknown>)) as Record<string, unknown>;
        const error = downloadPayloadErrorMessage(data);
        if (error) throw new Error(error);
        callbacks?.onProgress?.(data);
        if (downloadPayloadCompleted(data) || (data as any).action) {
          completeBackendInstall();
          return;
        }
      } else {
        const reader = resp.body?.getReader();
        if (!reader) throw new Error('No response body');
        const dec = new TextDecoder();
        let buf = '';
        let currentEventType = 'progress';
        let sawTerminal = false;

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          buf += dec.decode(value, { stream: true });
          const lines = buf.split('\n');
          buf = lines.pop() || '';
          for (const line of lines) {
            if (line.startsWith('event:')) {
              currentEventType = line.substring(6).trim() || 'progress';
              continue;
            }
            if (!line.startsWith('data:')) continue;
            const d = JSON.parse(line.substring(5).trim()) as Record<string, unknown>;
            const eventError = currentEventType === 'error'
              ? (d.error || d.message || d.detail || 'Unknown backend install error')
              : downloadPayloadErrorMessage(d);
            if (eventError) throw new Error(String(eventError));
            callbacks?.onProgress?.(d);
            if (currentEventType === 'complete' || downloadPayloadCompleted(d)) sawTerminal = true;
          }
        }

        if (sawTerminal) {
          completeBackendInstall();
          return;
        }
      }

      const downloadName = `${recipe}:${backend}`;
      const match = (await this.downloads().catch(() => []))
        .find(download => String(download.id || '') === `backend:${downloadName}` || String(download.model_name || download.name || '') === downloadName);
      const matchError = downloadPayloadErrorMessage(match);
      if (matchError) throw new Error(matchError);
      if (match && downloadPayloadCompleted(match) && match.running !== true) {
        completeBackendInstall();
        return;
      }

      completeBackendInstall();
    } catch (err) {
      callbacks?.onError?.(err as Error);
      this._notifyModelsChanged();
    }
  }

  async uninstallBackend(recipe: string, backend: string): Promise<unknown> {
    const result = await this._json('/api/v1/uninstall', {
      method: 'POST',
      body: { recipe, backend },
    });
    this._notifyModelsChanged();
    return result;
  }

  // ── Persistent downloads ───────────────────────────────────────

  async downloads(): Promise<DownloadProgressEvent[]> {
    const data = await this._json<unknown>('/api/v1/downloads', { cache: 'no-store' } as LemonadeRequestInit);
    if (Array.isArray(data)) return data as DownloadProgressEvent[];
    if (isObject(data) && Array.isArray(data.downloads)) return data.downloads as DownloadProgressEvent[];
    return [];
  }


  async createJob(name: string, steps: unknown[], inputs: Record<string, unknown> = {}): Promise<{ id: string }> {
    return this._json<{ id: string }>('/api/v1/jobs', {
      method: 'POST',
      body: { name, definition: { steps }, inputs },
    });
  }

  async getJob(id: string, signal?: AbortSignal): Promise<JobRecord> {
    return this._json<JobRecord>(`/api/v1/jobs/${encodeURIComponent(id)}`, {
      cache: 'no-store',
      signal,
    } as LemonadeRequestInit);
  }

  async listJobs(): Promise<JobRecord[]> {
    const data = await this._json<unknown>('/api/v1/jobs', { cache: 'no-store' } as LemonadeRequestInit);
    if (Array.isArray(data)) return data as JobRecord[];
    if (isObject(data) && Array.isArray(data.jobs)) return data.jobs as JobRecord[];
    return [];
  }

  async interruptJob(id: string): Promise<void> {
    await this._fetch(`/api/v1/jobs/${encodeURIComponent(id)}/interrupt`, { method: 'POST' });
  }

  async pauseJob(id: string): Promise<void> {
    await this._fetch(`/api/v1/jobs/${encodeURIComponent(id)}/pause`, { method: 'POST' });
  }

  async resumeJob(id: string): Promise<void> {
    await this._fetch(`/api/v1/jobs/${encodeURIComponent(id)}/resume`, { method: 'POST' });
  }

  async deleteJob(id: string): Promise<void> {
    await this._fetch(`/api/v1/jobs/${encodeURIComponent(id)}`, { method: 'DELETE' });
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


  private async _isModelDownloadedOnServer(modelName: string): Promise<boolean> {
    try {
      const target = modelName.trim().toLowerCase();
      const data = await this.models(true);
      return data.data.some((model: ModelInfo) => {
        const id = String((model as any).id || '').trim().toLowerCase();
        const name = String((model as any).name || '').trim().toLowerCase();
        const modelNameValue = String((model as any).model_name || '').trim().toLowerCase();
        const downloaded = (model as any).downloaded === true || (model as any).installed === true || (model as any).local === true;
        return downloaded && (id === target || name === target || modelNameValue === target);
      });
    } catch {
      return false;
    }
  }

  private async _waitForModelDownloadTerminal(
    modelName: string,
    signal: AbortSignal | undefined,
    onProgress: PullCallbacks['onProgress'] | undefined,
    initialJobSeen = false,
  ): Promise<DownloadProgressEvent | null> {
    const started = performance.now();
    let sawServerJob = initialJobSeen;
    let consecutiveMissingSnapshots = 0;
    let snapshotErrorsStartedAt: number | undefined;
    const snapshotErrorTimeoutMs = 300_000;

    while (!signal?.aborted) {
      let downloads: DownloadProgressEvent[] = [];
      try {
        downloads = await this.downloads();
        snapshotErrorsStartedAt = undefined;
      } catch (error) {
        const timestamp = Date.now();
        snapshotErrorsStartedAt ??= timestamp;
        if (timestamp - snapshotErrorsStartedAt >= snapshotErrorTimeoutMs) {
          throw new Error(`Timed out refreshing server download state: ${error instanceof Error ? error.message : 'Unknown error'}`);
        }
        await new Promise(resolve => setTimeout(resolve, 500));
        continue;
      }

      const match = downloads.find(download => this._isMatchingModelDownload(download, modelName));
      if (match) {
        sawServerJob = true;
        consecutiveMissingSnapshots = 0;
        const matchError = downloadPayloadErrorMessage(match);
        onProgress?.(match);
        if (matchError) throw new Error(matchError);

        const stopped = match.running !== true;
        const status = String(match.status || '').toLowerCase();
        if ((status === 'paused' || status === 'cancelled' || status === 'canceled') && stopped) return null;
        if (downloadPayloadCompleted(match) && stopped) return match;
      } else if (sawServerJob) {
        consecutiveMissingSnapshots += 1;
        if (consecutiveMissingSnapshots >= 10) {
          if (await this._isModelDownloadedOnServer(modelName)) {
            return {
              id: `model:${modelName}`,
              type: 'model',
              model_name: modelName,
              status: 'completed',
              running: false,
              complete: true,
              percent: 100,
            };
          }
          throw new Error(`Download for ${modelName} disappeared before completion.`);
        }
      } else if (performance.now() - started > 30000) {
        throw new Error(`Download did not appear in the server download list for ${modelName}.`);
      }

      await new Promise(resolve => setTimeout(resolve, 500));
    }

    return null;
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
        // Let lemond own the download. The UI observes /downloads, just like
        // Lemonade main, instead of deciding completion from a renderer stream.
        subscribe: false,
      };

      const resp = await this._fetch('/api/v1/pull', {
        method: 'POST',
        body,
        signal,
        cache: 'no-store',
      });

      const contentType = resp.headers.get('content-type') || '';
      if (contentType.includes('text/event-stream')) {
        const reader = resp.body?.getReader();
        if (!reader) throw new Error('No response body');

        const dec = new TextDecoder();
        let buf = '';
        let currentEventType = 'progress';
        let terminalPayload: DownloadProgressEvent | null = null;

        try {
          while (true) {
            if (signal?.aborted) {
              await reader.cancel();
              return;
            }
            const { done, value } = await reader.read();
            if (done) break;
            buf += dec.decode(value, { stream: true });
            const lines = buf.split('\n');
            buf = lines.pop() || '';

            for (const line of lines) {
              if (line.startsWith('event:')) {
                currentEventType = line.substring(6).trim() || 'progress';
                continue;
              }
              if (!line.startsWith('data:')) continue;

              const data = JSON.parse(line.substring(5).trim()) as DownloadProgressEvent;
              const eventError = currentEventType === 'error'
                ? (data.error || data.message || data.detail || 'Unknown download error')
                : downloadPayloadErrorMessage(data);
              if (eventError) throw new Error(String(eventError));

              onProgress?.(data);
              if (currentEventType === 'complete' || downloadPayloadCompleted(data)) {
                terminalPayload = data;
              }
            }
          }
        } finally {
          if (signal?.aborted) reader.cancel().catch(() => undefined);
        }

        // Prefer the server registry as the source of truth, so a 404/failed job
        // cannot become completed merely because an SSE response ended.
        const terminal = await this._waitForModelDownloadTerminal(
          modelName,
          signal,
          onProgress,
          terminalPayload != null,
        ).catch(async error => {
          if (terminalPayload && downloadPayloadCompleted(terminalPayload)) return terminalPayload;
          throw error;
        });
        if (terminal) {
          onComplete?.(terminal);
          this._notifyModelsChanged();
        }
        return;
      }

      const startedSnapshot = await resp.json().catch(() => ({} as DownloadProgressEvent)) as DownloadProgressEvent;
      const initialError = downloadPayloadErrorMessage(startedSnapshot);
      if (initialError) throw new Error(initialError);
      onProgress?.(startedSnapshot);

      const terminal = downloadPayloadCompleted(startedSnapshot) && startedSnapshot.running !== true
        ? startedSnapshot
        : await this._waitForModelDownloadTerminal(modelName, signal, onProgress, true);

      if (terminal) {
        const terminalError = downloadPayloadErrorMessage(terminal);
        if (terminalError) throw new Error(terminalError);
        onComplete?.(terminal);
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
    let reasoningStartTime: number | null = null;
    let reasoningEndTime: number | null = null;
    let reasoningClosed = false;

    const reasoningElapsedMs = (now: number): number | null => {
      if (reasoningStartTime == null) return null;
      return Math.max(0, (reasoningEndTime ?? now) - reasoningStartTime);
    };

    const buildDoneStats = (now: number, full: string, reasoning: string, respId: string | null): ChatCompletionStats => {
      if (reasoningStartTime != null && !reasoningClosed) {
        reasoningEndTime = now;
        reasoningClosed = true;
      }
      const decodeTime = firstTokenTime ? (now - firstTokenTime) / 1000 : 0;
      const totalTokens = tokenCount + reasoningTokenCount;
      return {
        content: full,
        reasoning,
        id: respId,
        tps: totalTokens > 0 && decodeTime > 0 ? (totalTokens / decodeTime).toFixed(1) : '0',
        ttft: firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null,
        tokens: tokenCount,
        reasoningTokens: reasoningTokenCount,
        reasoningElapsedMs: reasoningElapsedMs(now),
      };
    };

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
        reasoningElapsedMs: reasoningElapsedMs(now),
        elapsed,
        ttft: firstTokenTime ? firstTokenTime - t0 : null,
      });
    };

    // Timer-based stats: update every 200ms so display stays live even between tokens
    const statsInterval = onStats ? setInterval(emitStats, 200) : undefined;

    try {
      const requestModelInfo = this.allModels.find(candidate => modelInfoKey(candidate).toLowerCase() === model.trim().toLowerCase()) || null;
      const body: Record<string, unknown> = { model, messages, stream: true, ...samplingForModel(model, requestModelInfo), ...(params || {}) };
      if (tools && tools.length > 0) body.tools = tools;
      const resp = await this._fetch('/api/v1/chat/completions', {
        method: 'POST',
        body,
        signal,
        includeSessionHeaders: true,
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
            onDone?.(buildDoneStats(now, full, reasoning, respId));
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
              const now = performance.now();
              if (!firstTokenTime) firstTokenTime = now;
              if (reasoningStartTime == null) reasoningStartTime = now;
              reasoningEndTime = now;
              reasoningClosed = false;
              reasoningTokenCount++;
              reasoning += delta.reasoning_content;
              onReasoning?.(delta.reasoning_content, reasoning);
            }
            if (delta?.content) {
              const now = performance.now();
              if (!firstTokenTime) firstTokenTime = now;
              if (reasoningStartTime != null && !reasoningClosed) {
                reasoningEndTime = now;
                reasoningClosed = true;
              }
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
        onDone?.(buildDoneStats(now, full, reasoning, respId));
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
      body: { model, messages, stream: false, ...samplingForModel(model, this.allModels.find(candidate => modelInfoKey(candidate).toLowerCase() === model.trim().toLowerCase()) || null), ...params },
      includeSessionHeaders: true,
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
      const health = await this.health();
      let models: ModelsData;
      try {
        models = await this.models(true);
      } catch (err) {
        // Health is the connection source of truth. Some tests and lightweight
        // servers expose health/MCP before the model registry is available; do
        // not flip the whole app back to disconnected merely because /models
        // failed after /health succeeded. Keep the previous registry if present,
        // otherwise use an empty one until the next refresh.
        this._lastConnectionError = friendlyErrorMessage(err);
        models = this._modelsData || { data: [] };
      }
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
if (typeof window !== 'undefined') {
  (window as any).apiClient = api;
}
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
