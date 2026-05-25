import React, { useState, useEffect, useCallback, Component, ErrorInfo, ReactNode } from 'react';
import api, { ConnectionStatus, LoadedModel } from './api';
import ChatView from './components/ChatView';
import ModelManager from './components/ModelManager';
import ConnectView from './components/ConnectView';
import PresetManager from './components/PresetManager';
import BackendManager from './components/BackendManager';
import Dashboard from './components/Dashboard';
import LogViewer from './components/LogViewer';

type View = 'chat' | 'models' | 'presets' | 'backends' | 'dashboard' | 'logs' | 'connect';

/* ── Error boundary ────────────────────────────────────────── */

interface ErrorBoundaryProps { view: string; children: ReactNode; }
interface ErrorBoundaryState { error: Error | null; }

class ViewErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
  state: ErrorBoundaryState = { error: null };

  static getDerivedStateFromError(error: Error) { return { error }; }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error(`[${this.props.view}] Render error:`, error, info.componentStack);
  }

  componentDidUpdate(prev: ErrorBoundaryProps) {
    if (prev.view !== this.props.view) this.setState({ error: null });
  }

  render() {
    if (this.state.error) {
      return (
        <div style={{ padding: '2rem', color: '#e8c66b', fontFamily: 'var(--font-mono, monospace)' }}>
          <h2 style={{ color: '#ff6b6b', margin: '0 0 1rem' }}>
            Something went wrong in "{this.props.view}"
          </h2>
          <pre style={{ whiteSpace: 'pre-wrap', color: '#ccc', fontSize: '13px' }}>
            {this.state.error.message}
          </pre>
          <button
            onClick={() => this.setState({ error: null })}
            style={{ marginTop: '1rem', padding: '0.5rem 1rem', background: '#e8c66b', color: '#16140f', border: 'none', borderRadius: '6px', cursor: 'pointer' }}
          >
            Try again
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}

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
          {(['chat', 'models', 'presets', 'backends', 'dashboard', 'logs', 'connect'] as View[]).map(v => (
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
        <ViewErrorBoundary view={view}>
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
        {view === 'dashboard' && (
          <Dashboard />
        )}
        {view === 'logs' && (
          <LogViewer />
        )}
        {view === 'connect' && (
          <ConnectView status={status} />
        )}
        </ViewErrorBoundary>
      </div>
    </div>
  );
};

export default App;
