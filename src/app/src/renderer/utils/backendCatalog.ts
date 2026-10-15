import { BackendSchema, SchemaError, parseBackendSchema } from '../recipes/backendSchema';
import { ConnectionSnapshot, ObsoleteConnectionError, serverConfig } from './serverConfig';

export type CatalogState =
  | { status: 'idle' | 'loading'; connection: ConnectionSnapshot }
  | { status: 'ready'; connection: ConnectionSnapshot; catalog: BackendSchema }
  | { status: 'missing' | 'authentication' | 'unreachable' | 'incompatible'; connection: ConnectionSnapshot; message: string };
export class BackendCatalogClient {
  private cached?: { connection: ConnectionSnapshot; catalog: BackendSchema; etag: string | null };
  private pending?: { generation: number; promise: Promise<CatalogState> };
  private controller?: AbortController;

  clear(): void {
    this.controller?.abort();
    this.cached = undefined;
    this.pending = undefined;
  }

  refresh(): Promise<CatalogState> {
    const connection = serverConfig.getSnapshot();
    if (this.pending?.generation === connection.generation) return this.pending.promise;
    if (this.cached && (this.cached.connection.apiUrl !== connection.apiUrl ||
        this.cached.connection.authenticationGeneration !== connection.authenticationGeneration ||
        this.cached.connection.generation !== connection.generation)) this.clear();
    const controller = new AbortController();
    this.controller = controller;
    const promise = this.fetch(connection, controller.signal).finally(() => {
      if (this.pending?.promise === promise) this.pending = undefined;
    });
    this.pending = { generation: connection.generation, promise };
    return promise;
  }

  private async fetch(connection: ConnectionSnapshot, signal: AbortSignal): Promise<CatalogState> {
    try {
      const headers: Record<string, string> = {};
      if (this.cached?.etag) headers['If-None-Match'] = this.cached.etag;
      let response = await serverConfig.fetchForConnection(connection, '/backends/schema', { headers, signal });
      if (response.status === 304 && !this.cached) response = await serverConfig.fetchForConnection(connection, '/backends/schema', { signal });
      if (response.status === 304 && this.cached) return { status: 'ready', connection, catalog: this.cached.catalog };
      if (!response.ok) {
        this.cached = undefined;
        const status = response.status === 401 || response.status === 403 ? 'authentication' : response.status === 404 ? 'missing' : 'unreachable';
        const message = status === 'authentication' ? 'Authentication failed for the selected server.' :
          status === 'missing' ? 'This server has no backend schema endpoint.' : 'Backend schema request failed.';
        return { status, connection, message };
      }
      const catalog = parseBackendSchema(await response.json());
      serverConfig.assertCurrent(connection);
      this.cached = { connection, catalog, etag: response.headers.get('ETag') };
      return { status: 'ready', connection, catalog };
    } catch (error) {
      serverConfig.assertCurrent(connection);
      if (error instanceof ObsoleteConnectionError) throw error;
      this.cached = undefined;
      return { status: error instanceof SchemaError ? 'incompatible' : 'unreachable', connection,
        message: error instanceof SchemaError ? error.message : 'Cannot reach the selected server for its backend schema.' };
    }
  }
}
export const backendCatalogClient = new BackendCatalogClient();
serverConfig.onConnectionChange(() => backendCatalogClient.clear());
