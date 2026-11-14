import { api, ConnectionSnapshot, ObsoleteConnectionError } from '../../api';
import { BackendSchema, SchemaError, parseBackendSchema } from './backendSchema';
import {
  CatalogState,
  catalogState,
  clearServerRecipeNames,
  setCatalogState,
  setSystemRecipeNames,
} from './catalogState';

export type ReadyCatalogState = Extract<CatalogState, { status: 'ready' }>;
type SettledCatalogState = Exclude<CatalogState, { status: 'idle' | 'loading' }>;

export class BackendCatalogClient {
  private cached?: { connection: ConnectionSnapshot; catalog: BackendSchema; etag: string | null };
  private pending?: { generation: number; promise: Promise<SettledCatalogState> };
  private controller?: AbortController;

  clear(): void {
    this.controller?.abort();
    this.cached = undefined;
    this.pending = undefined;
  }

  refresh(): Promise<SettledCatalogState> {
    const connection = api.connectionSnapshot();
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

  private async fetch(connection: ConnectionSnapshot, signal: AbortSignal): Promise<SettledCatalogState> {
    try {
      const headers: Record<string, string> = {};
      if (this.cached?.etag) headers['If-None-Match'] = this.cached.etag;
      let response = await api.fetchForConnection(connection, '/backends/schema', { headers, signal, cache: 'no-store' });
      if (response.status === 304 && !this.cached) {
        response = await api.fetchForConnection(connection, '/backends/schema', { signal, cache: 'no-store' });
      }
      if (response.status === 304 && this.cached) return { status: 'ready', connection, catalog: this.cached.catalog };
      if (!response.ok) {
        this.cached = undefined;
        const status = response.status === 401 || response.status === 403 ? 'authentication' : response.status === 404 ? 'missing' : 'unreachable';
        const message = status === 'authentication' ? 'Authentication failed for the selected server.' :
          status === 'missing' ? 'This server has no backend schema endpoint.' : 'Backend schema request failed.';
        return { status, connection, message };
      }
      const catalog = parseBackendSchema(await response.json());
      api.assertCurrentConnection(connection);
      this.cached = { connection, catalog, etag: response.headers.get('ETag') };
      return { status: 'ready', connection, catalog };
    } catch (error) {
      api.assertCurrentConnection(connection);
      if (error instanceof ObsoleteConnectionError) throw error;
      this.cached = undefined;
      return { status: error instanceof SchemaError || error instanceof SyntaxError ? 'incompatible' : 'unreachable', connection,
        message: error instanceof SchemaError ? error.message : error instanceof SyntaxError ? 'Malformed backend schema response.' : 'Cannot reach the selected server for its backend schema.' };
    }
  }
}

export const backendCatalogClient = new BackendCatalogClient();

export async function refreshBackendCatalog(): Promise<SettledCatalogState> {
  const result = await backendCatalogClient.refresh();
  api.assertCurrentConnection(result.connection);
  const current = catalogState();
  const unchanged = current.status === 'ready' && result.status === 'ready'
    && current.connection.generation === result.connection.generation
    && current.catalog.catalog_id === result.catalog.catalog_id;
  if (!unchanged) setCatalogState(result);
  return result;
}

let started = false;

export function startBackendCatalog(): void {
  if (started) return;
  started = true;
  const refresh = () => { void refreshBackendCatalog().catch(() => undefined); };
  api.onConnectionChange(connection => {
    backendCatalogClient.clear();
    clearServerRecipeNames();
    setCatalogState({ status: 'loading', connection });
    if (api.isConnected) refresh();
  });
  api.onStatusChange(status => { if (status === 'connected') refresh(); });
  api.onSystemInfoChange(info => {
    setSystemRecipeNames(info);
    refresh();
  });
  if (api.systemInfoData) setSystemRecipeNames(api.systemInfoData);
  if (api.isConnected) refresh();
}
