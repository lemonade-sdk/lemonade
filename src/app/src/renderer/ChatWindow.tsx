import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import { createPortal } from 'react-dom';
import {
  AppSettings,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import { serverFetch } from './utils/serverConfig';
import { useModels } from './hooks/useModels';
import { useInferenceState } from './hooks/useInferenceState';
import { useToast, ToastContainer } from './Toast';
import EmbeddingPanel from './components/panels/EmbeddingPanel';
import RerankingPanel from './components/panels/RerankingPanel';
import TranscriptionPanel from './components/panels/TranscriptionPanel';
import ImageGenerationPanel from './components/panels/ImageGenerationPanel';
import TTSPanel from './components/panels/TTSPanel';
import LLMChatPanel from './components/panels/LLMChatPanel';
import { RefreshIcon } from './components/Icons';
import { isCollectionModel, getCollectionPrimaryChatModel, getCollectionComponents } from './utils/collectionModels';
import AddModelPanel, { AddModelInitialValues, ModelInstallData } from './AddModelPanel';

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
  const inference = useInferenceState();
  const { toasts, removeToast, showError } = useToast();

  const [currentLoadedModel, setCurrentLoadedModel] = useState<string | null>(null);
  const [appSettings, setAppSettings] = useState<AppSettings | null>(null);
  const [resetKey, setResetKey] = useState(0);
  const [showAddModelForm, setShowAddModelForm] = useState(false);
  const [addModelInitialValues, setAddModelInitialValues] = useState<AddModelInitialValues | undefined>(undefined);
  const addModelFromJSONRef = useRef<HTMLInputElement>(null);

  type ModelType = 'llm' | 'embedding' | 'reranking' | 'transcription' | 'image' | 'speech';

  const modelType = useMemo((): ModelType => {
    if (!selectedModel) return 'llm';
    const info = modelsData[selectedModel];
    if (!info) return 'llm';
    if (isCollectionModel(info)) return 'llm';
    // Chat-indicator labels win over modality labels so multimodal "any-to-text"
    // models (e.g. Gemma 4 on FLM) — which carry both "vision" / "tool-calling"
    // AND modality labels like "transcription" — route to the LLM
    // panel rather than the Transcription/Image panel.
    const chatIndicators = ['vision', 'reasoning', 'tool-calling', 'tools'];
    if (info.labels?.some(l => chatIndicators.includes(l))) return 'llm';
    if (info.labels?.includes('embeddings') || (info as any)?.embedding) return 'embedding';
    if (info.labels?.includes('reranking') || (info as any)?.reranking) return 'reranking';
    if (info.labels?.includes('transcription')) return 'transcription';
    if (info.labels?.includes('image')) return 'image';
    if (info.labels?.includes('speech')) return 'speech';
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
    const info = modelsData[selectedModel];
    if (isCollectionModel(info)) {
      const chatModel = getCollectionPrimaryChatModel(selectedModel, modelsData);
      return modelsData[chatModel]?.labels?.includes('vision') || false;
    }
    return info?.labels?.includes('vision') || false;
  }, [selectedModel, modelsData]);

  // A multimodal chat model that accepts audio *as input* to a chat turn.
  // Models with the "chat-transcription" label can handle audio in
  // /chat/completions; distinct from pure ASR (Whisper) models which serve
  // /audio/transcriptions via the "transcription" label.
  const isAudioChat = useMemo(() => {
    if (!selectedModel) return false;
    const labels = modelsData[selectedModel]?.labels || [];
    return labels.includes('chat-transcription');
  }, [selectedModel, modelsData]);

  const isCollectionSelected = useMemo(() => {
    if (!selectedModel) return false;
    return isCollectionModel(modelsData[selectedModel]);
  }, [selectedModel, modelsData]);

  const collectionMode = activeModelType === 'llm' && isCollectionSelected;

  // Use refs so the mount-once effect can read current values without re-running
  const selectedModelRef = useRef(selectedModel);
  selectedModelRef.current = selectedModel;
  const modelsDataRef = useRef(modelsData);
  modelsDataRef.current = modelsData;
  const userHasSelectedModelRef = useRef(userHasSelectedModel);
  userHasSelectedModelRef.current = userHasSelectedModel;

  const fetchLoadedModel = useCallback(async () => {
    try {
      const response = await serverFetch('/health');
      const data = await response.json();
      if (data?.model_loaded) {
        setCurrentLoadedModel(data.model_loaded);
        const selectedInfo = selectedModelRef.current ? modelsDataRef.current[selectedModelRef.current] : undefined;
        const keepCollectionSelection = !!selectedInfo && isCollectionModel(selectedInfo);
        if (!userHasSelectedModelRef.current && !keepCollectionSelection) {
          setSelectedModel(data.model_loaded);
        }
      } else {
        setCurrentLoadedModel(null);
      }
    } catch (error) {
      console.error('Failed to fetch loaded model:', error);
    }
  }, [setSelectedModel]);

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
      } else {
        fetchLoadedModel();
      }
    };

    const handleModelUnload = () => {
      setCurrentLoadedModel(null);
    };

    const handleModelLoadStart = (e: CustomEvent) => {
      setSelectedModel(e.detail.modelId);
      setUserHasSelectedModel(true);
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
  }, [fetchLoadedModel, setSelectedModel, setUserHasSelectedModel]);

  useEffect(() => {
    const handleOpenAddModel = (e: Event) => {
      const detail = (e as CustomEvent).detail;
      setAddModelInitialValues(detail?.initialValues ?? undefined);
      setShowAddModelForm(true);
    };
    const handleOpenAddModelFromJSON = () => {
      addModelFromJSONRef.current?.click();
    };
    window.addEventListener('openAddModel', handleOpenAddModel);
    window.addEventListener('openAddModelFromJSON', handleOpenAddModelFromJSON);
    return () => {
      window.removeEventListener('openAddModel', handleOpenAddModel);
      window.removeEventListener('openAddModelFromJSON', handleOpenAddModelFromJSON);
    };
  }, []);

  const handleAddModelInstall = (data: ModelInstallData) => {
    setShowAddModelForm(false);
    setAddModelInitialValues(undefined);
    const modelName = data.name.startsWith('user.') ? data.name : `user.${data.name}`;
    window.dispatchEvent(new CustomEvent('installModel', {
      detail: {
        name: modelName,
        registrationData: {
          checkpoint: data.checkpoint,
          recipe: data.recipe,
          mmproj: data.mmproj,
          reasoning: data.reasoning,
          vision: data.vision,
          embedding: data.embedding,
          reranking: data.reranking,
        },
      },
    }));
  };

  const handleNewChat = () => {
    inference.reset();
    setResetKey(k => k + 1);
  };

  const handleUnloadCollectionModel = async () => {
    if (!selectedModel || inference.isBusy) return;

    try {
      const info = modelsData[selectedModel];
      const components = isCollectionModel(info) ? getCollectionComponents(info) : [selectedModel];

      for (const component of components) {
        const response = await serverFetch('/unload', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ model_name: component }),
        });

        if (!response.ok) {
          throw new Error(`Failed to unload ${component}: ${response.statusText}`);
        }
      }

      inference.reset();
      setCurrentLoadedModel(null);
      setSelectedModel('');
      setUserHasSelectedModel(false);
      window.dispatchEvent(new CustomEvent('modelUnload'));
    } catch (error) {
      console.error('Failed to unload collection:', error);
      showError(`Failed to unload model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
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
    <div
      className={`chat-window ${activeModelType === 'llm' ? 'chat-window-llm' : ''}`}
      style={width ? { width: `${width}px` } : undefined}
    >
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      {activeModelType !== 'llm' && (
        <div className="chat-header">
          <h3>{headerTitle}</h3>
          <button
            className="new-chat-button"
            onClick={handleNewChat}
            disabled={inference.isBusy}
            title="Clear"
          >
            <RefreshIcon />
          </button>
        </div>
      )}

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
          isAudioChat={isAudioChat}
          currentLoadedModel={currentLoadedModel}
          setCurrentLoadedModel={setCurrentLoadedModel}
          collectionMode={collectionMode}
          onNewChat={handleNewChat}
          onUnloadCollection={handleUnloadCollectionModel}
        />
      )}
      <input
        ref={addModelFromJSONRef}
        type="file"
        accept=".json"
        style={{ display: 'none' }}
        onChange={(e: React.ChangeEvent<HTMLInputElement>) => {
          const file = e.target.files?.[0];
          if (!file) return;
          const reader = new FileReader();
          reader.onload = (ev) => {
            try {
              const json = JSON.parse(ev.target?.result as string);
              window.dispatchEvent(new CustomEvent('installModelFromJSON', { detail: json }));
            } catch { /* ignore */ }
          };
          reader.readAsText(file);
          e.target.value = '';
        }}
      />
      {showAddModelForm && createPortal(
        <div className="settings-overlay" onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => { if (e.target === e.currentTarget) { setShowAddModelForm(false); setAddModelInitialValues(undefined); } }}>
          <div className="settings-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <AddModelPanel
              onClose={() => { setShowAddModelForm(false); setAddModelInitialValues(undefined); }}
              initialValues={addModelInitialValues}
              onInstall={handleAddModelInstall}
            />
          </div>
        </div>,
        document.body
      )}
    </div>
  );
};

export default ChatWindow;
