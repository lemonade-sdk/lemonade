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

const trimTrailingSlashes = (value: string): string => value.replace(/\/+$/, '');
const ensureLeadingSlash = (value: string): string => value.startsWith('/') ? value : `/${value}`;
const normalizeWebAppBasePath = (pathname: string): string => {
  let normalizedPath = trimTrailingSlashes(pathname || '/');

  if (!normalizedPath || normalizedPath === '/') {
    return '';
  }

  if (normalizedPath.endsWith('/index.html')) {
    normalizedPath = trimTrailingSlashes(
      normalizedPath.slice(0, -'/index.html'.length),
    );
  }

  if (normalizedPath === '/web-app') {
    return '';
  }

  if (normalizedPath.endsWith('/web-app')) {
    normalizedPath = trimTrailingSlashes(
      normalizedPath.slice(0, -'/web-app'.length),
    );
  }

  return normalizedPath === '/' ? '' : normalizedPath;
};

const deriveWebAppBaseUrl = (): string | null => {
  if (typeof window === 'undefined') {
    return null;
  }

  const origin = window.location?.origin;
  if (!origin || origin === 'null') {
    return null;
  }

  const basePath = normalizeWebAppBasePath(window.location?.pathname ?? '/');
  return `${trimTrailingSlashes(origin)}${basePath}`;
};

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
  private hasExternalUrl: boolean = false;

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
      if (typeof window !== 'undefined' && window.api?.getServerAPIKey) {
        this.apiKey = await window.api.getServerAPIKey();
      }

      let configured = false;

      if (typeof window !== 'undefined' && window.api?.isWebApp) {
        configured = await this.initializeWebAppBaseUrl();
      }

      // In Tauri mode, use an explicit base URL when configured via --base-url
      // or the environment. The separate websocket_port still applies there.
      if (!configured && typeof window !== 'undefined' && window.api?.getServerBaseUrl) {
        const baseUrl = await window.api.getServerBaseUrl();
        if (baseUrl) {
          const normalizedBaseUrl = trimTrailingSlashes(baseUrl);
          console.log('Using explicit server base URL:', normalizedBaseUrl);
          this.explicitBaseUrl = normalizedBaseUrl;
          this.hasExternalUrl = false;
          configured = true;
        }
      }

      // No explicit URL - use localhost with port discovery
      if (!configured && typeof window !== 'undefined' && window.api?.getServerPort) {
        const port = await window.api.getServerPort();
        if (port) {
          this.port = port;
        }

        console.log('Using localhost mode with port:', this.port);
      }

      this.initialized = true;
    } catch (error) {
      console.error('Failed to initialize server config:', error);
      this.initialized = true;
    }

    this.registerEventListeners();
  }

  private async initializeWebAppBaseUrl(): Promise<boolean> {
    const browserBaseUrl = deriveWebAppBaseUrl();
    if (!browserBaseUrl) {
      return false;
    }

    console.log('Using web app request base URL:', browserBaseUrl);
    this.explicitBaseUrl = browserBaseUrl;
    this.hasExternalUrl = false;

    await this.loadExternalUrlFromSystemInfo(browserBaseUrl);
    return true;
  }

  private async loadExternalUrlFromSystemInfo(browserBaseUrl: string): Promise<void> {
    try {
      const headers = this.apiKey
        ? { Authorization: `Bearer ${this.apiKey}` }
        : undefined;
      const response = await fetch(
        `${browserBaseUrl}/api/v1/system-info`,
        headers ? { headers } : undefined,
      );

      if (!response.ok) {
        console.warn(
          'Failed to fetch /system-info for external_url:',
          response.status,
        );
        return;
      }

      const systemInfo = await response.json();
      const externalUrl = typeof systemInfo.external_url === 'string'
        ? trimTrailingSlashes(systemInfo.external_url)
        : '';

      if (!externalUrl) {
        return;
      }

      console.log('Using external_url from /system-info:', externalUrl);
      this.explicitBaseUrl = externalUrl;
      this.hasExternalUrl = true;
    } catch (error) {
      console.warn('Failed to fetch /system-info for external_url:', error);
    }
  }

  private registerEventListeners() {
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
   * Whether the active web-app base URL came from the server's external_url
   * config surfaced through /system-info. When true, browser WebSocket
   * clients should derive public URLs from that base URL instead of using
   * websocket_port directly.
   */
  isExternalUrl(): boolean {
    return this.hasExternalUrl;
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
    const normalizedBaseUrl = trimTrailingSlashes(baseURL);
    if (this.explicitBaseUrl != normalizedBaseUrl) {
      console.log(`Base URL updated: ${this.explicitBaseUrl} -> ${normalizedBaseUrl}`);
      this.explicitBaseUrl = normalizedBaseUrl;
      this.hasExternalUrl = false;
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

    if (this.apiKey != null && this.apiKey != '') {
      options.headers = {
        ...options.headers,
        Authorization: `Bearer ${this.apiKey}`,
      };
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
export const isExternalUrl = () => serverConfig.isExternalUrl();
export const getWebSocketUrl = (endpointPath: string, wsPort: number, query?: URLSearchParams) => {
  const normalizedPath = ensureLeadingSlash(endpointPath);
  const queryString = query?.toString();

  if (serverConfig.isExternalUrl()) {
    const baseUrl = new URL(serverConfig.getServerBaseUrl());
    baseUrl.protocol = baseUrl.protocol === 'https:' ? 'wss:' : 'ws:';
    const basePath = trimTrailingSlashes(baseUrl.pathname);
    baseUrl.pathname = `${basePath}${normalizedPath}` || normalizedPath;
    baseUrl.search = queryString ? `?${queryString}` : '';
    baseUrl.hash = '';
    return baseUrl.toString();
  }

  const serverBaseUrl = new URL(serverConfig.getServerBaseUrl());
  const wsProtocol = serverBaseUrl.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = new URL(`${wsProtocol}//${serverConfig.getServerHost()}:${wsPort}`);
  wsUrl.pathname = normalizedPath;
  wsUrl.search = queryString ? `?${queryString}` : '';
  return wsUrl.toString();
};
export const isRemoteServer = () => serverConfig.isRemoteServer();
export const onServerPortChange = (listener: PortChangeListener) => serverConfig.onPortChange(listener);
export const onServerUrlChange = (listener: UrlChangeListener) => serverConfig.onUrlChange(listener);
export const serverFetch = (endpoint: string, options?: RequestInit) => serverConfig.fetch(endpoint, options);
