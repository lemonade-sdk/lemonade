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

class ServerConfig {
  private port: number = 13305;
  private explicitBaseUrl: string | null = null;
  private apiKey: string | null = null;
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
        }

        this.initialized = true;
        return;
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
    return this.explicitBaseUrl !== null;
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
   * Get the server hostname
   */
  getServerHost(): string {
    const url = new URL(this.getServerBaseUrl());
    return url.hostname;
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

  private setUpdatedURL(baseURL: string) {
    if (this.explicitBaseUrl != baseURL) {
      console.log(`Base URL updated: ${this.explicitBaseUrl} -> ${baseURL}`);
      this.explicitBaseUrl = baseURL;
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
   * Discover the server port by calling lemonade-server --status
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
export const getServerHost = () => serverConfig.getServerHost();
export const getAPIKey = () => serverConfig.getAPIKey();
export const getServerPort = () => serverConfig.getPort();
export const discoverServerPort = () => serverConfig.discoverPort();
export const getWebSocketProtocol = () => new URL(serverConfig.getServerBaseUrl()).protocol === 'https:' ? 'wss' : 'ws';
export const isRemoteServer = () => serverConfig.isRemoteServer();
export const onServerPortChange = (listener: PortChangeListener) => serverConfig.onPortChange(listener);
export const onServerUrlChange = (listener: UrlChangeListener) => serverConfig.onUrlChange(listener);
export const serverFetch = (endpoint: string, options?: RequestInit) => serverConfig.fetch(endpoint, options);

// =====================================================================
// Chat-target layer
// =====================================================================
//
// Spotify-style "Run on" picker: per-feature override that re-routes JUST the
// LLM chat panel's HTTP requests to a peer Lemonade server discovered via the
// UDP beacon (see src/app/src-tauri/src/beacon.rs). Everything else (Model
// Manager, downloads, image gen, TTS, etc.) keeps using the global
// `serverConfig` singleton — that's the v1 scope agreed in the plan.
//
// Per AGENTS.md invariant 11 ("per-client state lives locally"), the selected
// target is persisted in localStorage rather than pushed to lemond.

export interface ChatTarget {
  baseUrl: string;
  apiKey: string;
  isLocal: boolean;
}

type ChatTargetListener = (target: ChatTarget) => void;

const CHAT_TARGET_STORAGE_KEY = 'lemonade.chatTarget';

// Constant token sent on requests to remote peers. The plan explicitly defers
// per-device API keys ("dummy token, all local"); a remote that has
// LEMONADE_API_KEY set will reject this and the chat panel will surface the
// 401 in the assistant bubble.
const REMOTE_LAN_TOKEN = 'lemonade-lan';

class ChatTargetConfig {
  // Stored base URL with `/api/v1/` already appended (matches the beacon
  // payload format from network_beacon.cpp). null = use the local singleton.
  private remoteBaseUrl: string | null = null;
  private listeners: Set<ChatTargetListener> = new Set();

  constructor() {
    this.loadFromStorage();
  }

  private loadFromStorage(): void {
    try {
      if (typeof localStorage === 'undefined') return;
      const stored = localStorage.getItem(CHAT_TARGET_STORAGE_KEY);
      if (stored && stored.length > 0) {
        this.remoteBaseUrl = stored;
      }
    } catch (err) {
      console.warn('Failed to read chat target from localStorage:', err);
    }
  }

  private saveToStorage(): void {
    try {
      if (typeof localStorage === 'undefined') return;
      if (this.remoteBaseUrl) {
        localStorage.setItem(CHAT_TARGET_STORAGE_KEY, this.remoteBaseUrl);
      } else {
        localStorage.removeItem(CHAT_TARGET_STORAGE_KEY);
      }
    } catch (err) {
      console.warn('Failed to persist chat target to localStorage:', err);
    }
  }

  /**
   * Strip a trailing `/api/v1` (or `/api/v1/`) so we can append `/api/v1`
   * uniformly when building the chat URL. The beacon advertises the
   * `/api/v1/` form; the global singleton stores the bare server origin.
   */
  private toServerOrigin(rawBaseUrl: string): string {
    let trimmed = rawBaseUrl.replace(/\/+$/, '');
    if (trimmed.endsWith('/api/v1')) {
      trimmed = trimmed.slice(0, -'/api/v1'.length);
    } else if (trimmed.endsWith('/api/v0')) {
      trimmed = trimmed.slice(0, -'/api/v0'.length);
    }
    return trimmed;
  }

  /** True if the user has picked a peer device (anything other than local). */
  isRemote(): boolean {
    return this.remoteBaseUrl !== null;
  }

  /** Raw selected base URL (with `/api/v1/`) or null if using local. */
  getRawRemoteBaseUrl(): string | null {
    return this.remoteBaseUrl;
  }

  /**
   * Resolved chat target. When no remote is selected, falls through to the
   * global singleton so chat behaves identically to every other panel.
   */
  getTarget(): ChatTarget {
    if (this.remoteBaseUrl) {
      return {
        baseUrl: this.toServerOrigin(this.remoteBaseUrl),
        apiKey: REMOTE_LAN_TOKEN,
        isLocal: false,
      };
    }
    return {
      baseUrl: serverConfig.getServerBaseUrl(),
      apiKey: serverConfig.getAPIKey(),
      isLocal: true,
    };
  }

  setRemoteBaseUrl(rawBaseUrl: string | null): void {
    const normalized = rawBaseUrl && rawBaseUrl.length > 0 ? rawBaseUrl : null;
    if (normalized === this.remoteBaseUrl) return;
    this.remoteBaseUrl = normalized;
    this.saveToStorage();
    this.notify();
  }

  onChange(listener: ChatTargetListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  private notify(): void {
    const t = this.getTarget();
    this.listeners.forEach((cb) => {
      try {
        cb(t);
      } catch (err) {
        console.error('Error in chat target listener:', err);
      }
    });
  }

  /**
   * Force-fire listeners without changing the selection. Used when the
   * global singleton's URL/port shifts under us while we're targeting local —
   * subscribers (e.g. the picker pill) need to re-read `getTarget().baseUrl`.
   */
  notifyForLocalChange(): void {
    if (!this.remoteBaseUrl) {
      this.notify();
    }
  }

  /**
   * Drop-in replacement for `serverFetch` that routes through the selected
   * chat target. When the target is local, this is intentionally identical to
   * `serverConfig.fetch` (including the auto-rediscover-on-failure path).
   * When the target is remote, we hit the peer directly and don't attempt
   * port discovery — peers are responsible for their own beacons.
   */
  async fetch(endpoint: string, opts?: RequestInit): Promise<Response> {
    if (!this.remoteBaseUrl) {
      return serverConfig.fetch(endpoint, opts);
    }
    await serverConfig.waitForInit();
    const target = this.getTarget();
    const fullUrl = endpoint.startsWith('http')
      ? endpoint
      : `${target.baseUrl}/api/v1${endpoint.startsWith('/') ? endpoint : `/${endpoint}`}`;

    const options: RequestInit = { ...opts };
    const headers: Record<string, string> = {
      ...(options.headers as Record<string, string> | undefined),
    };
    if (target.apiKey) {
      headers['Authorization'] = `Bearer ${target.apiKey}`;
    }
    // Hostname header lets the host owner's authorization prompt show a
    // friendly label ("alice-mac") instead of just the IP. Cached on
    // window.api.localHostname by the Tauri shim; absent in the web app
    // (browsers can't read it), in which case the host falls back to IP.
    const hostname = typeof window !== 'undefined' ? window.api?.localHostname : '';
    if (hostname && hostname.length > 0) {
      headers['X-Lemonade-Client-Hostname'] = hostname;
    }
    options.headers = headers;
    return fetch(fullUrl, options);
  }
}

const chatTargetConfig = new ChatTargetConfig();

// React to the global singleton's URL changes so listeners get re-fired when
// the user is on "local" and the local port shifts. This is the same
// notify-on-port-change behavior chat had before the picker existed.
serverConfig.onUrlChange(() => {
  chatTargetConfig.notifyForLocalChange();
});

export const getChatTarget = (): ChatTarget => chatTargetConfig.getTarget();
export const isRemoteChatTarget = (): boolean => chatTargetConfig.isRemote();
export const getRemoteChatBaseUrl = (): string | null => chatTargetConfig.getRawRemoteBaseUrl();
export const setChatTarget = (rawBaseUrl: string | null): void =>
  chatTargetConfig.setRemoteBaseUrl(rawBaseUrl);
export const onChatTargetChange = (listener: ChatTargetListener): (() => void) =>
  chatTargetConfig.onChange(listener);
export const chatFetch = (endpoint: string, options?: RequestInit): Promise<Response> =>
  chatTargetConfig.fetch(endpoint, options);
