import React, { useState, useEffect, useCallback, useMemo, useRef, Component, ErrorInfo, ReactNode } from 'react';
import api, { ConnectionStatus, LoadedModel } from './api';
import { canSelectInComposer, capabilityFromModelInfo, selectPreferredLoadedModel } from './modelCapabilities';
import AccountMenu from './features/accounts/AccountMenu';
import { AccountSession, currentSession, subscribeAccountSessionChanges } from './features/accounts/accountStore';
import { setPresetStorageScope } from './presetStore';
import { customModelToModelInfo, loadCustomModels } from './features/customModels/customModelStore';
import { findModelInfoByName, isCollectionFullyLoaded, isCollectionModel, withVirtualLoadedCollections } from './features/collections/collectionModels';
import ChatView from './components/ChatView';
import ModelManager from './components/ModelManager';
import ConnectView from './components/ConnectView';
import PresetManager from './components/PresetManager';
import BackendManager from './components/BackendManager';
import Dashboard from './components/Dashboard';
import LogViewer from './components/LogViewer';
import DownloadManager from './components/DownloadManager';
import { Icon } from './components/Icon';
import { DownloadListItem, downloadStore, isDownloadActive } from './features/downloadManager/downloadStore';

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

const VALID_VIEWS: View[] = ['chat', 'models', 'presets', 'backends', 'dashboard', 'logs', 'connect'];


type HostNavigationPayload = string | URL | {
  url?: string;
  href?: string;
  view?: string;
  model?: string;
  [key: string]: unknown;
};

type HostNavigateUnsubscribe = void | (() => void);

type LemonadeHostApi = {
  onNavigate?: (callback: (payload: HostNavigationPayload) => void) => HostNavigateUnsubscribe;
  signalReady?: () => void;
};

declare global {
  interface Window { api?: LemonadeHostApi & Record<string, unknown>; }
}

