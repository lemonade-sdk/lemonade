/**
 * Centralized server configuration management
 * This module provides a single source of truth for the server API base URL
 * and handles automatic port discovery when connections fail.
 *
 * When an explicit URL is configured, port discovery is disabled.
 * Falls back to localhost + port discovery when no explicit URL is provided.
 */

type PortChangeListener = (port: number) => void;
type UrlChangeListener = (url: string, apiKey: string) => void;

class ServerConfig {
  private port: number = 8000;
  private explicitBaseUrl: string | null = null;
  private apiKey: string | null = null;
  private portListeners: Set<PortChangeListener> = new Set();
  private urlListeners: Set<UrlChangeListener> = new Set();
  private isDiscovering: boolean = false;
  private discoveryPromise: Promise<number | null> | null = null;
  private initialized: boolean = false;
  private initPromise: Promise<void> | null = null;

  constructor() {
    // Initialize from Electron API on startup
    this.initPromise = this.initialize();

    // Listen for port updates from main process (only relevant for localhost mode)
    if (typeof window !== 'undefined' && window.api?.onServerPortUpdated && window.api?.onConnectionSettingsUpdated) {
      window.api.onServerPortUpdated((port: number) => {
        // Only update port if we're not using an explicit URL
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

  private async initialize(): Promise<void> {
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
    if (this.explicitBaseUrl) {
      return `${this.explicitBaseUrl}/api/v1`;
    }
    return `http://localhost:${this.port}/api/v1`;
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
export const getAPIKey = () => serverConfig.getAPIKey();
export const getServerPort = () => serverConfig.getPort();
export const discoverServerPort = () => serverConfig.discoverPort();
export const isRemoteServer = () => serverConfig.isRemoteServer();
export const onServerPortChange = (listener: PortChangeListener) => serverConfig.onPortChange(listener);
export const onServerUrlChange = (listener: UrlChangeListener) => serverConfig.onUrlChange(listener);
export const serverFetch = (endpoint: string, options?: RequestInit) => serverConfig.fetch(endpoint, options);
