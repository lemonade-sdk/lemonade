import React, { lazy, Suspense, useState, useEffect, useCallback, useMemo, useRef, Component, ErrorInfo, ReactNode } from 'react';
import type { LoadedModel, ModelInfo } from './api';
import { Icon } from './components/Icon';
import ChatView from './components/ChatView';
import { scheduleIdleWork } from './startupScheduler';
import { attachServerModelState, useServerModelState } from './features/models/modelState';
import { preloadInteractionSurfaces } from './interactionPreload';
const MARKETPLACE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';
type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
};
type MarketplaceCategory = { id: string; label: string };

import {
  WORKSPACE_NAVIGATION,
  type ConnectSection,
  type DashboardSection,
  type WorkspaceRoute,
  workspaceHash,
  workspaceRouteFromPath,
} from './features/navigation/workspaceNavigation';

// Chat is the default route and stays in the initial renderer chunk to avoid an
// immediate request waterfall. Non-default workspaces stay out of the cold path,
// then their code is warmed immediately after the first usable frame so the
// first tab switch does not pay the lazy-chunk latency.
type ViewModule<P extends object> = { default: React.ComponentType<P> };

let backendDataWarmStarted = false;

function warmBackendData(): void {
  if (backendDataWarmStarted) return;
  backendDataWarmStarted = true;

  void import(/* webpackChunkName: "api-client" */ './api')
    .then(({ default: api }) => {
      const warm = () => {
        if (api.systemInfoData) return;
        void api.systemInfo().catch(error => {
          // This is speculative warming only. BackendManager still owns the
          // user-visible error/retry path when the workspace is actually open.
          console.debug('Backend system-info prewarm skipped:', error);
        });
      };

      if (api.status === 'connected' || api.healthData) {
        warm();
        return;
      }

      const unsubscribe = api.onStatusChange(status => {
        if (status !== 'connected') return;
        unsubscribe();
        warm();
      });
    })
    .catch(error => console.debug('Backend data prewarm unavailable:', error));
}

function createPreloadableView<P extends object>(loader: () => Promise<ViewModule<P>>) {
  let resolved: React.ComponentType<P> | null = null;
  let modulePromise: Promise<ViewModule<P>> | null = null;

  const load = () => {
    if (!modulePromise) {
      modulePromise = loader().then(module => {
        resolved = module.default;
        return module;
      });
    }
    return modulePromise;
  };

  const LazyView = lazy(load);
  const View = (props: P) => resolved
    ? React.createElement(resolved, props)
    : React.createElement(LazyView, props);

  return { View, preload: () => load().then(() => undefined) };
}

const loadModelManagerView = () => import(/* webpackChunkName: "view-models" */ './components/ModelManager');
const loadMonitorView = () => import(/* webpackChunkName: "view-monitor" */ './components/MonitorView');


const modelManagerView = createPreloadableView(loadModelManagerView);
const backendManagerView = createPreloadableView(() => {
  // Start expensive backend discovery as soon as navigation intent/code preload
  // is known, but never wait for it before resolving the workspace module.
  warmBackendData();
  return import(/* webpackChunkName: "view-backends" */ './components/BackendManager');
});
const appsWorkspaceView = createPreloadableView(() => import(/* webpackChunkName: "view-apps" */ './components/AppsView'));
const monitorWorkspaceView = createPreloadableView(loadMonitorView);
const connectWorkspaceView = createPreloadableView(() => import(/* webpackChunkName: "view-settings" */ './components/ConnectView'));
const downloadManagerView = createPreloadableView(() => import(/* webpackChunkName: "download-manager" */ './components/DownloadManager'));

const ModelManager = modelManagerView.View;
const BackendManager = backendManagerView.View;
const AppsView = appsWorkspaceView.View;
const MonitorView = monitorWorkspaceView.View;
const ConnectView = connectWorkspaceView.View;
const DownloadManager = downloadManagerView.View;

type View = 'chat' | 'models' | 'backends' | 'apps' | 'dashboard' | 'connect';
type SimpleView = Exclude<View, 'dashboard' | 'connect'>;
type LazyWorkspace = Exclude<View, 'chat'>;

