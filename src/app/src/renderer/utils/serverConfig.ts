/**
 * Centralized server configuration management
 * This module provides a single source of truth for the server API base URL
 * and handles automatic port discovery when connections fail.
 *
 * When an explicit URL is configured, port discovery is disabled.
 * Falls back to localhost + port discovery when no explicit URL is provided.
 */

import { tauriReady } from '../tauriShim';

type PortChangeListener = (port: number) => void;
type UrlChangeListener = (url: string, apiKey: string) => void;

// Per-provider cloud credentials, mirrored from app settings. Used by
// fetch() to attach X-Lemonade-Cloud-Key / X-Lemonade-Cloud-Base-Url
// headers when the request body's "model" prefix matches a configured
// provider. lemond's CloudServer reads + strips these headers; non-cloud
// requests are untouched.
type CloudProviderCacheEntry = { baseUrl: string; apiKey: string };

class ServerConfig {
  private port: number = 13305;
  private explicitBaseUrl: string | null = null;
  private apiKey: string | null = null;
  // model_name (e.g., "fireworks.kimi-k2p5") -> upstream id
  // (e.g., "accounts/fireworks/models/kimi-k2p5"). Populated from cloud
  // discovery responses so fetch() can attach X-Lemonade-Cloud-Upstream-Model
  // and the server forwards the right id upstream.
  private cloudModelCheckpoints: Map<string, string> = new Map();
  // model_name -> capability labels (e.g. ["cloud","vision","tool-calling"])
  // learned at discovery. Sent via X-Lemonade-Cloud-Labels on load/chat so the
  // server's lazily-registered entry keeps the model's real capabilities
  // instead of collapsing to just "cloud" (which would erase image/tool
  // support once the model shows up in /models).
  private cloudModelLabels: Map<string, string[]> = new Map();
  private portListeners: Set<PortChangeListener> = new Set();
  private urlListeners: Set<UrlChangeListener> = new Set();
  private isDiscovering: boolean = false;
  private discoveryPromise: Promise<number | null> | null = null;
  private initialized: boolean = false;
  private initPromise: Promise<void> | null = null;

  constructor() {
    // Initialize from the host (Tauri invoke bridge or web-app mock) on startup.
    // Event listeners are registered inside initialize() rather than here
    // because window.api is installed asynchronously by tauriShim.ts and is
    // not yet available during synchronous module-graph evaluation.
    this.initPromise = this.initialize();
  }

  private async initialize(): Promise<void> {
    // Wait for tauriShim.ts to finish installing window.api. Both
    // installTauriApi() and this method are kicked off during module
    // evaluation as fire-and-forget promises; without this await,
    // initialize() races installTauriApi() and loses — every window.api
    // check below sees undefined, and we fall through to localhost:13305
    // with no API key.
    await tauriReady;

    try {
      // Get API Key if available
      if (typeof window !== 'undefined'&& window.api?.getServerAPIKey) {
        this.apiKey = await window.api.getServerAPIKey();
      }

      // In web app mode, use the current origin as the server base URL
      if (typeof window !== 'undefined' && window.api?.isWebApp) {
        const origin = window.location?.origin;
        if (origin && origin !== 'null') {
          const trimmedOrigin = origin.replace(/\/+$/, '');
          console.log('Using web app origin as server base URL:', trimmedOrigin);
          this.explicitBaseUrl = trimmedOrigin;
          this.initialized = true;
          return;
        }
      }

      // Check if an explicit base URL was configured (--base-url or env var)
      if (typeof window !== 'undefined' && window.api?.getServerBaseUrl && window.api?.getServerAPIKey) {
        const baseUrl = await window.api.getServerBaseUrl();
        if (baseUrl) {
          console.log('Using explicit server base URL:', baseUrl);
          this.explicitBaseUrl = baseUrl;
          this.initialized = true;
          return;
        }
      }

      // No explicit URL - use localhost with port discovery
      if (typeof window !== 'undefined' && window.api?.getServerPort) {
        const port = await window.api.getServerPort();
        if (port) {
          this.port = port;
        }
      }

      console.log('Using localhost mode with port:', this.port);
      this.initialized = true;
    } catch (error) {
      console.error('Failed to initialize server config:', error);
      this.initialized = true;
    }

    // Register event listeners AFTER the first await-cycle so window.api is
    // guaranteed to be installed. tauriShim.ts installs window.api via a
    // fire-and-forget async call that completes on the microtask queue; the
    // constructor runs synchronously during module-graph evaluation and would
    // see window.api as undefined if we registered there.
    if (typeof window !== 'undefined' && window.api?.onServerPortUpdated && window.api?.onConnectionSettingsUpdated) {
      window.api.onServerPortUpdated((port: number) => {
        if (!this.explicitBaseUrl) {
          this.setPort(port);
        }
      });

      window.api.onConnectionSettingsUpdated((baseURL: string, apiKey: string) => {
        if (this.explicitBaseUrl != baseURL) {
          this.setUpdatedURL(baseURL);
        }
        if (this.apiKey != apiKey) {
          this.setUpdatedAPIKey(apiKey);
        }
      });
    }
  }

