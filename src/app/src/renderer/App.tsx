import React, { useState, useEffect, useRef, useCallback } from 'react';
import { createPortal } from 'react-dom';
import { ChevronLeft } from './components/Icons';
import TitleBar from './TitleBar';
import ChatWindow from './ChatWindow';
import ModelManager, { LeftPanelView } from './ModelManager';
import LogsWindow from './LogsWindow';
import ResizableDivider from './ResizableDivider';
import DownloadManager from './DownloadManager';
import StatusBar from './StatusBar';
import { ModelsProvider, useModels } from './hooks/useModels';
import { SystemProvider } from './hooks/useSystem';
import { DEFAULT_LAYOUT_SETTINGS } from './utils/appSettings';
import { downloadTracker } from './utils/downloadTracker';
import CustomCollectionPanel from './components/CustomCollectionPanel';
import RouterCollectionPanel from './components/RouterCollectionPanel';
import { ToastContainer, useToast } from './Toast';
import { pullModel, type ModelRegistrationData } from './utils/backendInstaller';
import {
  CustomCollectionDraft,
  RouterCollectionDraft,
  buildCustomCollectionPullRequest,
  buildRouterCollectionPullRequest,
  getCollectionDisplayName,
  routingBlocksEquivalent,
  validateRouterImportPayload,
} from './utils/customCollections';
import { isCollectionRecipe, COLLECTION_ROUTER_MODEL_RECIPE } from './utils/recipeNames';
import { buildModelExportFile, downloadJsonFile } from './utils/modelData';
import { isModelEffectivelyDownloaded } from './utils/collectionModels';
import '../../styles/index.css';

type PullRegistrationPayload = {
  model_name: string;
  recipe: string;
  checkpoint?: string;
  checkpoints?: Record<string, string>;
  components?: string[];
  models?: Array<Record<string, unknown>>;
  labels?: string[];
  mmproj?: string;
  size?: number;
  image_defaults?: unknown;
  reasoning?: boolean;
  vision?: boolean;
};


const LAYOUT_CONSTANTS = {
  modelManagerMinWidth: 200,
  experienceRailWidth: 40,
  mainContentMinWidth: 300,
  chatMinWidth: 250,
  dividerWidth: 4,
  absoluteMinWidth: 400,
};

