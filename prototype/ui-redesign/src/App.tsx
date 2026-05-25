import React, { useState, useEffect, useCallback } from 'react';
import api, { ConnectionStatus, LoadedModel } from './api';
import ChatView from './components/ChatView';
import ModelManager from './components/ModelManager';
import ConnectView from './components/ConnectView';
import PresetManager from './components/PresetManager';
import BackendManager from './components/BackendManager';

type View = 'chat' | 'models' | 'presets' | 'backends' | 'connect';

const App: React.FC = () => {
  const [view, setView] = useState<View>('chat');
  const [status, setStatus] = useState<ConnectionStatus>(api.status);
  const [currentModel, setCurrentModel] = useState<string | null>(null);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);

  useEffect(() => {
    const unsub = api.onStatusChange(setStatus);
    api.connect().then(connected => {
      if (connected) {
        const loaded = api.loadedModels;
        setLoadedModels(loaded);
        const llm = loaded.find(m => m.type === 'llm');
        if (llm) setCurrentModel(llm.model_name);
      }
    });
    api.startPolling(15000);
    return () => { unsub(); api.stopPolling(); };
  }, []);

  const handleRefreshModels = useCallback(async () => {
    const result = await api.refresh();
    if (result) {
      setLoadedModels(result.health.all_models_loaded);
    }
  }, []);

  const handleModelSelect = useCallback((modelName: string) => {
    setCurrentModel(modelName);
    setView('chat');
  }, []);

  return (
    <div className="app">
      <header className="titlebar">
        <div className="titlebar__brand">
          <span className="titlebar__lemon" aria-hidden="true" />
          <span>lemonade</span>
        </div>

        <nav className="titlebar__nav" aria-label="Primary">
          {(['chat', 'models', 'presets', 'backends', 'connect'] as View[]).map(v => (
            <button
              key={v}
              className={view === v ? 'is-active' : ''}
              onClick={() => setView(v)}
            >
              {v.charAt(0).toUpperCase() + v.slice(1)}
            </button>
          ))}
        </nav>

        <div className="titlebar__right">
          <button className="model-selector" aria-label="Active model">
            <span className={`model-selector__dot ${
              status === 'connected' ? 'model-selector__dot--connected' :
              status === 'connecting' ? 'model-selector__dot--connecting' : ''
            }`} aria-hidden="true" />
            <span className="model-selector__name">
              {currentModel || (status === 'connected' ? 'No model' : 'Offline')}
            </span>
            <span className="model-selector__caret">▾</span>
          </button>
        </div>
      </header>

      <div className="view-container">
        {view === 'chat' && (
          <ChatView
            currentModel={currentModel}
            loadedModels={loadedModels}
            onModelSelect={handleModelSelect}
            onRefresh={handleRefreshModels}
          />
        )}
        {view === 'models' && (
          <ModelManager
            onModelSelect={handleModelSelect}
            selectedModel={currentModel}
          />
        )}
        {view === 'presets' && (
          <PresetManager loadedModels={loadedModels} />
        )}
        {view === 'backends' && (
          <BackendManager />
        )}
        {view === 'connect' && (
          <ConnectView status={status} />
        )}
      </div>
    </div>
  );
};

export default App;
