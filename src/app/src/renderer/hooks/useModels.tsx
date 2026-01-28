import React, { createContext, useContext, useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { ModelsData, ModelInfo, USER_MODEL_PREFIX, fetchSupportedModelsData } from '../utils/modelData';
import { onServerPortChange } from '../utils/serverConfig';

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
  const userHasSelectedModelRef = useRef(false);

  // Derive downloaded models from modelsData
  const downloadedModels = useMemo<DownloadedModel[]>(() => {
    return Object.entries(modelsData)
      .filter(([_, info]) => info.downloaded)
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
      const hasDownloadedModels = Object.values(data).some(info => info.downloaded);

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
            const firstDownloaded = Object.entries(data).find(([_, info]) => info.downloaded);
            return firstDownloaded ? firstDownloaded[0] : '';
          }
          // If the previously selected model is still downloaded, keep it
          if (data[prev]?.downloaded) {
            return prev;
          }
          // Otherwise, select the first downloaded model
          const firstDownloaded = Object.entries(data).find(([_, info]) => info.downloaded);
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

  // Initial load
  useEffect(() => {
    refresh();
  }, [refresh]);

  // Listen for modelsUpdated events
  useEffect(() => {
    const handleModelsUpdated = () => {
      console.log('Models updated, refreshing...');
      refresh();
    };

    window.addEventListener('modelsUpdated', handleModelsUpdated);

    return () => {
      window.removeEventListener('modelsUpdated', handleModelsUpdated);
    };
  }, [refresh]);

  // Listen for server port changes
  useEffect(() => {
    const unsubscribe = onServerPortChange(() => {
      console.log('Server port changed, refreshing models...');
      refresh();
    });

    return unsubscribe;
  }, [refresh]);

  const value: ModelsContextValue = {
    modelsData,
    downloadedModels,
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
