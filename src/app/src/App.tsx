import React, { useState, useEffect, useCallback, useMemo, useRef, Component, ErrorInfo, ReactNode } from 'react';
import api, { friendlyErrorMessage, LoadedModel, ModelInfo } from './api';
import { canSelectInComposer, capabilityFromModelInfo, selectPreferredLoadedModel } from './modelCapabilities';
import { customModelToModelInfo, loadCustomModels } from './features/customModels/customModelStore';
import { findModelInfoByName, isCollectionFullyLoaded, isCollectionModel, withVirtualLoadedCollections } from './features/collections/collectionModels';
import ChatView from './components/ChatView';
import ModelManager from './components/ModelManager';
import ConnectView from './components/ConnectView';
import AppsView, { MARKETPLACE_URL, type MarketplaceApp, type MarketplaceCategory } from './components/AppsView';
import BackendManager from './components/BackendManager';
import DownloadManager from './components/DownloadManager';
import MonitorView from './components/MonitorView';
import { Icon } from './components/Icon';
import { WorkspaceActionButton } from './components/WorkspacePanels';
import { downloadStore, isDownloadActive } from './features/downloadManager/downloadStore';
import { useServerModelState } from './features/models/modelState';
import {
  WORKSPACE_NAVIGATION,
  type ConnectSection,
  type DashboardSection,
  type WorkspaceRoute,
  workspaceHash,
  workspaceRouteFromPath,
} from './features/navigation/workspaceNavigation';

type View = 'chat' | 'models' | 'backends' | 'apps' | 'dashboard' | 'connect';
type SimpleView = Exclude<View, 'dashboard' | 'connect'>;
type AppRoute =
  | { view: SimpleView }
  | { view: 'dashboard'; section: DashboardSection }
  | { view: 'connect'; section: ConnectSection };

const NAVIGATION_DESTINATIONS: Array<{
  id: View;
  label: string;
  keywords: string;
  icon: Parameters<typeof Icon>[0]['name'];
}> = [
  { id: 'chat', label: 'Chat', keywords: 'conversation messages', icon: 'chat' },
  { id: 'models', label: 'Models', keywords: 'model manager download load', icon: 'hard-drive' },
  { id: 'backends', label: 'Backends', keywords: 'runtime inference engine', icon: 'box' },
  { id: 'apps', label: 'Apps', keywords: 'clients integrations', icon: 'layers' },
  { id: 'dashboard', label: 'Monitor', keywords: 'dashboard monitor system hardware statistics', icon: 'gauge' },
  { id: 'connect', label: 'Settings', keywords: 'connect configuration preferences server', icon: 'settings' },
];

const BACKEND_DESTINATIONS = [
  'llama.cpp',
  'FastFlowLM',
  'RyzenAI',
  'vLLM',
  'whisper.cpp',
  'stable-diffusion.cpp',
  'Moonshine',
  'Kokoro TTS',
];

type GlobalSearchResult = {
  id: string;
  label: string;
  description: string;
  icon: Parameters<typeof Icon>[0]['name'];
  view?: View;
  route?: AppRoute;
  modelName?: string;
};

function modelSearchName(model: Record<string, unknown>): string {
  return String(model.model_name ?? model.name ?? model.id ?? '').trim();
}

function searchKey(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, '');
}

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
        <div className="view-error">
          <h2>
            Something went wrong in "{this.props.view}"
          </h2>
          <pre>
            {this.state.error.message}
          </pre>
          <WorkspaceActionButton
            appearance="primary"
            onClick={() => this.setState({ error: null })}
          >
            Try again
          </WorkspaceActionButton>
        </div>
      );
    }
    return this.props.children;
  }
}

const SIMPLE_VIEWS: SimpleView[] = ['chat', 'models', 'backends', 'apps'];


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
  minimizeWindow?: () => void;
  maximizeWindow?: () => void;
  closeWindow?: () => void;
  isWebApp?: boolean;
};

declare global {
  interface Window { api?: LemonadeHostApi & Record<string, unknown>; }
}