  // Read the configured cloud providers from app settings — the single
  // source of truth (the same store the cloud-provider UI writes and that
  // modelData.ts reads for discovery). We deliberately do NOT keep a
  // long-lived in-memory copy: a cached map drifts out of sync with settings
  // (the web app's onSettingsUpdated is a no-op, and another tab/client can
  // edit settings), and a stale cache silently drops the per-request creds
  // headers, which the server then rejects with an opaque 404. Reading fresh
  // costs one settings read per HTTP request (not per token) — negligible
  // next to inference latency.
  private async readCloudProvidersFromSettings(): Promise<Map<string, CloudProviderCacheEntry>> {
    const out = new Map<string, CloudProviderCacheEntry>();
    try {
      if (typeof window === 'undefined' || !window.api?.getSettings) return out;
      const stored = await window.api.getSettings();
      const providers = (stored as any)?.cloudProviders;
      if (providers && typeof providers === 'object' && !Array.isArray(providers)) {
        for (const [name, cfg] of Object.entries(providers)) {
          if (!cfg || typeof cfg !== 'object') continue;
          const c = cfg as any;
          if (typeof c.baseUrl === 'string' && typeof c.apiKey === 'string' && c.apiKey.length > 0) {
            out.set(name, { baseUrl: c.baseUrl, apiKey: c.apiKey });
          }
        }
      }
    } catch (err) {
      console.warn('Failed to read cloud providers from settings:', err);
    }
    return out;
  }

  // Resolve the cloud provider creds for a model name. Convention: cloud
  // models are named "<provider>.<upstream-id>", so the prefix before the
  // first dot is the provider slug. Provider slugs contain no dots, so the
  // first dot is always the namespace separator. Returns null for
  // local/non-cloud models (no dot, or prefix not in the configured
  // providers — so a local name like "Qwen2.5-..." won't match unless a
  // provider is literally named "Qwen2").
  private async resolveCloudProvider(
    model: unknown,
  ): Promise<{ provider: string; entry: CloudProviderCacheEntry } | null> {
    if (typeof model !== 'string') return null;
    const dot = model.indexOf('.');
    if (dot <= 0) return null;
    const prefix = model.substring(0, dot);
    const providers = await this.readCloudProvidersFromSettings();
    const entry = providers.get(prefix);
    return entry ? { provider: prefix, entry } : null;
  }

  // Called by modelData.ts after each cloud discovery so fetch() can
  // attach the right upstream id per request. Replaces (does not merge)
  // the entries for the named provider — if discovery returned a smaller
  // list this time, stale entries are dropped.
  setCloudModelCheckpoints(provider: string, entries: Array<{ id: string; checkpoint: string }>): void {
    // Drop any previous entries for this provider before reseeding.
    const prefix = `${provider}.`;
    for (const key of Array.from(this.cloudModelCheckpoints.keys())) {
      if (key.startsWith(prefix)) {
        this.cloudModelCheckpoints.delete(key);
      }
    }
    for (const entry of entries) {
      if (entry?.id && entry?.checkpoint) {
        this.cloudModelCheckpoints.set(entry.id, entry.checkpoint);
      }
    }
  }

  // Reseed the model_name -> capability-labels map for a provider (replaces
  // this provider's prior entries). Mirrors setCloudModelCheckpoints; called
  // by modelData.ts after each discovery so load/chat can forward
  // X-Lemonade-Cloud-Labels.
  setCloudModelLabels(provider: string, entries: Array<{ id: string; labels: string[] }>): void {
    const prefix = `${provider}.`;
    for (const key of Array.from(this.cloudModelLabels.keys())) {
      if (key.startsWith(prefix)) {
        this.cloudModelLabels.delete(key);
      }
    }
    for (const entry of entries) {
      if (entry?.id && Array.isArray(entry.labels) && entry.labels.length > 0) {
        this.cloudModelLabels.set(entry.id, entry.labels);
      }
    }
  }

  /**
   * Wait for initialization to complete
   */
  async waitForInit(): Promise<void> {
    if (this.initPromise) {
      await this.initPromise;
    }
  }

  /**
   * Check if using an explicit remote server URL
   */
  isRemoteServer(): boolean {
    return !!this.explicitBaseUrl;
  }

  /**
   * Get the current server port (only meaningful for localhost mode)
   */
  getPort(): number {
    return this.port;
  }

  /**
   * Get the full API base URL
   */
  getApiBaseUrl(): string {
    return `${this.getServerBaseUrl()}/api/v1`;
  }