// Inner component that can use SystemProvider context
const AppContent: React.FC = () => {
  const [theme, setTheme] = useState(DEFAULT_LAYOUT_SETTINGS.theme);
  const [isChatVisible, setIsChatVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isChatVisible);
  const [isModelManagerVisible, setIsModelManagerVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isModelManagerVisible);
  const [leftPanelView, setLeftPanelView] = useState<LeftPanelView>('models');
  const [externalContentUrl, setExternalContentUrl] = useState<string | null>(null);
  const [isLogsVisible, setIsLogsVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isLogsVisible);
  const [isDownloadManagerVisible, setIsDownloadManagerVisible] = useState(false);
  const [modelManagerWidth, setModelManagerWidth] = useState(DEFAULT_LAYOUT_SETTINGS.modelManagerWidth);
  const [chatWidth, setChatWidth] = useState(DEFAULT_LAYOUT_SETTINGS.chatWidth);
  const [logsHeight, setLogsHeight] = useState(DEFAULT_LAYOUT_SETTINGS.logsHeight);
  const [layoutLoaded, setLayoutLoaded] = useState(false);
  const [customCollectionModal, setCustomCollectionModal] = useState<{ mode: 'create' | 'edit'; collectionId?: string } | null>(null);
  const importCollectionFileRef = useRef<HTMLInputElement>(null);
  const [routerCollectionModal, setRouterCollectionModal] = useState<{ mode: 'create' | 'edit'; collectionId?: string } | null>(null);
  const importRouterFileRef = useRef<HTMLInputElement>(null);
  const { modelsData, selectedModel, setSelectedModel, setUserHasSelectedModel, refresh: refreshModels } = useModels();
  const { toasts, removeToast, showError, showSuccess, showInfo } = useToast();
  const isDraggingRef = useRef<'left' | 'right' | 'bottom' | null>(null);
  const startXRef = useRef(0);
  const startYRef = useRef(0);
  const startWidthRef = useRef(0);
  const startHeightRef = useRef(0);
  // Auto-open the manager when a tab first discovers active downloads, but
  // respect a user close while those same downloads are still active. Without
  // this, the 2 s /downloads poll reopens the panel immediately.
  const suppressDownloadAutoOpenRef = useRef(false);

  // Load saved layout settings on mount
  useEffect(() => {
    const loadLayoutSettings = async () => {
      try {
        if (window?.api?.getSettings) {
          const settings = await window.api.getSettings();
          if (settings.layout) {
            setTheme(settings.layout.theme ?? DEFAULT_LAYOUT_SETTINGS.theme);
            setIsChatVisible(settings.layout.isChatVisible ?? DEFAULT_LAYOUT_SETTINGS.isChatVisible);
            setIsModelManagerVisible(settings.layout.isModelManagerVisible ?? DEFAULT_LAYOUT_SETTINGS.isModelManagerVisible);
            const savedView = settings.layout.leftPanelView;
            if (savedView === 'models' || savedView === 'marketplace' || savedView === 'backends' || savedView === 'settings') {
              setLeftPanelView(savedView);
            }
            setIsLogsVisible(settings.layout.isLogsVisible ?? DEFAULT_LAYOUT_SETTINGS.isLogsVisible);
            setModelManagerWidth(settings.layout.modelManagerWidth ?? DEFAULT_LAYOUT_SETTINGS.modelManagerWidth);
            setChatWidth(settings.layout.chatWidth ?? DEFAULT_LAYOUT_SETTINGS.chatWidth);
            setLogsHeight(settings.layout.logsHeight ?? DEFAULT_LAYOUT_SETTINGS.logsHeight);
          }
        }
      } catch (error) {
        console.error('Failed to load layout settings:', error);
      } finally {
        // Override with URL parameters if present
        const urlParams = new URLSearchParams(window.location.search);
        if (urlParams.get('view') === 'logs') {
          setIsLogsVisible(true);
        }
        setLayoutLoaded(true);
      }
    };
    loadLayoutSettings();
  }, []);

  // Save layout settings when they change (debounced)
  const saveLayoutSettings = useCallback(async () => {
    if (!layoutLoaded) return;
    try {
      if (window?.api?.getSettings && window?.api?.saveSettings) {
        // Get current settings and merge layout changes
        const currentSettings = await window.api.getSettings();
        await window.api.saveSettings({
          ...currentSettings,
          layout: {
            theme,
            isChatVisible,
            isModelManagerVisible,
            leftPanelView,
            isLogsVisible,
            modelManagerWidth,
            chatWidth,
            logsHeight,
          },
        });
      }
    } catch (error) {
      console.error('Failed to save layout settings:', error);
    }
  }, [layoutLoaded, theme, isChatVisible, isModelManagerVisible, leftPanelView, isLogsVisible, modelManagerWidth, chatWidth, logsHeight]);

  // Debounced save effect
  useEffect(() => {
    if (!layoutLoaded) return;
    const timeoutId = setTimeout(saveLayoutSettings, 300);
    return () => clearTimeout(timeoutId);
  }, [saveLayoutSettings, layoutLoaded]);

  // Listen for download events to automatically open the download manager.
  useEffect(() => {
    const isVisibleStatus = (status?: string) =>
      status === 'downloading' || status === 'paused' || status === 'error';

    const openIfActive = (downloads = downloadTracker.getActiveDownloads()) => {
      const hasVisibleDownload = downloads.some((d: any) => isVisibleStatus(d?.status));
      if (!hasVisibleDownload) {
        suppressDownloadAutoOpenRef.current = false;
        return false;
      }
      if (!suppressDownloadAutoOpenRef.current) {
        setIsDownloadManagerVisible(true);
      }
      return true;
    };

    const handleDownloadStart = () => {
      suppressDownloadAutoOpenRef.current = false;
      setIsDownloadManagerVisible(true);
    };

    const handleDownloadSignal = (e: any) => {
      const downloads = Array.isArray(e.detail?.downloads) ? e.detail.downloads : downloadTracker.getActiveDownloads();
      openIfActive(downloads);
    };

    const handleChatDownloadComplete = () => {
      suppressDownloadAutoOpenRef.current = true;
      setIsDownloadManagerVisible(false);
    };

    downloadTracker.startServerPolling();

    window.addEventListener('download:started' as any, handleDownloadStart);
    window.addEventListener('download:update' as any, handleDownloadSignal);
    window.addEventListener('download:snapshot' as any, handleDownloadSignal);
    window.addEventListener('download:chatComplete' as any, handleChatDownloadComplete);

    void downloadTracker.hydrateFromServer().then(() => openIfActive());

    const handleOpenExternalContent = (e: any) => {
      if (e.detail?.url) {
        setExternalContentUrl(e.detail.url);
        setIsChatVisible(true);
        if (!openIfActive()) {
          setIsDownloadManagerVisible(false);
        }
      }
    };
    window.addEventListener('open-external-content' as any, handleOpenExternalContent);

    return () => {
      window.removeEventListener('download:started' as any, handleDownloadStart);
      window.removeEventListener('download:update' as any, handleDownloadSignal);
      window.removeEventListener('download:snapshot' as any, handleDownloadSignal);
      window.removeEventListener('download:chatComplete' as any, handleChatDownloadComplete);
      window.removeEventListener('open-external-content' as any, handleOpenExternalContent);
    };
  }, [refreshModels]);

  // Handle lemonade:// protocol navigation from main process.
  // Must await tauriReady because window.api is installed asynchronously
  // and isn't available on the first render.
  useEffect(() => {
    let unsubscribe: (() => void) | undefined;
    let cancelled = false;
    (async () => {
      const { tauriReady } = await import('./tauriShim');
      await tauriReady;
      if (cancelled || !window?.api?.onNavigate) return;
      const unsub = window.api.onNavigate((data: { view?: string; model?: string }) => {
        if (data.view === 'logs') {
          setIsLogsVisible(true);
        }
      });
      if (typeof unsub === 'function') unsubscribe = unsub;
      window?.api?.signalReady?.();
    })();
    return () => { cancelled = true; unsubscribe?.(); };
  }, []);

  useEffect(() => {
    const hasMainColumn = isLogsVisible;
    let computedMinWidth = LAYOUT_CONSTANTS.experienceRailWidth; // Rail always visible

    if (isModelManagerVisible) {
      computedMinWidth += LAYOUT_CONSTANTS.modelManagerMinWidth;
    }

    if (hasMainColumn) {
      computedMinWidth += LAYOUT_CONSTANTS.mainContentMinWidth;
    }

    if (isChatVisible) {
      computedMinWidth += LAYOUT_CONSTANTS.chatMinWidth;
    }

    let dividerCount = 0;
    if (isModelManagerVisible && (hasMainColumn || isChatVisible)) {
      dividerCount += 1;
    }
    if (hasMainColumn && isChatVisible) {
      dividerCount += 1;
    }

    computedMinWidth += dividerCount * LAYOUT_CONSTANTS.dividerWidth;

    const targetWidth = Math.max(computedMinWidth, LAYOUT_CONSTANTS.absoluteMinWidth);
    if (window?.api?.updateMinWidth) {
      window.api.updateMinWidth(targetWidth);
    }
  }, [isModelManagerVisible, isLogsVisible, isChatVisible]);

  useEffect(() => {
    const handleMouseMove = (e: MouseEvent) => {
      if (!isDraggingRef.current) return;

      if (isDraggingRef.current === 'left') {
        // Dragging left divider (model manager)
        const delta = e.clientX - startXRef.current;
        const newWidth = Math.max(200, Math.min(500, startWidthRef.current + delta));
        setModelManagerWidth(newWidth);
      } else if (isDraggingRef.current === 'right') {
        // Dragging right divider (chat window)
        // For right-side panel: drag left = increase width, drag right = decrease width
        const delta = startXRef.current - e.clientX;

        // Base chat width from drag
        let proposedWidth = startWidthRef.current + delta;

        // Compute maximum allowed width so the center panel (welcome screen/logs)
        // can respect its minimum width and the layout doesn't overflow horizontally.
        const appLayout = document.querySelector('.app-layout') as HTMLElement | null;
        const appWidth = appLayout?.clientWidth || window.innerWidth;

        const leftPanelWidth = isModelManagerVisible
          ? modelManagerWidth + LAYOUT_CONSTANTS.experienceRailWidth
          : LAYOUT_CONSTANTS.experienceRailWidth;
        const hasCenterColumn = isLogsVisible;
        const minCenterWidth = hasCenterColumn ? 300 : 0; // keep in sync with CSS min-width

        // Account for vertical dividers (each 4px wide)
        const dividerCount =
          ((isModelManagerVisible && (hasCenterColumn || isChatVisible)) ? 1 : 0) +
          ((hasCenterColumn && isChatVisible) ? 1 : 0);
        const dividerSpace = dividerCount * 4;

        const maxWidthFromLayout = appWidth - leftPanelWidth - minCenterWidth - dividerSpace;

        const minChatWidth = 250;
        const maxChatWidth = Math.max(
          minChatWidth,
          Math.min(800, isFinite(maxWidthFromLayout) ? maxWidthFromLayout : 800)
        );

        const newWidth = Math.max(minChatWidth, Math.min(maxChatWidth, proposedWidth));
        setChatWidth(newWidth);
      } else if (isDraggingRef.current === 'bottom') {
        // Dragging bottom divider (logs window)
        const delta = startYRef.current - e.clientY;
        const newHeight = Math.max(100, Math.min(400, startHeightRef.current + delta));
        setLogsHeight(newHeight);
      }
    };

    const handleMouseUp = () => {
      isDraggingRef.current = null;
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    };

    window.addEventListener('mousemove', handleMouseMove);
    window.addEventListener('mouseup', handleMouseUp);

    return () => {
      window.removeEventListener('mousemove', handleMouseMove);
      window.removeEventListener('mouseup', handleMouseUp);
    };
  }, [
    chatWidth,
    isChatVisible,
    isModelManagerVisible,
    isLogsVisible,
    modelManagerWidth,
    logsHeight,
  ]);

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);


  useEffect(() => {
    const handleOpenCustomCollection = () => setCustomCollectionModal({ mode: 'create' });
    const handleImportCustomCollection = () => importCollectionFileRef.current?.click();
    const handleEditCustomCollection = (event: Event) => {
      const collectionId = (event as CustomEvent<{ collectionId?: string }>).detail?.collectionId;
      if (collectionId) {
        setCustomCollectionModal({ mode: 'edit', collectionId });
      }
    };

    window.addEventListener('openCustomCollection', handleOpenCustomCollection);
    window.addEventListener('openCustomCollectionFromJSON', handleImportCustomCollection);
    window.addEventListener('editCustomCollection', handleEditCustomCollection);
    document.addEventListener('openCustomCollection', handleOpenCustomCollection);
    document.addEventListener('openCustomCollectionFromJSON', handleImportCustomCollection);
    document.addEventListener('editCustomCollection', handleEditCustomCollection);

    return () => {
      window.removeEventListener('openCustomCollection', handleOpenCustomCollection);
      window.removeEventListener('openCustomCollectionFromJSON', handleImportCustomCollection);
      window.removeEventListener('editCustomCollection', handleEditCustomCollection);
      document.removeEventListener('openCustomCollection', handleOpenCustomCollection);
      document.removeEventListener('openCustomCollectionFromJSON', handleImportCustomCollection);
      document.removeEventListener('editCustomCollection', handleEditCustomCollection);
    };
  }, []);

  useEffect(() => {
    const handleOpenRouterCollection = () => setRouterCollectionModal({ mode: 'create' });
    const handleImportRouterCollection = () => importRouterFileRef.current?.click();
    const handleEditRouterCollection = (event: Event) => {
      const collectionId = (event as CustomEvent<{ collectionId?: string }>).detail?.collectionId;
      if (collectionId) setRouterCollectionModal({ mode: 'edit', collectionId });
    };

    window.addEventListener('openRouterCollection', handleOpenRouterCollection);
    window.addEventListener('openRouterCollectionFromJSON', handleImportRouterCollection);
    window.addEventListener('editRouterCollection', handleEditRouterCollection);
    document.addEventListener('openRouterCollection', handleOpenRouterCollection);
    document.addEventListener('openRouterCollectionFromJSON', handleImportRouterCollection);
    document.addEventListener('editRouterCollection', handleEditRouterCollection);

    return () => {
      window.removeEventListener('openRouterCollection', handleOpenRouterCollection);
      window.removeEventListener('openRouterCollectionFromJSON', handleImportRouterCollection);
      window.removeEventListener('editRouterCollection', handleEditRouterCollection);
      document.removeEventListener('openRouterCollection', handleOpenRouterCollection);
      document.removeEventListener('openRouterCollectionFromJSON', handleImportRouterCollection);
      document.removeEventListener('editRouterCollection', handleEditRouterCollection);
    };
  }, []);

  const pullRegistration = async (requestBody: PullRegistrationPayload) => {
    const collectionComponents = Array.isArray(requestBody.components)
      ? requestBody.components
      : undefined;
    // An Omni collection whose components are all present needs no download -
    // only its definition gets registered. registrationOnly skips the Download
    // Manager/progress flow for that case. Require at least one component:
    // every() is vacuously true for an empty array, which would otherwise let a
    // malformed collection (missing/empty components) take the no-download path.
    const collectionFullyDownloaded = (requestBody.recipe === 'collection.omni' || requestBody.recipe === COLLECTION_ROUTER_MODEL_RECIPE) &&
      collectionComponents !== undefined &&
      collectionComponents.length > 0 &&
      collectionComponents.every((component) =>
        isModelEffectivelyDownloaded(component, modelsData[component], modelsData)
      );

    await pullModel(requestBody.model_name, {
      registrationData: requestBody as ModelRegistrationData,
      collectionComponents,
      declaredSizeGB: typeof requestBody.size === 'number' ? requestBody.size : undefined,
      registrationOnly: collectionFullyDownloaded,
    });
  };

  // ── Omni collection handlers ────────────────────────────────────────────

  const handleSaveCustomCollection = async (collection: CustomCollectionDraft) => {
    setCustomCollectionModal(null);
    try {
      const requestBody = buildCustomCollectionPullRequest(collection);
      await pullRegistration(requestBody);
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      await refreshModels();
      setSelectedModel(requestBody.model_name);
      setUserHasSelectedModel(true);
    } catch (error) {
      showError('Failed to save Omni Model: ' + (error instanceof Error ? error.message : 'Unknown error'));
    }
  };

  const handleExportCustomCollection = async (collection: CustomCollectionDraft) => {
    try {
      const request = buildCustomCollectionPullRequest(collection);
      const downloadPullBody = (message: string) => {
        showInfo(message);
        const bareName = request.model_name.replace(/^user\./, '');
        downloadJsonFile(`${bareName}.json`, request);
      };
      try {
        const { filename, payload } = await buildModelExportFile(request.model_name);
        const savedComponents = JSON.stringify(Array.isArray(payload.components) ? payload.components : []);
        const savedPrompt = typeof payload.system_prompt === 'string' ? payload.system_prompt : '';
        if (savedComponents !== JSON.stringify(request.components) || savedPrompt !== (request.system_prompt ?? '')) {
          downloadPullBody('Exporting your current edits (not saved yet). Create or save first for a fully portable export with embedded component definitions.');
          return;
        }
        downloadJsonFile(filename, payload);
      } catch (serverError) {
        const msg = serverError instanceof Error ? serverError.message : '';
        if (msg.includes('404') || msg.includes('not found') || msg.includes('HTTP 404')) {
          downloadPullBody('Omni Model not saved yet - downloading the registration JSON instead. Create or save first for a fully portable export.');
        } else {
          showError(msg || 'Failed to export Omni Model.');
        }
      }
    } catch (buildError) {
      showError(buildError instanceof Error ? buildError.message : 'Failed to build Omni Model JSON.');
    }
  };

  // ── Router collection handlers ───────────────────────────────────────────

  const handleSaveRouterCollection = async (collection: RouterCollectionDraft) => {
    try {
      const requestBody = buildRouterCollectionPullRequest(collection);
      await pullRegistration(requestBody);
      setRouterCollectionModal(null);
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      await refreshModels();
      setSelectedModel(requestBody.model_name);
      setUserHasSelectedModel(true);
    } catch (error) {
      showError('Failed to save Hybrid Router: ' + (error instanceof Error ? error.message : 'Unknown error'));
    }
  };

  const handleExportRouterCollection = async (collection: RouterCollectionDraft) => {
    try {
      const request = buildRouterCollectionPullRequest(collection);
      // The pull-body is the /v1/pull registration JSON. It won't carry embedded
      // component definitions, so it may need those models present on the target.
      const downloadPullBody = (message: string) => {
        // Informational - a file IS downloaded, just the less-portable variant.
        showInfo(message);
        const bareName = request.model_name.replace(/^user\./, '');
        downloadJsonFile(`${bareName}.json`, request);
      };
      try {
        const { filename, payload } = await buildModelExportFile(request.model_name);
        if (!routingBlocksEquivalent(payload.routing, request.routing)) {
          // The editor holds unsaved edits - exporting the server copy would
          // silently discard them. Export what the user sees instead.
          downloadPullBody('Exporting your current edits (not saved yet). Create or save first for a fully portable export with embedded component definitions.');
          return;
        }
        downloadJsonFile(filename, payload);
      } catch (serverError) {
        const msg = serverError instanceof Error ? serverError.message : '';
        if (msg.includes('404') || msg.includes('not found') || msg.includes('HTTP 404')) {
          // Model not saved yet - fall back to downloading the pull-body directly.
          downloadPullBody('Router not saved yet - downloading the registration JSON instead. Create or save first for a fully portable export.');
        } else {
          showError(msg || 'Failed to export Hybrid Router.');
        }
      }
    } catch (buildError) {
      showError(buildError instanceof Error ? buildError.message : 'Failed to build router JSON.');
    }
  };

  const handleImportRouterCollectionFile = (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;

    const reader = new FileReader();
    reader.onload = async (loadEvent) => {
      try {
        const parsed: unknown = JSON.parse(String(loadEvent.target?.result ?? ''));
        // Validate before hitting the server so errors are descriptive.
        const record = validateRouterImportPayload(parsed) as PullRegistrationPayload;
        await pullRegistration(record);
        await refreshModels();
        showSuccess(`Imported Hybrid Router ${getCollectionDisplayName(record.model_name)}.`);
      } catch (importError) {
        showError(importError instanceof Error ? importError.message : 'Failed to import Hybrid Router.');
      }
    };
    reader.onerror = () => showError('Failed to read the selected file.');
    reader.readAsText(file);
  };

  const handleImportCustomCollectionFile = (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;

    const reader = new FileReader();
    reader.onload = async (loadEvent) => {
      try {
        // Exported model/collection files are import-ready /pull bodies: hand
        // the whole file to the server, which registers any components from
        // the embedded `models` definitions and downloads what is missing.
        const parsed: unknown = JSON.parse(String(loadEvent.target?.result ?? ''));
        if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
          throw new Error('The selected file is not an exported model JSON.');
        }
        const record = parsed as PullRegistrationPayload;
        if (typeof record.model_name !== 'string' || record.model_name.length === 0) {
          throw new Error("The selected file is missing a 'model_name' field.");
        }
        if (typeof record.recipe !== 'string' || record.recipe.length === 0) {
          throw new Error("The selected file is missing a 'recipe' field.");
        }
        if (record.recipe === COLLECTION_ROUTER_MODEL_RECIPE) {
          // Router files get the structural checks plus version defaulting.
          Object.assign(record, validateRouterImportPayload(record));
        }
        await pullRegistration(record);
        await refreshModels();
        const kind = record.recipe === COLLECTION_ROUTER_MODEL_RECIPE ? 'Lemonade Router' : isCollectionRecipe(record.recipe) ? 'Omni Model' : 'model';
        showSuccess(`Imported ${kind} ${getCollectionDisplayName(record.model_name)}.`);
      } catch (importError) {
        showError(importError instanceof Error ? importError.message : 'Failed to import model.');
      }
    };
    reader.onerror = () => showError('Failed to read the selected file.');
    reader.readAsText(file);
  };

  const handleLeftDividerMouseDown = (e: React.MouseEvent) => {
    // preventDefault stops the WebKit-based webview (Tauri) from starting a
    // drag-text-selection on the divider, which would swallow our mousemove
    // events and break the resize. Chromium ignored this implicitly; WebKit
    // does not.
    e.preventDefault();
    isDraggingRef.current = 'left';
    startXRef.current = e.clientX;
    startWidthRef.current = modelManagerWidth;
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  };

  const handleRightDividerMouseDown = (e: React.MouseEvent) => {
    e.preventDefault();
    isDraggingRef.current = 'right';
    startXRef.current = e.clientX;
    startWidthRef.current = chatWidth;
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  };

  const handleCloseDownloadManager = useCallback(() => {
    suppressDownloadAutoOpenRef.current = true;
    setIsDownloadManagerVisible(false);
  }, []);

  const handleToggleDownloadManager = useCallback(() => {
    if (isDownloadManagerVisible) {
      suppressDownloadAutoOpenRef.current = true;
      setIsDownloadManagerVisible(false);
      return;
    }

    suppressDownloadAutoOpenRef.current = false;
    setIsDownloadManagerVisible(true);
    void downloadTracker.hydrateFromServer();
  }, [isDownloadManagerVisible]);

  return (
    <>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <TitleBar
        theme={theme}
        setTheme={setTheme}
        isChatVisible={isChatVisible}
        onToggleChat={() => setIsChatVisible(!isChatVisible)}
        isModelManagerVisible={isModelManagerVisible}
        onToggleModelManager={() => setIsModelManagerVisible(!isModelManagerVisible)}
        isLogsVisible={isLogsVisible}
        onToggleLogs={() => setIsLogsVisible(!isLogsVisible)}
        isDownloadManagerVisible={isDownloadManagerVisible}
        onToggleDownloadManager={handleToggleDownloadManager}
      />
      <DownloadManager
        isVisible={isDownloadManagerVisible}
        onClose={handleCloseDownloadManager}
      />
      <div className="app-layout">
        <ModelManager
          isContentVisible={isModelManagerVisible}
          onContentVisibilityChange={setIsModelManagerVisible}
          width={isModelManagerVisible ? modelManagerWidth : LAYOUT_CONSTANTS.experienceRailWidth}
          currentView={leftPanelView}
          onViewChange={setLeftPanelView}
        />
        {isModelManagerVisible && (isLogsVisible || isChatVisible) && (
          <ResizableDivider onMouseDown={handleLeftDividerMouseDown} />
        )}
        {isLogsVisible && (
          <div className="main-content-container">
            <LogsWindow
              isVisible={true}
              height={undefined}
            />
          </div>
        )}
        {isChatVisible && (
          <>
            {isLogsVisible && (
              <ResizableDivider onMouseDown={handleRightDividerMouseDown} />
            )}
            {externalContentUrl ? (
              <div className="chat-window" style={isLogsVisible ? { width: `${chatWidth}px` } : undefined}>
                <div className="external-content-container">
                  <div className="external-content-header">
                    <button className="external-content-back-btn" onClick={() => setExternalContentUrl(null)}>
                      <ChevronLeft size={14} />
                      Back to Chat
                    </button>
                  </div>
                  <iframe src={externalContentUrl} className="marketplace-iframe" />
                </div>
              </div>
            ) : (
              <ChatWindow
                isVisible={true}
                width={isLogsVisible ? chatWidth : undefined}
              />
            )}
          </>
        )}
      </div>
      <input
        ref={importCollectionFileRef}
        type="file"
        accept=".json,application/json"
        className="collection-import-input"
        onChange={handleImportCustomCollectionFile}
      />
      <input
        ref={importRouterFileRef}
        type="file"
        accept=".json,application/json"
        className="collection-import-input"
        onChange={handleImportRouterCollectionFile}
      />
      {customCollectionModal && createPortal(
        <div className="settings-overlay" onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => { if (e.target === e.currentTarget) { setCustomCollectionModal(null); } }}>
          <div className="settings-modal custom-collection-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <CustomCollectionPanel
              mode={customCollectionModal.mode}
              collectionId={customCollectionModal.collectionId}
              onClose={() => setCustomCollectionModal(null)}
              onSave={handleSaveCustomCollection}
              onExport={handleExportCustomCollection}
            />
          </div>
        </div>,
        document.body
      )}
      {routerCollectionModal && createPortal(
        <div className="settings-overlay" onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => { if (e.target === e.currentTarget) { setRouterCollectionModal(null); } }}>
          <div className="settings-modal router-collection-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <RouterCollectionPanel
              mode={routerCollectionModal.mode}
              collectionId={routerCollectionModal.collectionId}
              onClose={() => setRouterCollectionModal(null)}
              onSave={handleSaveRouterCollection}
              onExport={handleExportRouterCollection}
            />
          </div>
        </div>,
        document.body
      )}
      <StatusBar />
      <WindowResizeHandles />
    </>
  );
};

