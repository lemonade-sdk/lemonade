import React, { useState, useEffect, useMemo } from 'react';
import {
  AppSettings,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import { serverFetch } from './utils/serverConfig';
import { useModels } from './hooks/useModels';
import { useSystem } from './hooks/useSystem';
import { useInferenceState } from './hooks/useInferenceState';
import { useToast, ToastContainer } from './Toast';
import EmbeddingPanel from './components/panels/EmbeddingPanel';
import RerankingPanel from './components/panels/RerankingPanel';
import TranscriptionPanel from './components/panels/TranscriptionPanel';
import ImageGenerationPanel from './components/panels/ImageGenerationPanel';
import TTSPanel from './components/panels/TTSPanel';
import LLMChatPanel from './components/panels/LLMChatPanel';

interface ChatWindowProps {
  isVisible: boolean;
  width?: number;
}

const ChatWindow: React.FC<ChatWindowProps> = ({ isVisible, width }) => {
  const {
    modelsData,
    selectedModel,
    setSelectedModel,
    userHasSelectedModel,
    setUserHasSelectedModel,
  } = useModels();
  const { checkForRocmUsage } = useSystem();
  const inference = useInferenceState();
  const { toasts, removeToast, showError } = useToast();

  const [currentLoadedModel, setCurrentLoadedModel] = useState<string | null>(null);
  const [appSettings, setAppSettings] = useState<AppSettings | null>(null);
  const [resetKey, setResetKey] = useState(0);

  type ModelType = 'llm' | 'embedding' | 'reranking' | 'transcription' | 'image' | 'speech';

  const modelType = useMemo((): ModelType => {
    if (!selectedModel) return 'llm';
    const info = modelsData[selectedModel];
    if (!info) return 'llm';
    if (info.labels?.includes('embeddings') || (info as any)?.embedding) return 'embedding';
    if (info.labels?.includes('reranking') || (info as any)?.reranking) return 'reranking';
    if (info.recipe === 'whispercpp') return 'transcription';
    if (info.recipe === 'sd-cpp' || info.labels?.includes('image')) return 'image';
    if (info.recipe === 'kokoro') return 'speech';
    return 'llm';
  }, [selectedModel, modelsData]);

  // Lock the rendered panel type during inference so that loading a
  // different-modality model via Model Manager doesn't yank the current
  // panel out from under the user mid-inference.
  const [activeModelType, setActiveModelType] = useState<ModelType>(modelType);
  useEffect(() => {
    if (!inference.isBusy) {
      setActiveModelType(modelType);
    }
  }, [modelType, inference.isBusy]);

  const isVision = useMemo(() => {
    if (!selectedModel) return false;
    return modelsData[selectedModel]?.labels?.includes('vision') || false;
  }, [selectedModel, modelsData]);

  const fetchLoadedModel = async () => {
    try {
      const response = await serverFetch('/health');
      const data = await response.json();
      if (data?.model_loaded) {
        setCurrentLoadedModel(data.model_loaded);
        if (!userHasSelectedModel) {
          setSelectedModel(data.model_loaded);
        }
      } else {
        setCurrentLoadedModel(null);
      }
    } catch (error) {
      console.error('Failed to fetch loaded model:', error);
    }
  };

  useEffect(() => {
    fetchLoadedModel();

    const loadSettings = async () => {
      if (!window.api?.getSettings) return;
      try {
        const stored = await window.api.getSettings();
        setAppSettings(mergeWithDefaultSettings(stored));
      } catch (error) {
        console.error('Failed to load app settings:', error);
      }
    };
    loadSettings();

    const unsubscribeSettings = window.api?.onSettingsUpdated?.((updated) => {
      setAppSettings(mergeWithDefaultSettings(updated));
    });

    const handleModelLoadEnd = (event: Event) => {
      const customEvent = event as CustomEvent<{ modelId?: string }>;
      const loadedModelId = customEvent.detail?.modelId;
      if (loadedModelId) {
        setCurrentLoadedModel(loadedModelId);
        setSelectedModel(loadedModelId);
        setUserHasSelectedModel(false);
      } else {
        fetchLoadedModel();
      }
      checkForRocmUsage();
    };

    const handleModelUnload = () => {
      setCurrentLoadedModel(null);
    };

    const handleModelLoadStart = (e: CustomEvent) => {
      setSelectedModel(e.detail.modelId);
    };

    window.addEventListener('modelLoadStart' as any, handleModelLoadStart);
    window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);
    window.addEventListener('modelUnload' as any, handleModelUnload);

    const healthCheckInterval = setInterval(() => {
      fetchLoadedModel();
    }, 5000);

    return () => {
      window.removeEventListener('modelLoadStart' as any, handleModelLoadStart);
      window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
      window.removeEventListener('modelUnload' as any, handleModelUnload);
      clearInterval(healthCheckInterval);
      if (typeof unsubscribeSettings === 'function') {
        unsubscribeSettings();
      }
    };
  }, [setSelectedModel, setUserHasSelectedModel]);

  const handleNewChat = () => {
    inference.reset();
    setResetKey(k => k + 1);
  };

  if (!isVisible) return null;

  const headerTitle = activeModelType === 'embedding' ? 'Lemonade Embeddings'
    : activeModelType === 'reranking' ? 'Lemonade Reranking'
    : activeModelType === 'transcription' ? 'Lemonade Transcriber'
    : activeModelType === 'image' ? 'Lemonade Image Generator'
    : activeModelType === 'speech' ? 'Lemonade Text to Speech'
    : 'LLM Chat';

  const sharedProps = {
    isBusy: inference.isBusy,
    isPreFlight: inference.isPreFlight,
    isInferring: inference.isInferring,
    activeModality: inference.activeModality,
    runPreFlight: inference.runPreFlight,
    reset: inference.reset,
    showError,
    appSettings,
  };

  return (
    <div className={`chat-window ${activeModelType === 'llm' ? 'chat-window-llm' : ''}`} style={width ? { width: `${width}px` } : undefined}>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <div className="chat-header">
        <h3>{headerTitle}</h3>
        <button
          className="new-chat-button"
          onClick={handleNewChat}
          disabled={inference.isBusy}
          title={activeModelType === 'llm' ? 'Start a new chat' : 'Clear'}
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
            <path
              d="M21 3V8M21 8H16M21 8L18 5.29168C16.4077 3.86656 14.3051 3 12 3C7.02944 3 3 7.02944 3 12C3 16.9706 7.02944 21 12 21C16.2832 21 19.8675 18.008 20.777 14"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
          </svg>
        </button>
      </div>

      {activeModelType === 'embedding' && <EmbeddingPanel key={resetKey} {...sharedProps} />}
      {activeModelType === 'reranking' && <RerankingPanel key={resetKey} {...sharedProps} />}
      {activeModelType === 'transcription' && <TranscriptionPanel key={resetKey} {...sharedProps} />}
      {activeModelType === 'image' && <ImageGenerationPanel key={resetKey} {...sharedProps} />}
      {activeModelType === 'speech' && <TTSPanel key={resetKey} {...sharedProps} />}
      {activeModelType === 'llm' && (
        <LLMChatPanel
          key={resetKey}
          {...sharedProps}
          isVision={isVision}
          currentLoadedModel={currentLoadedModel}
          setCurrentLoadedModel={setCurrentLoadedModel}
        />
      )}
    </div>
  );
};

export default ChatWindow;