function routeFromValue(raw: unknown): AppRoute | null {
  const value = String(raw || '').trim().replace(/^\//, '').toLowerCase();
  if (value === 'connect/app-directory') return { view: 'apps' };
  const workspaceRoute = workspaceRouteFromPath(value);
  if (workspaceRoute) return { view: workspaceRoute.workspace, section: workspaceRoute.section } as AppRoute;
  return SIMPLE_VIEWS.includes(value as SimpleView) ? { view: value as SimpleView } : null;
}

function hashForRoute(route: AppRoute): string {
  if (route.view === 'dashboard' || route.view === 'connect') {
    return workspaceHash({ workspace: route.view, section: route.section } as WorkspaceRoute);
  }
  return `#/${route.view}`;
}

function routeFromHashValue(hash: string): AppRoute | null {
  try {
    const clean = hash.replace(/^#\/?/, '');
    if (!clean) return null;
    const params = new URLSearchParams(clean.includes('?') ? clean.slice(clean.indexOf('?') + 1) : clean);
    return routeFromValue(params.get('view')) || routeFromValue(clean.split('?')[0]);
  } catch { return null; }
}

function parseUrlLikeNavigation(raw: string): { route: AppRoute | null; model: string | null } {
  const text = raw.trim();
  if (!text) return { route: null, model: null };
  try {
    const url = new URL(text, window.location.origin);
    const hashRoute = routeFromHashValue(url.hash || '');
    return {
      route: routeFromValue(url.searchParams.get('view')) || hashRoute || routeFromValue(url.hostname) || routeFromValue(url.pathname),
      model: url.searchParams.get('model') || null,
    };
  } catch {
    const search = text.includes('?') ? text.slice(text.indexOf('?') + 1) : text;
    const params = new URLSearchParams(search.replace(/^#\/?/, ''));
    return { route: routeFromValue(params.get('view')) || routeFromHashValue(text), model: params.get('model') || null };
  }
}

function parseHostNavigation(payload: HostNavigationPayload): { route: AppRoute | null; model: string | null } {
  if (typeof payload === 'string') return parseUrlLikeNavigation(payload);
  if (payload instanceof URL) return parseUrlLikeNavigation(payload.href);
  if (payload && typeof payload === 'object') {
    const obj = payload as Record<string, unknown>;
    const directRoute = routeFromValue(obj.view);
    const directModel = typeof obj.model === 'string' ? obj.model : null;
    const urlLike = typeof obj.url === 'string' ? obj.url : (typeof obj.href === 'string' ? obj.href : '');
    const parsed = urlLike ? parseUrlLikeNavigation(urlLike) : { route: null, model: null };
    return { route: directRoute || parsed.route, model: directModel || parsed.model };
  }
  return { route: null, model: null };
}

function routeFromCurrentLocation(): AppRoute | null {
  try {
    const params = new URLSearchParams(window.location.search);
    return routeFromValue(params.get('view')) || routeFromHashValue(window.location.hash || '');
  } catch { return routeFromHashValue(window.location.hash || ''); }
}

function routeFromHash(): AppRoute | null {
  return routeFromHashValue(window.location.hash || '');
}

function isLegacyAppDirectoryHash(hash: string): boolean {
  return /^#\/?connect\/app-directory(?:[/?]|$)/i.test(hash);
}

function canonicalizeLegacyHash(route: AppRoute | null): void {
  if (!route || !isLegacyAppDirectoryHash(window.location.hash || '')) return;
  const canonicalHash = hashForRoute(route);
  if (window.location.hash !== canonicalHash) {
    window.history.replaceState(null, '', canonicalHash);
  }
}

function loadSavedRoute(): AppRoute {
  const fromLocation = routeFromCurrentLocation();
  if (fromLocation) {
    canonicalizeLegacyHash(fromLocation);
    return fromLocation;
  }
  try {
    const saved = localStorage.getItem('lemonade_current_view');
    const savedRoute = routeFromValue(saved);
    if (savedRoute) return savedRoute;
  } catch { /* ignore */ }
  return { view: 'chat' };
}

type Theme = 'dark' | 'light';
const THEME_KEY = 'lemonade_theme';
const EMPTY_MODELS: ModelInfo[] = [];
const EMPTY_LOADED_MODELS: LoadedModel[] = [];

function loadTheme(): Theme {
  try {
    const saved = localStorage.getItem(THEME_KEY);
    if (saved === 'light' || saved === 'dark') return saved;
  } catch { /* ignore */ }
  return 'dark';
}

const App: React.FC = () => {
  const [route, setRouteState] = useState<AppRoute>(loadSavedRoute);
  const view = route.view;
  const routeRef = useRef(route);
  routeRef.current = route;
  const serverModelState = useServerModelState();
  const status = serverModelState.status;
  const serverModels = serverModelState.models?.data ?? EMPTY_MODELS;
  const rawLoadedModels = serverModelState.health?.all_models_loaded ?? EMPTY_LOADED_MODELS;
  const [currentModel, setCurrentModel] = useState<string | null>(null);
  const [theme, setTheme] = useState<Theme>(loadTheme);
  const [clientDataResetNonce, setClientDataResetNonce] = useState(0);
  const [downloadManagerOpen, setDownloadManagerOpen] = useState(false);
  const [modelDetailsRequest, setModelDetailsRequest] = useState<{ modelName: string; nonce: number } | null>(null);
  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const [marketplaceLoading, setMarketplaceLoading] = useState(true);
  const [utilityMenuOpen, setUtilityMenuOpen] = useState(false);
  const [navigationSearch, setNavigationSearch] = useState('');
  const [navigationSearchOpen, setNavigationSearchOpen] = useState(false);
  const [navigationSearchIndex, setNavigationSearchIndex] = useState(0);
  const utilityMenuRef = useRef<HTMLDivElement>(null);
  const utilityMenuTriggerRef = useRef<HTMLButtonElement>(null);
  const navigationSearchRef = useRef<HTMLInputElement>(null);
  const [isDesktop, setIsDesktop] = useState(false);
  const navigationSearchShortcut = /Mac|iPhone|iPad|iPod/.test(navigator.userAgent) ? '⌘ K' : 'Ctrl K';

  useEffect(() => {
    let cancelled = false;
    setMarketplaceLoading(true);
    fetch(MARKETPLACE_URL)
      .then(response => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(data => {
        if (cancelled) return;
        setMarketplaceApps(Array.isArray(data?.apps) ? data.apps as MarketplaceApp[] : []);
        setMarketplaceCategories(Array.isArray(data?.categories) ? data.categories as MarketplaceCategory[] : []);
        setMarketplaceError(null);
      })
      .catch(error => {
        if (!cancelled) setMarketplaceError(friendlyErrorMessage(error));
      })
      .finally(() => {
        if (!cancelled) setMarketplaceLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    import('./tauriShim').then(({ tauriReady }) => {
      tauriReady.then(() => {
        if (window.api && window.api.isWebApp !== true) {
          setIsDesktop(true);
        }
      });
    });
  }, []);
  const [activeDownloadCount, setActiveDownloadCount] = useState(
    () => downloadStore.snapshot().filter(isDownloadActive).length,
  );
  const activeDownloadCountRef = useRef(activeDownloadCount);
  const lastWorkspaceSectionsRef = useRef({
    dashboard: route.view === 'dashboard' ? route.section : WORKSPACE_NAVIGATION.dashboard.defaultSection,
    connect: route.view === 'connect' ? route.section : WORKSPACE_NAVIGATION.connect.defaultSection,
  });
  useEffect(() => downloadStore.subscribe(items => {
    const nextCount = items.filter(isDownloadActive).length;
    if (nextCount === activeDownloadCountRef.current) return;
    activeDownloadCountRef.current = nextCount;
    setActiveDownloadCount(nextCount);
  }), []);

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem(THEME_KEY, theme); } catch { /* ignore */ }
  }, [theme]);

  const toggleTheme = useCallback(() => {
    setTheme(t => t === 'dark' ? 'light' : 'dark');
  }, []);

  const handleLocalDataReset = useCallback(() => {
    setClientDataResetNonce(n => n + 1);
  }, []);

  useEffect(() => {
    if (!utilityMenuOpen) return;
    const onPointerDown = (event: PointerEvent) => {
      if (!utilityMenuRef.current?.contains(event.target as Node)) setUtilityMenuOpen(false);
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      setUtilityMenuOpen(false);
      requestAnimationFrame(() => utilityMenuTriggerRef.current?.focus());
    };
    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [utilityMenuOpen]);

  useEffect(() => {
    setUtilityMenuOpen(false);
  }, [view]);

  const loadedModelViewState = useMemo(() => {
    const customInfos = loadCustomModels().map(customModelToModelInfo);
    const knownInfos = [...customInfos, ...serverModels];
    const models = withVirtualLoadedCollections(rawLoadedModels, knownInfos).map(model => {
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
    return { models, customInfos, knownInfos };
  }, [clientDataResetNonce, rawLoadedModels, serverModels]);

  const loadedModels = loadedModelViewState.models;

  useEffect(() => {
    const { customInfos, knownInfos } = loadedModelViewState;
    const customSelectable = (name: string) => {
      const info = findModelInfoByName(customInfos, name);
      if (!info) return false;
      const cap = capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'audio-generation' || cap === 'tts' || cap === 'model3d';
    };
    const infoSelectable = (name: string) => {
      const info = findModelInfoByName(knownInfos, name);
      if (!info) return false;
      const cap = capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'audio-generation' || cap === 'tts' || cap === 'model3d';
    };
    setCurrentModel(current => {
      if (current && loadedModels.some(m => m.model_name === current && (canSelectInComposer(m) || customSelectable(m.model_name) || infoSelectable(m.model_name)))) return current;
      if (current) {
        const info = findModelInfoByName(knownInfos, current);
        if (info && isCollectionModel(info) && isCollectionFullyLoaded(info, rawLoadedModels)) return current;
      }
      const virtualOmni = loadedModels.find(model => {
        const info = findModelInfoByName(knownInfos, model.model_name);
        return info && isCollectionModel(info);
      });
      return virtualOmni?.model_name
        || selectPreferredLoadedModel(loadedModels)?.model_name
        || loadedModels.find(m => customSelectable(m.model_name) || infoSelectable(m.model_name))?.model_name
        || null;
    });
  }, [loadedModelViewState, loadedModels, rawLoadedModels]);

  const navigateToRoute = useCallback((nextRoute: AppRoute) => {
    if (nextRoute.view === 'dashboard') lastWorkspaceSectionsRef.current.dashboard = nextRoute.section;
    if (nextRoute.view === 'connect') lastWorkspaceSectionsRef.current.connect = nextRoute.section;
    setRouteState(nextRoute);
    const newHash = hashForRoute(nextRoute);
    try { localStorage.setItem('lemonade_current_view', newHash.slice(2)); } catch { /* ignore */ }
    if (window.location.hash !== newHash) {
      window.history.pushState(null, '', newHash);
    }
  }, []);

  const setView = useCallback((nextView: View) => {
    if (nextView === 'dashboard') {
      navigateToRoute({ view: nextView, section: lastWorkspaceSectionsRef.current.dashboard });
      return;
    }
    if (nextView === 'connect') {
      navigateToRoute({ view: nextView, section: lastWorkspaceSectionsRef.current.connect });
      return;
    }
    navigateToRoute({ view: nextView });
  }, [navigateToRoute]);

  const searchableServerModels = useMemo(() => {
    const loadedNames = new Set(
      rawLoadedModels.map(model => model.model_name.toLowerCase()),
    );
    return serverModels.filter(model => {
      const name = modelSearchName(model as unknown as Record<string, unknown>).toLowerCase();
      return (model as any).suggested !== false
        || Boolean((model as any).downloaded)
        || loadedNames.has(name);
    });
  }, [rawLoadedModels, serverModels]);

  const navigationSearchResults = useMemo<GlobalSearchResult[]>(() => {
    const query = navigationSearch.trim().toLowerCase();
    const normalizedQuery = searchKey(query);
    const matches = (value: string) =>
      !query || value.toLowerCase().includes(query) || searchKey(value).includes(normalizedQuery);
    if (!query) return [];

    const pages = NAVIGATION_DESTINATIONS
      .filter(destination => matches(`${destination.label} ${destination.keywords}`))
      .map(destination => ({
        id: `page:${destination.id}`,
        label: destination.label,
        description: 'Page',
        icon: destination.icon,
        view: destination.id,
      }));

    const monitorDefinition = WORKSPACE_NAVIGATION.dashboard;
    const monitor = monitorDefinition.sections
      .filter(section => matches(`${section.label} ${section.description}`))
      .map(section => ({
        id: `workspace:dashboard:${section.id}`,
        label: section.label,
        description: `${monitorDefinition.label} - ${section.description}`,
        icon: section.icon,
        route: { view: 'dashboard', section: section.id } as AppRoute,
      }));
    const settingsDefinition = WORKSPACE_NAVIGATION.connect;
    const settings = settingsDefinition.sections
      .filter(section => matches(`${section.label} ${section.description}`))
      .map(section => ({
        id: `workspace:connect:${section.id}`,
        label: section.label,
        description: `${settingsDefinition.label} - ${section.description}`,
        icon: section.icon,
        route: { view: 'connect', section: section.id } as AppRoute,
      }));
    const models = searchableServerModels
      .map(model => {
        const name = modelSearchName(model as unknown as Record<string, unknown>);
        return { model, name };
      })
      .filter(({ name }) => name && matches(name))
      .slice(0, 8)
      .map(({ model, name }) => {
        const type = String((model as any).type ?? '').trim();
        return {
          id: `model:${name}`,
          label: name,
          description: type && type.toLowerCase() !== 'model' ? `${type} model` : 'Model',
          icon: 'hard-drive' as Parameters<typeof Icon>[0]['name'],
          modelName: name,
        };
      });
    const backends = BACKEND_DESTINATIONS
      .filter(backend => matches(backend))
      .map(backend => ({
        id: `backend:${backend}`,
        label: backend,
        description: 'Backend',
        icon: 'box' as Parameters<typeof Icon>[0]['name'],
        view: 'backends' as View,
      }));
    const apps = marketplaceApps
      .filter(marketplaceApp => matches(`${marketplaceApp.name} ${marketplaceApp.description || ''} ${(marketplaceApp.category || []).join(' ')}`))
      .slice(0, 8)
      .map(marketplaceApp => ({
        id: `app:${marketplaceApp.id || marketplaceApp.name}`,
        label: marketplaceApp.name,
        description: marketplaceApp.category?.length ? `App - ${marketplaceApp.category.join(', ')}` : 'App',
        icon: 'layers' as Parameters<typeof Icon>[0]['name'],
        view: 'apps' as View,
      }));

    type SearchGroup = 'models' | 'backends' | 'apps' | 'settings' | 'monitor' | 'pages';
    const groups: Record<SearchGroup, GlobalSearchResult[]> = {
      models,
      backends,
      apps,
      settings,
      monitor,
      pages,
    };
    const defaultOrder: SearchGroup[] = ['pages', 'models', 'backends', 'apps', 'settings', 'monitor'];
    const preferredGroup: SearchGroup = view === 'apps'
      ? 'apps'
      : view === 'backends'
        ? 'backends'
        : view === 'connect'
          ? 'settings'
          : 'models';
    const groupOrder = [preferredGroup, ...defaultOrder.filter(group => group !== preferredGroup)];
    return groupOrder.flatMap(group => groups[group]);
  }, [marketplaceApps, navigationSearch, searchableServerModels, view]);

  const selectNavigationDestination = useCallback((destination: GlobalSearchResult) => {
    if (destination.modelName) {
      setModelDetailsRequest({ modelName: destination.modelName, nonce: Date.now() });
      setView('models');
    } else if (destination.route) {
      navigateToRoute(destination.route);
    } else if (destination.view) {
      setView(destination.view);
    }
    setNavigationSearch('');
    setNavigationSearchOpen(false);
    setUtilityMenuOpen(false);
  }, [navigateToRoute, setView]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault();
        if (window.matchMedia('(max-width: 768px)').matches) setUtilityMenuOpen(true);
        setNavigationSearchOpen(true);
        requestAnimationFrame(() => navigationSearchRef.current?.focus());
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);

  const openModelDetails = useCallback((modelName: string) => {
    setModelDetailsRequest({ modelName, nonce: Date.now() });
    setView('models');
  }, [setView]);

  // Desktop host deep-links use the same canonical routes as browser navigation.
  useEffect(() => {
    let cancelled = false;
    let cleanup: HostNavigateUnsubscribe;
    let attempts = 0;

    const applyNavigation = (payload: HostNavigationPayload) => {
      const target = parseHostNavigation(payload);
      if (target.model) setCurrentModel(target.model);
      if (target.route) navigateToRoute(target.route);
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

    const initialRoute = routeFromCurrentLocation();
    if (initialRoute) navigateToRoute(initialRoute);
    attach();

    return () => {
      cancelled = true;
      if (typeof cleanup === 'function') cleanup();
    };
  }, [navigateToRoute]);

  // Sync route from hash on back/forward navigation
  useEffect(() => {
    const onHashChange = () => {
      const nextRoute = routeFromHash();
      if (nextRoute) {
        canonicalizeLegacyHash(nextRoute);
        if (nextRoute.view === 'dashboard') lastWorkspaceSectionsRef.current.dashboard = nextRoute.section;
        if (nextRoute.view === 'connect') lastWorkspaceSectionsRef.current.connect = nextRoute.section;
        setRouteState(nextRoute);
        try { localStorage.setItem('lemonade_current_view', hashForRoute(nextRoute).slice(2)); } catch { /* ignore */ }
      } else {
        window.history.replaceState(null, '', hashForRoute(routeRef.current));
      }
    };
    window.addEventListener('hashchange', onHashChange);
    if (!routeFromHash()) {
      window.history.replaceState(null, '', hashForRoute(routeRef.current));
    }
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  // Client-local in-app deep-link: components dispatch
  // `lemonade:navigate` with { view } to switch views without involving lemond.
  useEffect(() => {
    const onAppNavigate = (e: Event) => {
      const detail = (e as CustomEvent).detail as { view?: string } | undefined;
      const target = detail?.view;
      const nextRoute = routeFromValue(target);
      if (nextRoute) navigateToRoute(nextRoute);
    };
    window.addEventListener('lemonade:navigate', onAppNavigate as EventListener);
    return () => window.removeEventListener('lemonade:navigate', onAppNavigate as EventListener);
  }, [navigateToRoute]);

  useEffect(() => {
    void api.connect();
  }, []);

  // App-level health polling: skip when Monitor is active (it polls every 2s)
  useEffect(() => {
    if (view === 'dashboard') {
      api.stopPolling();
    } else {
      api.startPolling(15000);
    }
    return () => { api.stopPolling(); };
  }, [view]);

  const handleRefreshModels = useCallback(async () => {
    await api.refresh();
  }, []);

  const handleModelSelect = useCallback((modelName: string) => {
    setCurrentModel(modelName);
    setView('chat');
  }, [setView]);

  return (
    <>
      <a href="#main-content" className="skip-link">Skip to main content</a>
      <div className="app">
        <header className="titlebar" data-tauri-drag-region>
        <div className="titlebar__brand" data-tauri-drag-region>
          <span className="titlebar__brand-logo" data-tauri-drag-region>
            <span className="titlebar__brand-icon" aria-hidden="true" />
            <span className="titlebar__brand-name">lemonade</span>
          </span>
          <span className={`titlebar__status-dot titlebar__status-dot--brand ${
            status === 'connected' ? 'titlebar__status-dot--connected' :
            status === 'connecting' ? 'titlebar__status-dot--connecting' : ''
          }`}
            role="status"
            aria-label={status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting…' : 'Offline'}
            title={status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting…' : 'Offline'}
          />
        </div>

        <nav className="titlebar__nav" data-tauri-drag-region="false" aria-label="Primary">
          {NAVIGATION_DESTINATIONS.map(({ id, label, icon }) => (
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

        <div className="titlebar__right" data-tauri-drag-region>
          <div
            ref={utilityMenuRef}
            className={`titlebar__utilities${utilityMenuOpen ? ' is-open' : ''}`}
            data-tauri-drag-region="false"
          >
            <button
              ref={utilityMenuTriggerRef}
              type="button"
              className="titlebar__utilities-toggle"
              aria-label="App controls"
              aria-expanded={utilityMenuOpen}
              aria-controls="titlebar-utility-menu"
              title="App controls"
              onClick={() => setUtilityMenuOpen(open => !open)}
            >
              <Icon name="sliders-horizontal" size={17} aria-hidden="true" />
            </button>
            <div id="titlebar-utility-menu" className="titlebar__utility-menu" aria-label="App controls">
              <div
                className={`titlebar__search${navigationSearchOpen ? ' is-open' : ''}${view === 'apps' ? ' is-context-visible' : ''}`}
                onBlur={event => {
                  if (!event.currentTarget.contains(event.relatedTarget as Node | null)) {
                    setNavigationSearchOpen(false);
                  }
                }}
              >
                <Icon name="search" size={15} className="titlebar__search-leading-icon" aria-hidden="true" />
                <button
                  type="button"
                  className="titlebar__search-toggle"
                  aria-label="Search Lemonade"
                  aria-expanded={navigationSearchOpen}
                  aria-controls="titlebar-search-results"
                  title="Search"
                  onClick={() => {
                    setNavigationSearchOpen(true);
                    requestAnimationFrame(() => navigationSearchRef.current?.focus());
                  }}
                >
                  <Icon name="search" size={16} aria-hidden="true" />
                </button>
                <div className="titlebar__search-popover">
                  <div className="titlebar__search-entry">
                    <input
                      ref={navigationSearchRef}
                      type="search"
                      value={navigationSearch}
                      placeholder="Search"
                      aria-label={view === 'apps' ? 'Search apps' : 'Search Lemonade'}
                      aria-autocomplete="list"
                      aria-expanded={navigationSearchOpen}
                      aria-controls="titlebar-search-results"
                      onFocus={() => setNavigationSearchOpen(true)}
                      onChange={event => {
                        setNavigationSearch(event.target.value);
                        setNavigationSearchIndex(0);
                        setNavigationSearchOpen(true);
                      }}
                      onKeyDown={event => {
                        if (event.key === 'ArrowDown') {
                          event.preventDefault();
                          if (navigationSearchResults.length > 0) {
                            setNavigationSearchIndex(index => Math.min(index + 1, navigationSearchResults.length - 1));
                          }
                        } else if (event.key === 'ArrowUp') {
                          event.preventDefault();
                          if (navigationSearchResults.length > 0) {
                            setNavigationSearchIndex(index => Math.max(index - 1, 0));
                          }
                        } else if (event.key === 'Enter' && navigationSearchResults[navigationSearchIndex]) {
                          event.preventDefault();
                          selectNavigationDestination(navigationSearchResults[navigationSearchIndex]);
                        } else if (event.key === 'Escape') {
                          setNavigationSearch('');
                          setNavigationSearchOpen(false);
                        }
                      }}
                    />
                    <kbd
                      aria-label={navigationSearchShortcut === '⌘ K' ? 'Command K' : 'Control K'}
                      title={`Search shortcut: ${navigationSearchShortcut}`}
                    >
                      <Icon name="terminal-square" size={13} aria-hidden="true" />
                    </kbd>
                  </div>
                  {navigationSearchOpen && (
                    <div id="titlebar-search-results" className="titlebar__search-results" role="listbox" aria-label="Global search results">
                      {navigationSearchResults.length > 0 ? navigationSearchResults.map((destination, index) => (
                        <button
                          key={destination.id}
                          type="button"
                          role="option"
                          aria-selected={index === navigationSearchIndex}
                          className={index === navigationSearchIndex ? 'is-active' : ''}
                          onMouseDown={event => event.preventDefault()}
                          onClick={() => selectNavigationDestination(destination)}
                        >
                          <Icon name={destination.icon} size={14} aria-hidden="true" />
                          <span>
                            <strong>{destination.label}</strong>
                            <small>{destination.description}</small>
                          </span>
                        </button>
                      )) : <p>{navigationSearch.trim() ? 'No matching results.' : 'Search models, backends, apps, and settings.'}</p>}
                    </div>
                  )}
                </div>
              </div>
              <div
                className="titlebar__utility-status"
                role="status"
                aria-label={`Server ${status === 'connected' ? 'connected' : status === 'connecting' ? 'connecting' : 'offline'}`}
              >
                <span className={`titlebar__status-dot ${
                  status === 'connected' ? 'titlebar__status-dot--connected' :
                  status === 'connecting' ? 'titlebar__status-dot--connecting' : ''
                }`} aria-hidden="true" />
                <span className="titlebar__utility-label">Server</span>
                <span className="titlebar__utility-value">
                  {status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting…' : 'Offline'}
                </span>
              </div>
              <button
                className="titlebar__theme-toggle"
                onClick={() => { toggleTheme(); setUtilityMenuOpen(false); }}
                aria-label="Toggle theme"
                title={theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'}
              >
                <Icon name={theme === 'dark' ? 'sun' : 'moon'} size={16} />
                <span className="titlebar__utility-label">{theme === 'dark' ? 'Light mode' : 'Dark mode'}</span>
              </button>
              <button
                className={`titlebar__download-toggle${downloadManagerOpen ? ' is-active' : ''}${activeDownloadCount > 0 ? ' has-active-downloads' : ''}`}
                onClick={() => { setDownloadManagerOpen(open => !open); setUtilityMenuOpen(false); }}
                aria-label="Open download manager"
                aria-expanded={downloadManagerOpen}
                title="Download manager"
              >
                <Icon name="download" size={16} />
                <span className="titlebar__utility-label">Downloads</span>
                {activeDownloadCount > 0 && <span className="titlebar__download-badge">{activeDownloadCount > 9 ? '9+' : activeDownloadCount}</span>}
              </button>
            </div>
          </div>
          {isDesktop && (
            <>
              <button
                className="titlebar__window-btn"
                data-tauri-drag-region="false"
                onClick={() => window.api?.minimizeWindow?.()}
                aria-label="Minimize"
                title="Minimize"
              >
                <Icon name="minimize-2" size={14} />
              </button>
              <button
                className="titlebar__window-btn"
                data-tauri-drag-region="false"
                onClick={() => window.api?.maximizeWindow?.()}
                aria-label="Maximize"
                title="Maximize"
              >
                <Icon name="maximize-2" size={14} />
              </button>
              <button
                className="titlebar__window-btn titlebar__window-btn--close"
                data-tauri-drag-region="false"
                onClick={() => window.api?.closeWindow?.()}
                aria-label="Close"
                title="Close"
              >
                <Icon name="x" size={14} />
              </button>
            </>
          )}
        </div>
      </header>

      <DownloadManager isVisible={downloadManagerOpen} onClose={() => setDownloadManagerOpen(false)} />

      <main id="main-content" tabIndex={-1} className="view-container">
        <div className="view-slot" hidden={view !== 'chat'}>
          <ViewErrorBoundary view="chat">
            <ChatView
              key={clientDataResetNonce}
              currentModel={currentModel}
              loadedModels={loadedModels}
              onModelSelect={handleModelSelect}
              onOpenModelDetails={openModelDetails}
              onRefresh={handleRefreshModels}
            />
          </ViewErrorBoundary>
        </div>
        <div className="view-slot" hidden={view !== 'models'}>
          <ViewErrorBoundary view="models">
            <ModelManager
              key={clientDataResetNonce}
              onModelSelect={handleModelSelect}
              openModelRequest={modelDetailsRequest}
            />
          </ViewErrorBoundary>
        </div>
        <div className="view-slot" hidden={view !== 'backends'}>
          <ViewErrorBoundary view="backends">
            <BackendManager isActive={view === 'backends'} />
          </ViewErrorBoundary>
        </div>
        <div className="view-slot" hidden={view !== 'apps'}>
          <ViewErrorBoundary view="apps">
            <AppsView
              apps={marketplaceApps}
              categories={marketplaceCategories}
              loading={marketplaceLoading}
              error={marketplaceError}
            />
          </ViewErrorBoundary>
        </div>
        <div className="view-slot" hidden={view !== 'dashboard'}>
          <ViewErrorBoundary view="dashboard">
            <MonitorView
              activeSection={route.view === 'dashboard' ? route.section : lastWorkspaceSectionsRef.current.dashboard}
              isActive={view === 'dashboard'}
              onSectionChange={section => navigateToRoute({ view: 'dashboard', section })}
            />
          </ViewErrorBoundary>
        </div>
        <div className="view-slot" hidden={view !== 'connect'}>
          <ViewErrorBoundary view="connect">
            <ConnectView
              status={status}
              isActive={view === 'connect'}
              activeSection={route.view === 'connect' ? route.section : lastWorkspaceSectionsRef.current.connect}
              onSectionChange={section => navigateToRoute({ view: 'connect', section })}
              onLocalDataReset={handleLocalDataReset}
            />
          </ViewErrorBoundary>
        </div>
        </main>
      </div>
    </>
  );
};

export default App;