const WORKSPACE_PRELOADERS: Record<LazyWorkspace, () => Promise<void>> = {
  models: async () => {
    await modelManagerView.preload();
    const module = await loadModelManagerView();
    await module.preloadModelManagerSecondary();
  },
  backends: backendManagerView.preload,
  apps: appsWorkspaceView.preload,
  dashboard: async () => {
    await monitorWorkspaceView.preload();
    const module = await loadMonitorView();
    await module.preloadMonitorSections();
  },
  connect: connectWorkspaceView.preload,
};
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
          <button
            type="button"
            className="btn btn--primary btn--medium workspace-action-button workspace-action-button--primary workspace-action-button--medium"
            onClick={() => this.setState({ error: null })}
          >
            <span className="workspace-action-button__label">Try again</span>
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}

const SIMPLE_VIEWS: SimpleView[] = ['chat', 'models', 'backends', 'apps'];

const ViewLoadingFallback: React.FC<{ label?: string }> = ({ label = 'Loading view' }) => (
  <div className="view-loading" role="status" aria-label={label} aria-live="polite">
    <span className="spinner" aria-hidden="true" />
  </div>
);


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

type ModelHelpers = Pick<typeof import('./modelCapabilities'), 'canSelectInComposer' | 'capabilityFromModelInfo' | 'selectPreferredLoadedModel'>
  & Pick<typeof import('./features/collections/collectionModels'), 'findModelInfoByName' | 'isCollectionFullyLoaded' | 'isCollectionModel' | 'withVirtualLoadedCollections'>;
type AppApiClient = (typeof import('./api'))['default'];

type Theme = 'dark' | 'light';
const THEME_KEY = 'lemonade_theme';
const EMPTY_MODELS: ModelInfo[] = [];
const EMPTY_LOADED_MODELS: LoadedModel[] = [];