// Frameless windows on webkit2gtk (Tauri on Linux) get no resize handles from
// the OS. We paint 8 invisible regions on each edge and corner of the window;
// each one captures mousedown and asks Tauri to start a resize drag. Skipped
// in web mode and when window controls are unavailable for any other reason.
const WindowResizeHandles: React.FC = () => {
  if (typeof window === 'undefined' || !window.api?.startResizeDragging || window.api?.isWebApp) {
    return null;
  }
  const start = (direction: string) => (e: React.MouseEvent) => {
    // Only the primary button initiates a resize.
    if (e.button !== 0) return;
    e.preventDefault();
    window.api.startResizeDragging!(direction as never);
  };
  return (
    <div className="window-resize-handles" aria-hidden="true">
      <div className="resize-handle resize-handle-top" onMouseDown={start('Top')} />
      <div className="resize-handle resize-handle-right" onMouseDown={start('Right')} />
      <div className="resize-handle resize-handle-bottom" onMouseDown={start('Bottom')} />
      <div className="resize-handle resize-handle-left" onMouseDown={start('Left')} />
      <div className="resize-handle resize-handle-top-left" onMouseDown={start('TopLeft')} />
      <div className="resize-handle resize-handle-top-right" onMouseDown={start('TopRight')} />
      <div className="resize-handle resize-handle-bottom-left" onMouseDown={start('BottomLeft')} />
      <div className="resize-handle resize-handle-bottom-right" onMouseDown={start('BottomRight')} />
    </div>
  );
};

// Providers Wrapper
const App: React.FC = () => {
  return (
    <SystemProvider>
      <ModelsProvider>
        <AppContent />
      </ModelsProvider>
    </SystemProvider>
  );
};

export default App;
