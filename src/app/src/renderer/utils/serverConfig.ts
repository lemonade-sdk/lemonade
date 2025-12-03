/**
 * Centralized server configuration management
 * This module provides a single source of truth for the server API base URL
 * and handles automatic port discovery when connections fail.
 */

type PortChangeListener = (port: number) => void;

class ServerConfig {
  private port: number = 8000;
  private listeners: Set<PortChangeListener> = new Set();
  private isDiscovering: boolean = false;
  private discoveryPromise: Promise<number> | null = null;

  constructor() {
    // Initialize port from Electron API on startup
    this.initializePort();

    // Listen for port updates from main process
    if (typeof window !== 'undefined' && window.api?.onServerPortUpdated) {
      window.api.onServerPortUpdated((port: number) => {
        this.setPort(port);
      });
    }
  }

  private async initializePort() {
    try {
      if (typeof window !== 'undefined' && window.api?.getServerPort) {
        const port = await window.api.getServerPort();
        this.port = port;
      }
    } catch (error) {
      console.error('Failed to initialize server port:', error);
    }
  }

  /**
   * Get the current server port
   */
  getPort(): number {
    return this.port;
  }

  /**
   * Get the full API base URL
   */
  getApiBaseUrl(): string {
    return `http://localhost:${this.port}/api/v1`;
  }

  /**
   * Get the server base URL (without /api/v1)
   */
  getServerBaseUrl(): string {
    return `http://localhost:${this.port}`;
  }

  /**
   * Set the port and notify all listeners
   */
  private setPort(port: number) {
    if (this.port !== port) {
      console.log(`Server port updated: ${this.port} -> ${port}`);
      this.port = port;
      this.notifyListeners();
    }
  }

  /**
   * Discover the server port by calling lemonade-server --status
   * Returns a promise that resolves with the discovered port
   */
  async discoverPort(): Promise<number> {
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

  private async performDiscovery(): Promise<number> {
    try {
      if (typeof window === 'undefined' || !window.api?.discoverServerPort) {
        console.warn('Port discovery not available');
        return this.port;
      }

      console.log('Discovering server port...');
      const port = await window.api.discoverServerPort();
      this.setPort(port);
      return port;
    } catch (error) {
      console.error('Failed to discover server port:', error);
      return this.port;
    }
  }

  /**
   * Subscribe to port changes
   * Returns an unsubscribe function
   */
  onPortChange(listener: PortChangeListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  private notifyListeners() {
    this.listeners.forEach((listener) => {
      try {
        listener(this.port);
      } catch (error) {
        console.error('Error in port change listener:', error);
      }
    });
  }

  /**
   * Wrapper for fetch that automatically discovers port on connection failures
   */
  async fetch(endpoint: string, options?: RequestInit): Promise<Response> {
    const fullUrl = endpoint.startsWith('http') 
      ? endpoint 
      : `${this.getApiBaseUrl()}${endpoint.startsWith('/') ? endpoint : `/${endpoint}`}`;

    try {
      const response = await fetch(fullUrl, options);
      return response;
    } catch (error) {
      // If fetch fails, try discovering the port and retry once
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
export const getServerPort = () => serverConfig.getPort();
export const discoverServerPort = () => serverConfig.discoverPort();
export const onServerPortChange = (listener: PortChangeListener) => serverConfig.onPortChange(listener);
export const serverFetch = (endpoint: string, options?: RequestInit) => serverConfig.fetch(endpoint, options);