function friendlyErrorMessage(error: unknown): string {
  if (error && typeof error === 'object') {
    const value = error as { userMessage?: unknown; message?: unknown };
    if (typeof value.userMessage === 'string' && value.userMessage) return value.userMessage;
    if (typeof value.message === 'string' && value.message) return value.message;
  }
  return String(error || 'Unknown error');
}

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
  // Mount only the active workspace on cold start. Once a workspace has been
  // visited it stays mounted (hidden when inactive) so navigation still
  // preserves its local UI state without paying for every view up front.
  const mountedViewsRef = useRef<Set<View>>(new Set());
  mountedViewsRef.current.add(view);
  const routeRef = useRef(route);
  routeRef.current = route;
  const apiClientRef = useRef<AppApiClient | null>(null);
  const serverModelState = useServerModelState();
  const status = serverModelState.status;
  const serverModels = serverModelState.models?.data ?? EMPTY_MODELS;
  const rawLoadedModels = serverModelState.health?.all_models_loaded ?? EMPTY_LOADED_MODELS;
  const [currentModel, setCurrentModel] = useState<string | null>(null);
  const [theme, setTheme] = useState<Theme>(loadTheme);
  const [clientDataResetNonce, setClientDataResetNonce] = useState(0);
  const [customModelInfos, setCustomModelInfos] = useState<ModelInfo[]>(EMPTY_MODELS);
  const [modelHelpers, setModelHelpers] = useState<ModelHelpers | null>(null);
  const [downloadManagerOpen, setDownloadManagerOpen] = useState(false);
  const downloadManagerMountedRef = useRef(false);
  if (downloadManagerOpen) downloadManagerMountedRef.current = true;
  const [modelDetailsRequest, setModelDetailsRequest] = useState<{ modelName: string; nonce: number } | null>(null);
  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const [marketplaceLoading, setMarketplaceLoading] = useState(true);
  const marketplaceRequestedRef = useRef(false);
  const [utilityMenuOpen, setUtilityMenuOpen] = useState(false);
  const [navigationSearch, setNavigationSearch] = useState('');
  const [navigationSearchOpen, setNavigationSearchOpen] = useState(false);
  const [navigationSearchIndex, setNavigationSearchIndex] = useState(0);
  const utilityMenuRef = useRef<HTMLDivElement>(null);
  const utilityMenuTriggerRef = useRef<HTMLButtonElement>(null);
  const navigationSearchRef = useRef<HTMLInputElement>(null);
  const [isDesktop, setIsDesktop] = useState(false);
  const navigationSearchShortcut = /Mac|iPhone|iPad|iPod/.test(navigator.userAgent) ? '⌘ K' : 'Ctrl+K';

  const requestMarketplace = useCallback(() => {
    if (marketplaceRequestedRef.current) return;
    marketplaceRequestedRef.current = true;
    setMarketplaceLoading(true);
    fetch(MARKETPLACE_URL)
      .then(response => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(data => {
        setMarketplaceApps(Array.isArray(data?.apps) ? data.apps as MarketplaceApp[] : []);
        setMarketplaceCategories(Array.isArray(data?.categories) ? data.categories as MarketplaceCategory[] : []);
        setMarketplaceError(null);
      })
      .catch(error => {
        setMarketplaceError(friendlyErrorMessage(error));
      })
      .finally(() => {
        setMarketplaceLoading(false);
      });
  }, []);

  useEffect(() => {
    // Still load on demand if the user reaches Apps before background warming
    // has run (for example via a deep link or an immediate keyboard action).
    if (view === 'apps' || navigationSearchOpen) requestMarketplace();
  }, [navigationSearchOpen, requestMarketplace, view]);

  useEffect(() => {
    // Keep the cold path minimal, but do not move that latency to navigation.
    // Once the first usable frame is visible, evaluate every workspace module
    // plus the heavy secondary surfaces that used to load on click (Monitor
    // Telemetry/Logs and Models details/editors). They are cached but not mounted,
    // so effects and polling still do not run until the user opens the surface.
    let cancelled = false;

    const warmRemainingWorkspaces = () => {
      // Inspector state is a public runtime contract (window.inspectStore) and
      // also owns capture/session-header wiring. Initialize it immediately after
      // the first usable frame instead of waiting for the Telemetry tab to mount.
      void import(/* webpackChunkName: "inspect-store" */ './inspectStore').catch(error => {
        console.debug('Inspector state prewarm unavailable:', error);
      });

      if (cancelled) return;

      // First warm only the two interactions users can notice immediately:
      // Models -> Details and Monitor -> Telemetry. This starts after reveal,
      // so it cannot regress first paint. The remaining interaction surfaces
      // are scheduled by preloadInteractionSurfaces() in the background.
      void preloadInteractionSurfaces().finally(() => {
        if (cancelled) return;
        const preloaders = (Object.keys(WORKSPACE_PRELOADERS) as LazyWorkspace[])
          .map(target => WORKSPACE_PRELOADERS[target]);

        void Promise.allSettled(preloaders.map(preload => preload()))
          .then(results => {
            if (cancelled) return;
            const failed = results.filter(result => result.status === 'rejected');
            if (failed.length) {
              console.warn(`Failed to preload ${failed.length} workspace chunk(s)`);
              return;
            }
            if (window.__LEMONADE_STARTUP_METRICS__) {
              window.__LEMONADE_STARTUP_METRICS__.workspacePreloadReadyMs = performance.now();
            }
          });

        requestMarketplace();
      });
    };

    if (window.__LEMONADE_STARTUP_METRICS__?.firstUsableFrameMs !== undefined) {
      warmRemainingWorkspaces();
    } else {
      window.addEventListener('lemonade:first-usable', warmRemainingWorkspaces, { once: true });
    }

    return () => {
      cancelled = true;
      window.removeEventListener('lemonade:first-usable', warmRemainingWorkspaces);
    };
  }, [requestMarketplace]);

  useEffect(() => {
    // Paint the application shell before loading the native bridge and before
    // starting server I/O. Native settings are loaded explicitly here so we no
    // longer depend on the hidden Settings screen mounting at startup.
    let cancelled = false;
    let frame = 0;
    let timer = 0;

    const bootstrapHost = async () => {
      if (window.__LEMONADE_STARTUP_METRICS__) {
        window.__LEMONADE_STARTUP_METRICS__.hostBootstrapStartMs = performance.now();
      }
      try {
        // Pure-web launches already have their host bridge injected by the
        // server. Do not request/evaluate the Tauri shim there. Desktop still
        // loads it in parallel with the API client.
        const tauriReadyPromise = '__TAURI_INTERNALS__' in window
          ? import(/* webpackChunkName: "tauri-shim" */ './tauriShim').then(module => module.tauriReady)
          : Promise.resolve();
        const { default: api } = await import(/* webpackChunkName: "api-client" */ './api');
        apiClientRef.current = api;
        attachServerModelState(api);
        await tauriReadyPromise;
        if (window.__LEMONADE_STARTUP_METRICS__) {
          window.__LEMONADE_STARTUP_METRICS__.hostBridgeReadyMs = performance.now();
        }
        if (cancelled) return;
        if (window.api && window.api.isWebApp !== true) setIsDesktop(true);
        try {
          await api.loadConnectionSettings();
          if (window.__LEMONADE_STARTUP_METRICS__) {
            window.__LEMONADE_STARTUP_METRICS__.connectionSettingsReadyMs = performance.now();
          }
        } catch (error) {
          console.warn('Failed to load host connection settings:', error);
        }
        if (!cancelled) {
          void api.connect();
          if (routeRef.current.view === 'dashboard') api.stopPolling();
          else api.startPolling(15000);
        }
      } catch (error) {
        console.warn('Host bootstrap failed:', error);
        if (!cancelled) {
          void import(/* webpackChunkName: "api-client" */ './api')
            .then(async ({ default: api }) => {
              apiClientRef.current = api;
              attachServerModelState(api);
              void api.connect();
              if (routeRef.current.view === 'dashboard') api.stopPolling();
              else api.startPolling(15000);
            })
            .catch(apiError => console.warn('API bootstrap failed:', apiError));
        }
      }
    };

    frame = window.requestAnimationFrame(() => {
      timer = window.setTimeout(() => { void bootstrapHost(); }, 0);
    });

    return () => {
      cancelled = true;
      window.cancelAnimationFrame(frame);
      window.clearTimeout(timer);
      apiClientRef.current?.stopPolling();
    };
  }, []);
  // Download persistence/polling is sizeable and not needed for the first
  // paint. Hydrate the badge after the shell is visible instead of importing
  // the whole download manager into the entry bundle.
  const [activeDownloadCount, setActiveDownloadCount] = useState(0);
  const activeDownloadCountRef = useRef(0);
  const lastWorkspaceSectionsRef = useRef({
    dashboard: route.view === 'dashboard' ? route.section : WORKSPACE_NAVIGATION.dashboard.defaultSection,
    connect: route.view === 'connect' ? route.section : WORKSPACE_NAVIGATION.connect.defaultSection,
  });
  useEffect(() => {
    let cancelled = false;
    let unsubscribe: (() => void) | undefined;
    const cancelSchedule = scheduleIdleWork(() => {
      void import(/* webpackChunkName: "download-store" */ './features/downloadManager/downloadStore')
        .then(({ downloadStore, isDownloadActive }) => {
          if (cancelled) return;
          const update = (items: ReturnType<typeof downloadStore.snapshot>) => {
            const nextCount = items.filter(isDownloadActive).length;
            if (nextCount === activeDownloadCountRef.current) return;
            activeDownloadCountRef.current = nextCount;
            setActiveDownloadCount(nextCount);
          };
          update(downloadStore.snapshot());
          unsubscribe = downloadStore.subscribe(update);
        })
        .catch(error => console.warn('Failed to hydrate download activity:', error));
    }, 900);
    return () => {
      cancelled = true;
      cancelSchedule();
      unsubscribe?.();
    };
  }, []);

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem(THEME_KEY, theme); } catch { /* ignore */ }
  }, [theme]);

  useEffect(() => {
    if (status !== 'connected') return;
    const metrics = window.__LEMONADE_STARTUP_METRICS__;
    if (metrics && metrics.serverConnectedMs === undefined) {
      metrics.serverConnectedMs = performance.now();
      console.info('[startup] Lemonade server connected', { ...metrics });
    }
  }, [status]);

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

  useEffect(() => {
    if (modelHelpers || (serverModels.length === 0 && rawLoadedModels.length === 0 && customModelInfos.length === 0)) return;
    let cancelled = false;
    void Promise.all([
      import(/* webpackChunkName: "model-capabilities" */ './modelCapabilities'),
      import(/* webpackChunkName: "collection-models" */ './features/collections/collectionModels'),
    ]).then(([capabilities, collections]) => {
      if (!cancelled) setModelHelpers({
        canSelectInComposer: capabilities.canSelectInComposer,
        capabilityFromModelInfo: capabilities.capabilityFromModelInfo,
        selectPreferredLoadedModel: capabilities.selectPreferredLoadedModel,
        findModelInfoByName: collections.findModelInfoByName,
        isCollectionFullyLoaded: collections.isCollectionFullyLoaded,
        isCollectionModel: collections.isCollectionModel,
        withVirtualLoadedCollections: collections.withVirtualLoadedCollections,
      });
    }).catch(error => console.warn('Failed to hydrate model helpers:', error));
    return () => { cancelled = true; };
  }, [customModelInfos.length, modelHelpers, rawLoadedModels.length, serverModels.length]);

  useEffect(() => {
    let cancelled = false;
    const cancelSchedule = scheduleIdleWork(() => {
      void import(/* webpackChunkName: "custom-model-store" */ './features/customModels/customModelStore')
        .then(({ loadCustomModels, customModelToModelInfo }) => {
          if (!cancelled) setCustomModelInfos(loadCustomModels().map(customModelToModelInfo));
        })
        .catch(error => console.warn('Failed to hydrate custom models:', error));
    }, 700);
    return () => {
      cancelled = true;
      cancelSchedule();
    };
  }, [clientDataResetNonce]);

  const loadedModelViewState = useMemo(() => {
    const customInfos = customModelInfos;
    const knownInfos = [...customInfos, ...serverModels];
    if (!modelHelpers) return { models: rawLoadedModels, customInfos, knownInfos };
    const models = modelHelpers.withVirtualLoadedCollections(rawLoadedModels, knownInfos).map(model => {
      const info = modelHelpers.findModelInfoByName(knownInfos, model.model_name);
      if (!info) return model;
      const cap = modelHelpers.capabilityFromModelInfo(info);
      return {
        ...model,
        type: cap === 'unknown' ? model.type : cap,
        recipe: model.recipe || String((info as any).recipe || ''),
        checkpoint: model.checkpoint || String((info as any).checkpoint || ''),
      };
    });
    return { models, customInfos, knownInfos };
  }, [customModelInfos, modelHelpers, rawLoadedModels, serverModels]);

  const loadedModels = loadedModelViewState.models;

  useEffect(() => {
    if (!modelHelpers) return;
    const { customInfos, knownInfos } = loadedModelViewState;
    const customSelectable = (name: string) => {
      const info = modelHelpers.findModelInfoByName(customInfos, name);
      if (!info) return false;
      const cap = modelHelpers.capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'audio-generation' || cap === 'tts' || cap === 'model3d';
    };
    const infoSelectable = (name: string) => {
      const info = modelHelpers.findModelInfoByName(knownInfos, name);
      if (!info) return false;
      const cap = modelHelpers.capabilityFromModelInfo(info);
      return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'audio-generation' || cap === 'tts' || cap === 'model3d';
    };
    setCurrentModel(current => {
      if (current && loadedModels.some(m => m.model_name === current && (modelHelpers.canSelectInComposer(m) || customSelectable(m.model_name) || infoSelectable(m.model_name)))) return current;
      if (current) {
        const info = modelHelpers.findModelInfoByName(knownInfos, current);
        if (info && modelHelpers.isCollectionModel(info) && modelHelpers.isCollectionFullyLoaded(info, rawLoadedModels)) return current;
      }
      const virtualOmni = loadedModels.find(model => {
        const info = modelHelpers.findModelInfoByName(knownInfos, model.model_name);
        return info && modelHelpers.isCollectionModel(info);
      });
      return virtualOmni?.model_name
        || modelHelpers.selectPreferredLoadedModel(loadedModels)?.model_name
        || loadedModels.find(m => customSelectable(m.model_name) || infoSelectable(m.model_name))?.model_name
        || null;
    });
  }, [loadedModelViewState, loadedModels, modelHelpers, rawLoadedModels]);

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

  // The host bootstrap owns the API import and starts polling after its
  // first connection attempt. Route changes only toggle that already-loaded
  // client, avoiding a second API import request during the first React commit.
  useEffect(() => {
    const api = apiClientRef.current;
    if (!api) return;
    if (view === 'dashboard') api.stopPolling();
    else api.startPolling(15000);
  }, [view]);

  const handleRefreshModels = useCallback(async () => {
    const { default: api } = await import(/* webpackChunkName: "api-client" */ './api');
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
              onPointerEnter={() => {
                if (id !== 'chat') void WORKSPACE_PRELOADERS[id as LazyWorkspace]().catch(() => undefined);
              }}
              onFocus={() => {
                if (id !== 'chat') void WORKSPACE_PRELOADERS[id as LazyWorkspace]().catch(() => undefined);
              }}
              onPointerDown={() => {
                if (id !== 'chat') void WORKSPACE_PRELOADERS[id as LazyWorkspace]().catch(() => undefined);
              }}
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
                      role="combobox"
                      aria-label={view === 'apps' ? 'Search apps' : 'Search Lemonade'}
                      aria-autocomplete="list"
                      aria-expanded={navigationSearchOpen}
                      aria-controls="titlebar-search-results"
                      aria-activedescendant={navigationSearchOpen && navigationSearchResults[navigationSearchIndex]
                        ? `titlebar-search-option-${navigationSearchIndex}`
                        : undefined}
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
                      {navigationSearchShortcut}
                    </kbd>
                  </div>
                  {navigationSearchOpen && (
                    <div id="titlebar-search-results" className="titlebar__search-results" role="listbox" aria-label="Global search results">
                      {navigationSearchResults.length > 0 ? navigationSearchResults.map((destination, index) => (
                        <button
                          key={destination.id}
                          id={`titlebar-search-option-${index}`}
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

      {downloadManagerMountedRef.current && (
        <Suspense fallback={downloadManagerOpen ? <ViewLoadingFallback label="Loading downloads" /> : null}>
          <DownloadManager isVisible={downloadManagerOpen} onClose={() => setDownloadManagerOpen(false)} />
        </Suspense>
      )}

      <main id="main-content" tabIndex={-1} className="view-container">
        {mountedViewsRef.current.has('chat') && (
          <div className="view-slot" hidden={view !== 'chat'}>
            <ViewErrorBoundary view="chat">
              <ChatView
                key={clientDataResetNonce}
                currentModel={currentModel}
                loadedModels={loadedModels}
                serverModels={serverModels}
                connectionStatus={status}
                onModelSelect={handleModelSelect}
                onOpenModelDetails={openModelDetails}
                onRefresh={handleRefreshModels}
              />
            </ViewErrorBoundary>
          </div>
        )}
        {mountedViewsRef.current.has('models') && (
          <div className="view-slot" hidden={view !== 'models'}>
            <ViewErrorBoundary view="models">
              <Suspense fallback={<ViewLoadingFallback label="Loading models" />}>
                <ModelManager
                  key={clientDataResetNonce}
                  onModelSelect={handleModelSelect}
                  openModelRequest={modelDetailsRequest}
                />
              </Suspense>
            </ViewErrorBoundary>
          </div>
        )}
        {mountedViewsRef.current.has('backends') && (
          <div className="view-slot" hidden={view !== 'backends'}>
            <ViewErrorBoundary view="backends">
              <Suspense fallback={<ViewLoadingFallback label="Loading backends" />}>
                <BackendManager isActive={view === 'backends'} />
              </Suspense>
            </ViewErrorBoundary>
          </div>
        )}
        {mountedViewsRef.current.has('apps') && (
          <div className="view-slot" hidden={view !== 'apps'}>
            <ViewErrorBoundary view="apps">
              <Suspense fallback={<ViewLoadingFallback label="Loading apps" />}>
                <AppsView
                  apps={marketplaceApps}
                  categories={marketplaceCategories}
                  loading={marketplaceLoading}
                  error={marketplaceError}
                />
              </Suspense>
            </ViewErrorBoundary>
          </div>
        )}
        {mountedViewsRef.current.has('dashboard') && (
          <div className="view-slot" hidden={view !== 'dashboard'}>
            <ViewErrorBoundary view="dashboard">
              <Suspense fallback={<ViewLoadingFallback label="Loading monitor" />}>
                <MonitorView
                  activeSection={route.view === 'dashboard' ? route.section : lastWorkspaceSectionsRef.current.dashboard}
                  isActive={view === 'dashboard'}
                  onSectionChange={section => navigateToRoute({ view: 'dashboard', section })}
                />
              </Suspense>
            </ViewErrorBoundary>
          </div>
        )}
        {mountedViewsRef.current.has('connect') && (
          <div className="view-slot" hidden={view !== 'connect'}>
            <ViewErrorBoundary view="connect">
              <Suspense fallback={<ViewLoadingFallback label="Loading settings" />}>
                <ConnectView
                  status={status}
                  isActive={view === 'connect'}
                  activeSection={
                    route.view === 'connect'
                      ? route.section
                      : lastWorkspaceSectionsRef.current.connect
                  }
                  onSectionChange={section =>
                    navigateToRoute({ view: 'connect', section })
                  }
                  onLocalDataReset={handleLocalDataReset}
                  models={loadedModelViewState.knownInfos}
                  loadedModels={loadedModels}
                />
              </Suspense>
            </ViewErrorBoundary>
          </div>
        )}
      </main>
      </div>
    </>
  );
};

export default App;