  /**
   * Get the server base URL (without /api/v1)
   */
  getServerBaseUrl(): string {
    if (this.explicitBaseUrl) {
      return this.explicitBaseUrl;
    }
    return `http://localhost:${this.port}`;
  }

  /**
   * Get the server API key
   */
  getAPIKey(): string {
    if (this.apiKey) {
      return this.apiKey;
    }
    return '';
  }

  /**
   * Build a WebSocket URL for an endpoint served on the websocket port
   * advertised by /health. Going through URL rather than string concat is
   * what makes this correct for IPv6 literals — URL.host preserves the
   * brackets that hostname does not. The configured API key is appended
   * automatically when set.
   */
  buildWebSocketUrl(path: string, wsPort: number, query?: URLSearchParams): string {
    const url = new URL(this.getServerBaseUrl());
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    url.port = String(wsPort);
    url.pathname = url.pathname.replace(/\/$/, '') + path;

    const params = new URLSearchParams(query);
    const apiKey = this.getAPIKey();
    if (apiKey) {
      params.set('api_key', apiKey);
    }
    url.search = params.toString();
    return url.toString();
  }

  /**
   * Set the port and notify all listeners (only for localhost mode)
   */
  private setPort(port: number) {
    if (this.port !== port) {
      console.log(`Server port updated: ${this.port} -> ${port}`);
      this.port = port;
      this.notifyPortListeners();
      this.notifyUrlListeners();
    }
  }

  private setUpdatedURL(baseURL: string | null) {
    const nextBaseUrl = baseURL?.trim() || null;

    if (this.explicitBaseUrl !== nextBaseUrl) {
      console.log(`Base URL updated: ${this.explicitBaseUrl} -> ${nextBaseUrl}`);
      this.explicitBaseUrl = nextBaseUrl;
      this.notifyPortListeners();
      this.notifyUrlListeners();
    }
  }

  private setUpdatedAPIKey(apiKey: string) {
    if (this.apiKey != apiKey) {
      console.log(`API Key updated: ${this.apiKey} -> ${apiKey}`);
      this.apiKey = apiKey;
      this.notifyPortListeners();
      this.notifyUrlListeners();
    }
  }

  /**
   * Discover the server port via a UDP beacon from the local lemond instance.
   * Returns a promise that resolves with the discovered port, or null if discovery is disabled
   */
  async discoverPort(): Promise<number | null> {
    // Skip discovery if using explicit remote URL
    if (this.explicitBaseUrl) {
      console.log('Port discovery skipped - using explicit server URL');
      return null;
    }

    // If already discovering, return the existing promise
    if (this.isDiscovering && this.discoveryPromise) {
      return this.discoveryPromise;
    }

    this.isDiscovering = true;
    this.discoveryPromise = this.performDiscovery();

    try {
      const port = await this.discoveryPromise;
      return port;
    } finally {
      this.isDiscovering = false;
      this.discoveryPromise = null;
    }
  }

  private async performDiscovery(): Promise<number | null> {
    try {
      if (typeof window === 'undefined' || !window.api?.discoverServerPort) {
        console.warn('Port discovery not available');
        return this.port;
      }

      console.log('Discovering server port...');
      const port = await window.api.discoverServerPort();

      // discoverServerPort returns null when explicit URL is configured
      if (port === null) {
        console.log('Port discovery returned null (explicit URL configured)');
        return null;
      }

      this.setPort(port);
      return port;
    } catch (error) {
      console.error('Failed to discover server port:', error);
      return this.port;
    }
  }

  /**
   * Subscribe to port changes (only fires in localhost mode)
   * Returns an unsubscribe function
   */
  onPortChange(listener: PortChangeListener): () => void {
    this.portListeners.add(listener);
    return () => {
      this.portListeners.delete(listener);
    };
  }

  /**
   * Subscribe to URL changes (fires when port changes or explicit URL changes)
   * Returns an unsubscribe function
   */
  onUrlChange(listener: UrlChangeListener): () => void {
    this.urlListeners.add(listener);
    return () => {
      this.urlListeners.delete(listener);
    };
  }

  private notifyPortListeners() {
    this.portListeners.forEach((listener) => {
      try {
        listener(this.port);
      } catch (error) {
        console.error('Error in port change listener:', error);
      }
    });
  }

  private notifyUrlListeners() {
    const url = this.getServerBaseUrl();
    const apiKey = this.getAPIKey();

    this.urlListeners.forEach((listener) => {
      try {
        listener(url, apiKey);
      } catch (error) {
        console.error('Error in URL change listener:', error);
      }
    });
  }

