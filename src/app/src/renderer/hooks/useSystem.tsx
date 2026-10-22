import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { fetchSystemInfoData, SystemInfo } from '../utils/systemData';
import { serverConfig } from '../utils/serverConfig';
import { replaceRecipeDisplayNames } from '../utils/recipeNames';

interface SystemContextValue {
  systemInfo?: SystemInfo;
  isLoading: boolean;
  supportedRecipes: SupportedRecipes;
  refresh: () => Promise<void>;
  ensureSystemInfoLoaded: () => Promise<void>;
}
export interface SupportedRecipes { [recipeName: string]: string[]; }
const SystemContext = createContext<SystemContextValue | null>(null);

export const SystemProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [data, setData] = useState<{ generation: number; epoch: number; info?: SystemInfo }>();
  const [isLoading, setIsLoading] = useState(false);
  const epoch = useRef(0);
  const loaded = useRef<string | undefined>(undefined);
  const pending = useRef<{ generation: number; epoch: number; promise: Promise<void> } | undefined>(undefined);
  const systemInfo = data?.generation === serverConfig.getSnapshot().generation && data.epoch === epoch.current ? data.info : undefined;
  const supportedRecipes = useMemo<SupportedRecipes>(() => {
    const result: SupportedRecipes = Object.create(null);
    for (const [name, recipe] of Object.entries(systemInfo?.recipes ?? {})) {
      const backends = Object.entries(recipe.backends ?? {}).filter(([, backend]) => backend.state && backend.state !== 'unsupported').map(([backend]) => backend);
      if (backends.length) result[name] = backends;
    }
    return result;
  }, [systemInfo]);
  const refresh = useCallback(async () => {
    await serverConfig.waitForInit();
    const connection = serverConfig.getSnapshot();
    const requestEpoch = epoch.current;
    if (pending.current?.generation === connection.generation && pending.current.epoch === requestEpoch) return pending.current.promise;
    setIsLoading(true);
    const promise = (async () => {
      try {
        const result = await fetchSystemInfoData(connection);
        serverConfig.assertCurrent(connection);
        if (requestEpoch !== epoch.current) return;
        setData({ generation: connection.generation, epoch: requestEpoch, info: result.info });
        loaded.current = `${connection.generation}:${requestEpoch}`;
        replaceRecipeDisplayNames('system', result.info?.recipes, connection);
      } catch {
        if (serverConfig.getSnapshot().generation === connection.generation && requestEpoch === epoch.current) {
          setData(undefined);
          loaded.current = undefined;
          replaceRecipeDisplayNames('system', undefined, connection);
        }
      } finally {
        if (pending.current?.generation === connection.generation && pending.current.epoch === requestEpoch) { pending.current = undefined; setIsLoading(false); }
      }
    })();
    pending.current = { generation: connection.generation, epoch: requestEpoch, promise };
    return promise;
  }, []);
  const ensureSystemInfoLoaded = useCallback(async () => {
    if (loaded.current !== `${serverConfig.getSnapshot().generation}:${epoch.current}`) await refresh();
  }, [refresh]);
  useEffect(() => {
    const update = () => { void refresh(); };
    const invalidate = () => {
      epoch.current += 1;
      setData(undefined);
      loaded.current = undefined;
      replaceRecipeDisplayNames('system', undefined, serverConfig.getSnapshot());
      update();
    };
    const unsubscribe = serverConfig.onConnectionChange(invalidate);
    window.addEventListener('backendsUpdated', update);
    window.addEventListener('backendCatalogChanged', invalidate);
    return () => {
      unsubscribe();
      window.removeEventListener('backendsUpdated', update);
      window.removeEventListener('backendCatalogChanged', invalidate);
    };
  }, [refresh]);
  return <SystemContext.Provider value={{ systemInfo, supportedRecipes, isLoading, refresh, ensureSystemInfoLoaded }}>{children}</SystemContext.Provider>;
};
export const useSystem = (): SystemContextValue => {
  const context = useContext(SystemContext);
  if (!context) throw new Error('useSystem must be used within a SystemProvider');
  return context;
};
