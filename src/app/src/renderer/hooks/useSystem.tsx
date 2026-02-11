import React, {createContext, useCallback, useContext, useMemo, useState} from "react";
import {fetchSystemInfoData, SystemInfo, Recipes, fetchSystemChecks, SystemCheck} from "../utils/systemData";

interface SystemContextValue {
  systemInfo?: SystemInfo;
  isLoading: boolean;
  supportedRecipes: SupportedRecipes;
  systemChecks: SystemCheck[];
  refresh: () => Promise<void>;
  ensureSystemInfoLoaded: () => Promise<void>;
}

// Programmatic structure: recipe -> list of supported backends
export interface SupportedRecipes {
  [recipeName: string]: string[]; // e.g., { llamacpp: ['vulkan', 'rocm', 'cpu'], 'ryzenai-llm': ['default'] }
}

const SystemContext = createContext<SystemContextValue | null>(null);

export const SystemProvider: React.FC<{ children: React.ReactNode }> = ({children}) => {
  const [systemInfo, setSystemInfo] = useState<SystemInfo>();
  const [systemChecks, setSystemChecks] = useState<SystemCheck[]>([]);
  const [isLoading, setIsLoading] = useState(false); // Changed to false - no longer loading on startup
  const [hasLoaded, setHasLoaded] = useState(false); // Track if we've ever loaded system info

  // Programmatically extract supported recipes and backends
  const supportedRecipes = useMemo<SupportedRecipes>(() => {
    const result: SupportedRecipes = {};

    const recipes = systemInfo?.recipes;
    if (!recipes) return result;

    // Iterate over all recipes dynamically
    for (const [recipeName, recipe] of Object.entries(recipes)) {
      if (!recipe?.backends) continue;

      // Collect all supported backends for this recipe (not just available/installed)
      const supportedBackends: string[] = [];
      for (const [backendName, backend] of Object.entries(recipe.backends)) {
        if (backend?.supported) {
          supportedBackends.push(backendName);
        }
      }

      // Only include recipes that have at least one supported backend
      if (supportedBackends.length > 0) {
        result[recipeName] = supportedBackends;
      }
    }

    return result;
  }, [systemInfo]);

  // Fetch system info from the server
  const refresh = useCallback(async () => {
    setIsLoading(true);
    try {
      const data = await fetchSystemInfoData();
      setSystemInfo(data.info);

      // Fetch system checks (kernel issues, driver warnings, etc.)
      const checks = await fetchSystemChecks();
      setSystemChecks(checks);

      setHasLoaded(true);

    } catch (error) {
      console.error('Failed to fetch system info:', error);
    } finally {
      setIsLoading(false);
    }
  }, []);

  // Ensure system info is loaded (for lazy loading on first model use)
  const ensureSystemInfoLoaded = useCallback(async () => {
    if (!hasLoaded && !isLoading) {
      await refresh();
    }
  }, [hasLoaded, isLoading, refresh]);

  // No initial load - system info will be fetched when first needed
  // (e.g., when user tries to load a model)

  const value: SystemContextValue = {
    systemInfo,
    supportedRecipes,
    systemChecks,
    isLoading,
    refresh,
    ensureSystemInfoLoaded,
  };

  return (
      <SystemContext.Provider value={value}>
        {children}
      </SystemContext.Provider>
  );
};

export const useSystem = (): SystemContextValue => {
  const context = useContext(SystemContext);
  if (!context) {
    throw new Error('useSystem must be used within a SystemProvider');
  }
  return context;
};
