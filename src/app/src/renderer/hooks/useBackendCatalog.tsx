import React, { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { BackendRecipe } from '../recipes/backendSchema';
import { CatalogState, backendCatalogClient } from '../utils/backendCatalog';
import { replaceRecipeDisplayNames } from '../utils/recipeNames';
import { serverConfig } from '../utils/serverConfig';

interface CatalogContextValue {
  state: CatalogState;
  refresh: () => Promise<CatalogState>;
  recipe: (name: string) => BackendRecipe | undefined;
}
const CatalogContext = createContext<CatalogContextValue | null>(null);
export const BackendCatalogProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [state, setState] = useState<CatalogState>({ status: 'idle', connection: serverConfig.getSnapshot() });
  const identity = useRef('');
  const refresh = useCallback(async () => {
    await serverConfig.waitForInit();
    const result = await backendCatalogClient.refresh();
    serverConfig.assertCurrent(result.connection);
    setState(result);
    replaceRecipeDisplayNames('catalog', result.status === 'ready' ?
      Object.fromEntries(result.catalog.recipes.map(row => [row.recipe, row])) : undefined, result.connection);
    const next = result.status === 'ready' ? `${result.connection.generation}:${result.catalog.catalog_id}` : '';
    if (next !== identity.current) {
      identity.current = next;
      window.dispatchEvent(new Event('backendCatalogChanged'));
    }
    return result;
  }, []);
  useEffect(() => {
    const update = () => { void refresh().catch(() => {}); };
    const unsubscribe = serverConfig.onConnectionChange(connection => {
      setState({ status: 'loading', connection });
      update();
    });
    window.addEventListener('backendsUpdated', update);
    window.addEventListener('backendCatalogAvailable', update);
    return () => {
      unsubscribe();
      window.removeEventListener('backendsUpdated', update);
      window.removeEventListener('backendCatalogAvailable', update);
    };
  }, [refresh]);
  return <CatalogContext.Provider value={{ state, refresh,
    recipe: name => state.status === 'ready' ? state.catalog.recipes.find(row => row.recipe === name) : undefined }}>{children}</CatalogContext.Provider>;
};
export function useBackendCatalog(): CatalogContextValue {
  const context = useContext(CatalogContext);
  if (!context) throw new Error('BackendCatalogProvider is required.');
  return context;
}
