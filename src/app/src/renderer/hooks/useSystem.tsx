import React, {createContext, useCallback, useContext, useEffect, useMemo, useState} from "react";
import {fetchSystemInfoData, SystemInfo, Recipes} from "../utils/systemData";

interface SystemContextValue {
  systemInfo?: SystemInfo;
  isLoading: boolean;
  supportedRecipes: SupportedRecipes;
  refresh: () => Promise<void>;
}

// Programmatic structure: recipe -> list of supported backends
export interface SupportedRecipes {
  [recipeName: string]: string[]; // e.g., { llamacpp: ['vulkan', 'rocm', 'cpu'], 'oga-hybrid': ['default'] }
}

const SystemContext = createContext<SystemContextValue | null>(null);

export const SystemProvider: React.FC<{ children: React.ReactNode }> = ({children}) => {
  const [systemInfo, setSystemInfo] = useState<SystemInfo>();
  const [isLoading, setIsLoading] = useState(true);

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

    } catch (error) {
      console.error('Failed to fetch system info:', error);
    } finally {
      setIsLoading(false);
    }
  }, []);

  // Initial load
  useEffect(() => {
    refresh();
  }, [refresh]);

  const value: SystemContextValue = {
    systemInfo,
    supportedRecipes,
    isLoading,
    refresh,
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
