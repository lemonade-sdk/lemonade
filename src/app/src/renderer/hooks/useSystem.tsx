import React, {createContext, useCallback, useContext, useEffect, useMemo, useState} from "react";
import {fetchSystemInfoData, SystemInfo, fetchSystemChecks, SystemCheck} from "../utils/systemData";

interface SystemContextValue {
  systemInfo?: SystemInfo;
  isLoading: boolean;
  supportedRecipes: SupportedRecipes;
  systemChecks: SystemCheck[];
  shouldShowSystemChecks: boolean;
  checkForRocmUsage: () => Promise<void>;
  dismissSystemChecks: (permanent: boolean) => void;
  refresh: () => Promise<void>;
  ensureSystemInfoLoaded: () => Promise<void>;
}

// Programmatic structure: recipe -> list of supported backends
export interface SupportedRecipes {
  [recipeName: string]: string[]; // e.g., { llamacpp: ['vulkan', 'rocm', 'cpu'], 'ryzenai-llm': ['npu'] }
}

const SystemContext = createContext<SystemContextValue | null>(null);

const SYSTEM_CHECKS_DISMISSED_KEY = 'lemonade_system_checks_dismissed';

export const SystemProvider: React.FC<{ children: React.ReactNode }> = ({children}) => {
  const [systemInfo, setSystemInfo] = useState<SystemInfo>();
  const [systemChecks, setSystemChecks] = useState<SystemCheck[]>([]);
  const [shouldShowSystemChecks, setShouldShowSystemChecks] = useState(false);
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

      // Collect all backends that are viable on this system
      const supportedBackends: string[] = [];
      for (const [backendName, backend] of Object.entries(recipe.backends)) {
        if (backend?.state && backend.state !== 'unsupported') {
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

  // Check if user has dismissed system checks permanently
  const isSystemChecksDismissed = useCallback(() => {
    try {
      const dismissed = localStorage.getItem(SYSTEM_CHECKS_DISMISSED_KEY);
      return dismissed === 'true';
    } catch {
      return false;
    }
  }, []);

  const checkForRocmUsage = useCallback(async () => {
    // Fetch fresh system checks to avoid stale state from closures
    let checks: SystemCheck[] = [];
    try {
      checks = await fetchSystemChecks();
      setSystemChecks(checks);
    } catch (error) {
      console.error('Failed to fetch system checks:', error);
      return;
    }

    if (isSystemChecksDismissed()) {
      return;
    }

    if (checks.length === 0) {
      setShouldShowSystemChecks(false);
      return;
    }

    const checkHealth = async (retryCount = 0): Promise<boolean> => {
      try {
        const { serverFetch } = await import('../utils/serverConfig');
        const response = await serverFetch('/health');
        if (!response.ok) {
          return false;
        }

        const data = await response.json();
        const allModelsLoaded = data.all_models_loaded || [];

        const hasRocmModel = allModelsLoaded.some((model: any) => {
          const recipeOptions = model.recipe_options || {};
          const recipe = model.recipe || '';

          if (recipe === 'llamacpp' && recipeOptions.llamacpp_backend === 'rocm') {
            return true;
          }

          if (recipe === 'sd-cpp' && recipeOptions['sd-cpp_backend'] === 'rocm') {
            return true;
          }

          return false;
        });

        // Retry once after a delay if model hasn't loaded yet
        if (!hasRocmModel && retryCount === 0 && allModelsLoaded.length === 0) {
          await new Promise(resolve => setTimeout(resolve, 500));
          return checkHealth(1);
        }

        return hasRocmModel;
      } catch (error) {
        console.error('Failed to check for ROCm usage:', error);
        return false;
      }
    };

    const hasRocmModel = await checkHealth();
    setShouldShowSystemChecks(hasRocmModel);
  }, [isSystemChecksDismissed]);

  // Dismiss system checks modal
  const dismissSystemChecks = useCallback((permanent: boolean) => {
    setShouldShowSystemChecks(false);
    if (permanent) {
      try {
        localStorage.setItem(SYSTEM_CHECKS_DISMISSED_KEY, 'true');
      } catch (error) {
        console.error('Failed to save dismiss preference:', error);
      }
    }
  }, []);

  // Auto-refresh when a backend install completes (from any codepath)
  useEffect(() => {
    const handleBackendsUpdated = () => {
      refresh();
    };
    window.addEventListener('backendsUpdated', handleBackendsUpdated);
    return () => {
      window.removeEventListener('backendsUpdated', handleBackendsUpdated);
    };
  }, [refresh]);

  // No initial load - system info will be fetched when first needed
  // (e.g., when user tries to load a model)

  const value: SystemContextValue = {
    systemInfo,
    supportedRecipes,
    systemChecks,
    shouldShowSystemChecks,
    isLoading,
    refresh,
    ensureSystemInfoLoaded,
    checkForRocmUsage,
    dismissSystemChecks,
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
