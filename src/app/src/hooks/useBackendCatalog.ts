import { useEffect, useSyncExternalStore } from 'react';
import { startBackendCatalog } from '../features/backends/backendCatalog';
import { CatalogState, catalogState, catalogVersion, subscribeCatalog } from '../features/backends/catalogState';

export function useBackendCatalog(): CatalogState {
  useEffect(() => { startBackendCatalog(); }, []);
  useSyncExternalStore(subscribeCatalog, catalogVersion, catalogVersion);
  return catalogState();
}