  /**
   * Wrapper for fetch that automatically discovers port on connection failures
   * (only attempts discovery in localhost mode)
   */
  async fetch(endpoint: string, opts?: RequestInit): Promise<Response> {
    await this.waitForInit();

    const fullUrl = endpoint.startsWith('http')
      ? endpoint
      : `${this.getApiBaseUrl()}${endpoint.startsWith('/') ? endpoint : `/${endpoint}`}`;

    const options = { ...opts };

    if(this.apiKey != null && this.apiKey != '') {
      options.headers = {
        ...options.headers,
        Authorization: `Bearer ${this.apiKey}`,
      }
    }

    // If the request body names a cloud-routed model, attach the per-request
    // cloud headers. lemond's chat handlers copy these into the forwarded
    // request body as "_lemonade_cloud_creds" so CloudServer can read and
    // strip them before forwarding upstream. Creds are resolved from app
    // settings (the source of truth) at request time — see resolveCloudProvider
    // for why we don't trust a cached map.
    if (typeof options.body === 'string' && options.body.length > 0 && options.body[0] === '{') {
      let modelField: unknown;
      try {
        const parsed = JSON.parse(options.body);
        // OpenAI-compat endpoints use "model"; lemonade's /load uses "model_name".
        modelField = parsed?.model ?? parsed?.model_name;
      } catch {
        modelField = undefined; // Body wasn't JSON; nothing to inject.
      }

      // Only dotted names can be cloud models ("<provider>.<id>"); skip the
      // settings read entirely for plain local names.
      if (typeof modelField === 'string' && modelField.indexOf('.') > 0) {
        const resolved = await this.resolveCloudProvider(modelField);
        if (resolved) {
          const cloudHeaders: Record<string, string> = {
            'X-Lemonade-Cloud-Key': resolved.entry.apiKey,
            'X-Lemonade-Cloud-Base-Url': resolved.entry.baseUrl,
          };
          // Some providers (Fireworks, OpenRouter) clean their public model
          // ids in a way that doesn't round-trip server-side. Pass the raw
          // upstream id from the discovery response so the server forwards
          // exactly what the provider's API expects.
          const upstream = this.cloudModelCheckpoints.get(modelField);
          if (upstream) {
            cloudHeaders['X-Lemonade-Cloud-Upstream-Model'] = upstream;
          }
          // Forward the discovery-time capability labels so the server's
          // lazily-registered entry keeps vision/tool-calling/etc. (see #2).
          const labels = this.cloudModelLabels.get(modelField);
          if (labels && labels.length > 0) {
            cloudHeaders['X-Lemonade-Cloud-Labels'] = labels.join(',');
          }
          options.headers = { ...options.headers, ...cloudHeaders };
        } else if (this.cloudModelCheckpoints.has(modelField)) {
          // The model was discovered as a cloud model (it's in the checkpoint
          // map) but no provider creds resolve for it now — the provider is
          // not configured (or was removed) on this client. Fail loud instead
          // of firing a credential-less request that the server rejects with
          // an opaque "Model not found" 404.
          const prefix = modelField.slice(0, modelField.indexOf('.'));
          throw new Error(
            `"${modelField}" is a cloud model, but no API key is configured for ` +
            `provider "${prefix}" on this client. Add it under Settings → Cloud Providers.`,
          );
        }
      }
    }

    try {
      const response = await fetch(fullUrl, options);
      return response;
    } catch (error) {
      // If using explicit URL, don't attempt discovery - just throw
      if (this.explicitBaseUrl) {
        throw error;
      }

      // If fetch fails in localhost mode, try discovering the port and retry once
      console.warn('Fetch failed, attempting port discovery...', error);

      try {
        await this.discoverPort();
        const newUrl = endpoint.startsWith('http')
          ? endpoint.replace(/localhost:\d+/, `localhost:${this.port}`)
          : `${this.getApiBaseUrl()}${endpoint.startsWith('/') ? endpoint : `/${endpoint}`}`;

        return await fetch(newUrl, options);
      } catch (retryError) {
        // If retry also fails, throw the original error
        throw error;
      }
    }
  }
}

// Export singleton instance
export const serverConfig = new ServerConfig();

// Export convenience functions
export const getApiBaseUrl = () => serverConfig.getApiBaseUrl();
export const getServerBaseUrl = () => serverConfig.getServerBaseUrl();
export const getAPIKey = () => serverConfig.getAPIKey();
export const getServerPort = () => serverConfig.getPort();
export const discoverServerPort = () => serverConfig.discoverPort();
export const buildWebSocketUrl = (path: string, wsPort: number, query?: URLSearchParams) =>
  serverConfig.buildWebSocketUrl(path, wsPort, query);
export const isRemoteServer = () => serverConfig.isRemoteServer();
export const onServerPortChange = (listener: PortChangeListener) => serverConfig.onPortChange(listener);
export const onServerUrlChange = (listener: UrlChangeListener) => serverConfig.onUrlChange(listener);
export const serverFetch = (endpoint: string, options?: RequestInit) => serverConfig.fetch(endpoint, options);
