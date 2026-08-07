import React, { createContext, useContext, useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { ModelsData, ModelInfo, USER_MODEL_PREFIX, fetchSupportedModelsData } from '../utils/modelData';
import { onServerPortChange, serverFetch } from '../utils/serverConfig';
import { isModelEffectivelyDownloaded } from '../utils/collectionModels';

// Default model to use when no models are downloaded (first-time user experience)
export const DEFAULT_MODEL_ID = 'Qwen3-0.6B-GGUF';

export interface DownloadedModel {
  id: string;
  info: ModelInfo;
}

export interface SuggestedModel {
  name: string;
  info: ModelInfo;
}

interface ModelsContextValue {
  // All models with full metadata (keyed by model ID)
  modelsData: ModelsData;

  // List of downloaded models (derived from modelsData)
  downloadedModels: DownloadedModel[];

  // IDs of models currently loaded in the server (used to float them to the
  // top of the chat model dropdown)
  loadedModelIds: string[];

  // List of suggested models for Model Manager (derived from modelsData)
  suggestedModels: SuggestedModel[];

  // Currently selected model in chat dropdown
  selectedModel: string;
  setSelectedModel: (model: string) => void;

  // Whether the selected model is the default (not yet downloaded)
  isDefaultModelPending: boolean;

  // Whether data is currently being fetched
  isLoading: boolean;

  // Refresh model data from server
  refresh: () => Promise<void>;

  // Track if user has manually selected a model
  userHasSelectedModel: boolean;
  setUserHasSelectedModel: (value: boolean) => void;
}

const ModelsContext = createContext<ModelsContextValue | null>(null);

export const ModelsProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [modelsData, setModelsData] = useState<ModelsData>({});
  const [selectedModel, setSelectedModelState] = useState<string>('');
  const [isDefaultModelPending, setIsDefaultModelPending] = useState(false);
  const [isLoading, setIsLoading] = useState(true);
  const [loadedModelIds, setLoadedModelIds] = useState<string[]>([]);
  const userHasSelectedModelRef = useRef(false);

  // Derive downloaded models from modelsData
  const downloadedModels = useMemo<DownloadedModel[]>(() => {
    return Object.entries(modelsData)
      .filter(([id, info]) => isModelEffectivelyDownloaded(id, info, modelsData))
      .map(([id, info]) => ({ id, info }));
  }, [modelsData]);

  // Derive suggested models for Model Manager (suggested + user models)
  const suggestedModels = useMemo<SuggestedModel[]>(() => {
    return Object.entries(modelsData)
      .filter(([name, info]) => info.suggested || name.startsWith(USER_MODEL_PREFIX))
      .map(([name, info]) => ({ name, info }))
      .sort((a, b) => a.name.localeCompare(b.name));
  }, [modelsData]);

  // Fetch all models data from server
  const refresh = useCallback(async () => {
    setIsLoading(true);
    try {
      const data = await fetchSupportedModelsData();
      setModelsData(data);

      // Check if any models are downloaded
      const hasDownloadedModels = Object.entries(data).some(([id, info]) => isModelEffectivelyDownloaded(id, info, data));

      if (!hasDownloadedModels) {
        // No models downloaded - show default model in dropdown
        setIsDefaultModelPending(true);
        setSelectedModelState(DEFAULT_MODEL_ID);
      } else {
        // Models are available
        setIsDefaultModelPending(false);

        // Update selected model if needed
        setSelectedModelState(prev => {
          // If no selection or selection is the fake default, use first downloaded model
          if (!prev || prev === DEFAULT_MODEL_ID) {
            const firstDownloaded = Object.entries(data).find(([id, info]) => isModelEffectivelyDownloaded(id, info, data));
            return firstDownloaded ? firstDownloaded[0] : '';
          }
          // If the previously selected model is still downloaded, keep it
          if (data[prev] && isModelEffectivelyDownloaded(prev, data[prev], data)) {
            return prev;
          }
          // Otherwise, select the first downloaded model
          const firstDownloaded = Object.entries(data).find(([id, info]) => isModelEffectivelyDownloaded(id, info, data));
          return firstDownloaded ? firstDownloaded[0] : '';
        });
      }
    } catch (error) {
      console.error('Failed to fetch models:', error);
    } finally {
      setIsLoading(false);
    }
  }, []);

  // Wrapper for setSelectedModel that also clears default pending state if needed
  const setSelectedModel = useCallback((model: string) => {
    userHasSelectedModelRef.current = true;
    setSelectedModelState(model);
  }, []);

  // Fetch which models are currently loaded in the server
  const fetchLoadedModels = useCallback(async () => {
    try {
      const response = await serverFetch('/health');
      const data = await response.json();
      const loaded: Array<{ model_name?: string }> = Array.isArray(data?.all_models_loaded)
        ? data.all_models_loaded
        : [];
      const ids = loaded
        .map((entry) => entry?.model_name)
        .filter((name): name is string => typeof name === 'string' && name.length > 0);
      setLoadedModelIds((prev) =>
        prev.length === ids.length && prev.every((id, i) => id === ids[i]) ? prev : ids
      );
    } catch (error) {
      console.error('Failed to fetch loaded models:', error);
    }
  }, []);

  // Initial load
  useEffect(() => {
    refresh();
  }, [refresh]);

  // Track loaded models: poll periodically and react to load/unload events so
  // the chat dropdown can float loaded models to the top. Polling also picks up
  // changes made by other clients driving the same server.
  useEffect(() => {
    fetchLoadedModels();

    const handleChange = () => fetchLoadedModels();
    window.addEventListener('modelLoadStart', handleChange);
    window.addEventListener('modelLoadEnd', handleChange);
    window.addEventListener('modelUnload', handleChange);

    const interval = setInterval(fetchLoadedModels, 5000);

    return () => {
      window.removeEventListener('modelLoadStart', handleChange);
      window.removeEventListener('modelLoadEnd', handleChange);
      window.removeEventListener('modelUnload', handleChange);
      clearInterval(interval);
    };
  }, [fetchLoadedModels]);

  // Listen for modelsUpdated and backendsUpdated events
  // (backend installs/uninstalls can change which models are available)
  useEffect(() => {
    const handleRefresh = () => {
      refresh();
    };

    window.addEventListener('modelsUpdated', handleRefresh);
    window.addEventListener('backendsUpdated', handleRefresh);

    return () => {
      window.removeEventListener('modelsUpdated', handleRefresh);
      window.removeEventListener('backendsUpdated', handleRefresh);
    };
  }, [refresh]);

  // Listen for server port changes
  useEffect(() => {
    const unsubscribe = onServerPortChange(() => {
      console.log('Server port changed, refreshing models...');
      refresh();
      fetchLoadedModels();
    });

    return unsubscribe;
  }, [refresh, fetchLoadedModels]);

  const value: ModelsContextValue = {
    modelsData,
    downloadedModels,
    loadedModelIds,
    suggestedModels,
    selectedModel,
    setSelectedModel,
    isDefaultModelPending,
    isLoading,
    refresh,
    userHasSelectedModel: userHasSelectedModelRef.current,
    setUserHasSelectedModel: (value: boolean) => {
      userHasSelectedModelRef.current = value;
    },
  };

  return (
    <ModelsContext.Provider value={value}>
      {children}
    </ModelsContext.Provider>
  );
};

export const useModels = (): ModelsContextValue => {
  const context = useContext(ModelsContext);
  if (!context) {
    throw new Error('useModels must be used within a ModelsProvider');
  }
  return context;
};