function viewFromValue(raw: unknown): View | null {
  const value = String(raw || '').trim().replace(/^\//, '').toLowerCase();
  return VALID_VIEWS.includes(value as View) ? value as View : null;
}

function viewFromHashValue(hash: string): View | null {
  try {
    const clean = hash.replace(/^#\/?/, '');
    if (!clean) return null;
    const params = new URLSearchParams(clean.includes('?') ? clean.slice(clean.indexOf('?') + 1) : clean);
    return viewFromValue(params.get('view')) || viewFromValue(clean.split('?')[0]);
  } catch { return null; }
}

function parseUrlLikeNavigation(raw: string): { view: View | null; model: string | null } {
  const text = raw.trim();
  if (!text) return { view: null, model: null };
  try {
    const url = new URL(text, window.location.origin);
    const hashView = viewFromHashValue(url.hash || '');
    return {
      view: viewFromValue(url.searchParams.get('view')) || hashView || viewFromValue(url.hostname) || viewFromValue(url.pathname),
      model: url.searchParams.get('model') || null,
    };
  } catch {
    const search = text.includes('?') ? text.slice(text.indexOf('?') + 1) : text;
    const params = new URLSearchParams(search.replace(/^#\/?/, ''));
    return { view: viewFromValue(params.get('view')) || viewFromHashValue(text), model: params.get('model') || null };
  }
}

function parseHostNavigation(payload: HostNavigationPayload): { view: View | null; model: string | null } {
  if (typeof payload === 'string') return parseUrlLikeNavigation(payload);
  if (payload instanceof URL) return parseUrlLikeNavigation(payload.href);
  if (payload && typeof payload === 'object') {
    const obj = payload as Record<string, unknown>;
    const directView = viewFromValue(obj.view);
    const directModel = typeof obj.model === 'string' ? obj.model : null;
    const urlLike = typeof obj.url === 'string' ? obj.url : (typeof obj.href === 'string' ? obj.href : '');
    const parsed = urlLike ? parseUrlLikeNavigation(urlLike) : { view: null, model: null };
    return { view: directView || parsed.view, model: directModel || parsed.model };
  }
  return { view: null, model: null };
}

function viewFromCurrentLocation(): View | null {
  try {
    const params = new URLSearchParams(window.location.search);
    return viewFromValue(params.get('view')) || viewFromHashValue(window.location.hash || '');
  } catch { return viewFromHashValue(window.location.hash || ''); }
}

function viewFromHash(): View | null {
  return viewFromHashValue(window.location.hash || '');
}

function loadSavedView(): View {
  // Deep-link/search/hash takes priority, then localStorage, then default
  const fromLocation = viewFromCurrentLocation();
  if (fromLocation) return fromLocation;
  try {
    const saved = localStorage.getItem('lemonade_current_view');
    if (saved && VALID_VIEWS.includes(saved as View)) return saved as View;
  } catch { /* ignore */ }
  return 'chat';
}

type Theme = 'dark' | 'light';
const THEME_KEY = 'lemonade_theme';

function loadTheme(): Theme {
  try {
    const saved = localStorage.getItem(THEME_KEY);
    if (saved === 'light' || saved === 'dark') return saved;
  } catch { /* ignore */ }
  return 'dark';
}

const App: React.FC = () => {
  const [view, setViewState] = useState<View>(loadSavedView);
  const [status, setStatus] = useState<ConnectionStatus>(api.status);
  const [currentModel, setCurrentModel] = useState<string | null>(null);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);
  const [theme, setTheme] = useState<Theme>(loadTheme);
  const [accountSession, setAccountSession] = useState<AccountSession>(() => {
    const session = currentSession();
    setPresetStorageScope(session.storageScope);
    return session;
  });
  const [accountResetNonce, setAccountResetNonce] = useState(0);
  const accountSessionRef = useRef(accountSession);
  const [downloadManagerOpen, setDownloadManagerOpen] = useState(false);
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  useEffect(() => {
    accountSessionRef.current = accountSession;
    setPresetStorageScope(accountSession.storageScope);
  }, [accountSession]);

  useEffect(() => downloadStore.subscribe(setDownloadItems), []);

  const activeDownloadCount = useMemo(
    () => downloadItems.filter(isDownloadActive).length,
    [downloadItems],
  );

  const handleAccountSessionChange = useCallback((next: AccountSession) => {
    setPresetStorageScope(next.storageScope);
    setAccountSession(next);
  }, []);

  const handleAccountDataReset = useCallback(() => {
    setAccountResetNonce(n => n + 1);
  }, []);

  useEffect(() => subscribeAccountSessionChanges((next) => {
    const prev = accountSessionRef.current;
    const changed = prev.id !== next.id || prev.name !== next.name || prev.role !== next.role || prev.storageScope !== next.storageScope;
    if (!changed) return;
    accountSessionRef.current = next;
    setAccountSession(next);
    setAccountResetNonce(n => n + 1);
  }), []);

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem(THEME_KEY, theme); } catch { /* ignore */ }
  }, [theme]);

  const toggleTheme = useCallback(() => {
    setTheme(t => t === 'dark' ? 'light' : 'dark');
  }, []);

  const applyLoadedModels = useCallback((loaded: LoadedModel[]) => {
    const customInfos = loadCustomModels(accountSession.storageScope).map(customModelToModelInfo);
    const knownInfos = [...customInfos, ...api.allModels];
    const enriched = withVirtualLoadedCollections(loaded, knownInfos).map(model => {
      const info = findModelInfoByName(knownInfos, model.model_name);
      if (!info) return model;
      const cap = capabilityFromModelInfo(info);
      return {
        ...model,
        type: cap === 'unknown' ? model.type : cap,
        recipe: model.recipe || String((info as any).recipe || ''),
        checkpoint: model.checkpoint || String((info as any).checkpoint || ''),
      };
    });
    const customSelectable = (name: string) => {
      const info = findModelInfoByName(customInfos, name);
      if (!info) return false;
      const cap = capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'tts';
    };
    const infoSelectable = (name: string) => {
      const info = findModelInfoByName(knownInfos, name);
      if (!info) return false;
      const cap = capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'tts';
    };
    setLoadedModels(enriched);
    setCurrentModel(current => {
      if (current && enriched.some(m => m.model_name === current && (canSelectInComposer(m) || customSelectable(m.model_name) || infoSelectable(m.model_name)))) return current;
      if (current) {
        const info = findModelInfoByName(knownInfos, current);
        if (info && isCollectionModel(info) && isCollectionFullyLoaded(info, loaded)) return current;
      }
      const virtualOmni = enriched.find(model => {
        const info = findModelInfoByName(knownInfos, model.model_name);
        return info && isCollectionModel(info);
      });
      return virtualOmni?.model_name
        || selectPreferredLoadedModel(enriched)?.model_name
        || enriched.find(m => customSelectable(m.model_name) || infoSelectable(m.model_name))?.model_name
        || null;
    });
  }, [accountSession.storageScope]);

  const setView = useCallback((v: View) => {
    setViewState(v);
    try { localStorage.setItem('lemonade_current_view', v); } catch { /* ignore */ }
    // Update hash without triggering hashchange (we're already setting state)
    const newHash = `#/${v}`;
    if (window.location.hash !== newHash) {
      window.history.pushState(null, '', newHash);
    }
  }, []);

  // Electron/host deep-links: GUI2-compatible lemonade://?view=logs&model=... navigation.
  useEffect(() => {
    let cancelled = false;
    let cleanup: HostNavigateUnsubscribe;
    let attempts = 0;

    const applyNavigation = (payload: HostNavigationPayload) => {
      const target = parseHostNavigation(payload);
      if (target.model) setCurrentModel(target.model);
      if (target.view) setView(target.view);
    };

    const attach = () => {
      if (cancelled) return;
      const hostApi = window.api;
      if (hostApi?.onNavigate || hostApi?.signalReady) {
        if (hostApi.onNavigate) cleanup = hostApi.onNavigate(applyNavigation);
        try { hostApi.signalReady?.(); } catch (err) { console.warn('Host signalReady failed:', err); }
        return;
      }
      attempts += 1;
      if (attempts < 50) window.setTimeout(attach, 100);
    };

    const initialView = viewFromCurrentLocation();
    if (initialView) setView(initialView);
    attach();

    return () => {
      cancelled = true;
      if (typeof cleanup === 'function') cleanup();
    };
  }, [setView]);

  // Sync view from hash on back/forward navigation
  useEffect(() => {
    const onHashChange = () => {
      const v = viewFromHash();
      if (v) setViewState(v);
    };
    window.addEventListener('hashchange', onHashChange);
    // Set initial hash if not already set
    if (!window.location.hash) {
      window.history.replaceState(null, '', `#/${view}`);
    }
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  useEffect(() => {
    const unsubStatus = api.onStatusChange(setStatus);
    const refreshGlobalModels = () => {
      const loaded = api.loadedModels;
      applyLoadedModels(loaded);
    };
    const unsubModels = api.onModelsChanged(async () => {
      const result = await api.refresh().catch(() => null);
      if (result) applyLoadedModels(result.health.all_models_loaded);
      else refreshGlobalModels();
    });
    api.connect().then(async connected => {
      if (!connected) return;
      const result = await api.refresh().catch(() => null);
      if (result) {
        applyLoadedModels(result.health.all_models_loaded);
      } else {
        refreshGlobalModels();
      }
    });
    return () => { unsubStatus(); unsubModels(); };
  }, [applyLoadedModels]);

  // App-level health polling: skip when Dashboard is active (it polls every 2s)
  useEffect(() => {
    if (view === 'dashboard') {
      api.stopPolling();
    } else {
      api.startPolling(15000);
    }
    return () => { api.stopPolling(); };
  }, [view]);

  const handleRefreshModels = useCallback(async () => {
    const result = await api.refresh();
    if (result) applyLoadedModels(result.health.all_models_loaded);
  }, [applyLoadedModels]);

  const handleModelSelect = useCallback((modelName: string) => {
    setCurrentModel(modelName);
    setView('chat');
  }, [setView]);

  return (
    <>
      <a href="#main-content" className="skip-link">Skip to main content</a>
      <div className="app">
        <header className="titlebar">
        <div className="titlebar__brand">
          <svg className="titlebar__lemon" width="18" height="18" viewBox="0 0 32 32" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
            <path d="M7.036 2.492L2 5.01c.826 2.337 3.525 3.525 6.043 2.518l6.044-2.518C13.26 2.663 9.85 1.177 7.036 2.492Z" fill="url(#ll0)"/>
            <path d="M14.924 4.6C9.52 6.507 6.69 12.45 8.592 17.87l1.252 3.558c1.403 3.989 4.987 6.583 8.93 6.908a3.07 3.07 0 0 1 1.994.884 2.56 2.56 0 0 0 3.027.605c1.078-.384 1.809-1.314 1.983-2.372a3.9 3.9 0 0 1 .997-1.942c2.864-2.745 4.035-7.013 2.632-11.002l-1.252-3.559C26.253 5.518 20.327 2.681 14.924 4.6Z" fill="url(#ll1)"/>
            <path d="M14.924 4.6C9.52 6.507 6.69 12.45 8.592 17.87l1.252 3.558c1.403 3.989 4.987 6.583 8.93 6.908a3.07 3.07 0 0 1 1.994.884 2.56 2.56 0 0 0 3.027.605c1.078-.384 1.809-1.314 1.983-2.372a3.9 3.9 0 0 1 .997-1.942c2.864-2.745 4.035-7.013 2.632-11.002l-1.252-3.559C26.253 5.518 20.327 2.681 14.924 4.6Z" fill="url(#ll2)"/>
            <defs>
              <linearGradient id="ll0" x1="2" y1="5.009" x2="14.087" y2="5.009" gradientUnits="userSpaceOnUse">
                <stop stopColor="#80A338"/><stop offset="1" stopColor="#B3D745"/>
              </linearGradient>
              <radialGradient id="ll1" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(21.2 10.57) rotate(115.15) scale(17.63 14.92)">
                <stop stopColor="#FFFB98"/><stop offset=".505" stopColor="#FFD84C"/><stop offset="1" stopColor="#E6B534"/>
              </radialGradient>
              <radialGradient id="ll2" cx="0" cy="0" r="1" gradientUnits="userSpaceOnUse" gradientTransform="translate(14.62 4.97) rotate(69.33) scale(26.75 22.64)">
                <stop offset=".52" stopColor="#FFDE67" stopOpacity="0"/><stop offset=".74" stopColor="#FFA457" stopOpacity=".2"/><stop offset=".89" stopColor="#D5676D" stopOpacity=".75"/><stop offset=".92" stopColor="#E88257"/><stop offset="1" stopColor="#F49754"/>
              </radialGradient>
            </defs>
          </svg>
          <span>lemonade</span>
          <span className={`titlebar__status-dot titlebar__status-dot--brand ${
            status === 'connected' ? 'titlebar__status-dot--connected' :
            status === 'connecting' ? 'titlebar__status-dot--connecting' : ''
          }`}
            role="status"
            aria-label={status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting…' : 'Offline'}
            title={status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting…' : 'Offline'}
          />
        </div>

        <nav className="titlebar__nav" aria-label="Primary">
          {([
            { id: 'chat',      label: 'Chat',      icon: 'chat'               },
            { id: 'models',    label: 'Models',    icon: 'hard-drive'         },
            { id: 'presets',   label: 'Presets',   icon: 'sliders-horizontal' },
            { id: 'backends',  label: 'Backends',  icon: 'box'                },
            { id: 'dashboard', label: 'Dashboard',  icon: 'gauge'              },
            { id: 'logs',      label: 'Logs',      icon: 'logs'               },
            { id: 'connect',   label: 'Connect',   icon: 'plug'               },
          ] as { id: View; label: string; icon: Parameters<typeof Icon>[0]['name'] }[]).map(({ id, label, icon }) => (
            <button
              key={id}
              className={view === id ? 'is-active' : ''}
              onClick={() => setView(id)}
              title={label}
              aria-label={label}
            >
              <Icon name={icon} size={14} aria-hidden="true" />
              <span className="nav-label">{label}</span>
            </button>
          ))}
        </nav>

        <div className="titlebar__right">
          <AccountMenu
            session={accountSession}
            onSessionChange={handleAccountSessionChange}
            onDataReset={handleAccountDataReset}
          />
          <button className="titlebar__theme-toggle" onClick={toggleTheme} aria-label="Toggle theme" title={theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'}>
            <Icon name={theme === 'dark' ? 'sun' : 'moon'} size={16} />
          </button>
          <button
            className={`titlebar__download-toggle${downloadManagerOpen ? ' is-active' : ''}${activeDownloadCount > 0 ? ' has-active-downloads' : ''}`}
            onClick={() => setDownloadManagerOpen(open => !open)}
            aria-label="Open download manager"
            aria-expanded={downloadManagerOpen}
            title="Download manager"
          >
            <Icon name="download" size={16} />
            {activeDownloadCount > 0 && <span className="titlebar__download-badge">{activeDownloadCount > 9 ? '9+' : activeDownloadCount}</span>}
          </button>
        </div>
      </header>

      <DownloadManager isVisible={downloadManagerOpen} onClose={() => setDownloadManagerOpen(false)} />

      <main id="main-content" tabIndex={-1} className="view-container">
        <div style={{ display: view === 'chat' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="chat">
            <ChatView
              key={`${accountSession.storageScope}:${accountResetNonce}`}
              currentModel={currentModel}
              loadedModels={loadedModels}
              accountSession={accountSession}
              onModelSelect={handleModelSelect}
              onRefresh={handleRefreshModels}
            />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'models' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="models">
            <ModelManager
              onModelSelect={handleModelSelect}
              selectedModel={currentModel}
              accountSession={accountSession}
            />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'presets' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="presets">
            <PresetManager key={accountSession.storageScope} loadedModels={loadedModels} />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'backends' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="backends">
            <BackendManager />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'dashboard' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="dashboard">
            <Dashboard />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'logs' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="logs">
            <LogViewer />
          </ViewErrorBoundary>
        </div>
        <div style={{ display: view === 'connect' ? 'contents' : 'none' }}>
          <ViewErrorBoundary view="connect">
            <ConnectView
              status={status}
              accountSession={accountSession}
              onLocalDataReset={handleAccountDataReset}
              onSessionChange={handleAccountSessionChange}
            />
          </ViewErrorBoundary>
        </div>
        </main>
      </div>
    </>
  );
};

export default App;
