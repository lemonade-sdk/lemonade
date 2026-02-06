import React, { useState, useEffect, useRef, useCallback } from 'react';
import TitleBar from './TitleBar';
import ChatWindow from './ChatWindow';
import ModelManager from './ModelManager';
import LogsWindow from './LogsWindow';
import CenterPanel, { CenterPanelView } from './CenterPanel';
import ResizableDivider from './ResizableDivider';
import DownloadManager from './DownloadManager';
import StatusBar from './StatusBar';
import { ModelsProvider } from './hooks/useModels';
import { SystemProvider } from './hooks/useSystem';
import { DEFAULT_LAYOUT_SETTINGS } from './utils/appSettings';
import '../../styles.css';

const LAYOUT_CONSTANTS = {
  modelManagerMinWidth: 200,
  mainContentMinWidth: 300,
  chatMinWidth: 250,
  dividerWidth: 4,
  absoluteMinWidth: 400,
};

const App: React.FC = () => {
  const [isChatVisible, setIsChatVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isChatVisible);
  const [isModelManagerVisible, setIsModelManagerVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isModelManagerVisible);
  const [isCenterPanelVisible, setIsCenterPanelVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isCenterPanelVisible_v2);
  const [centerPanelView, setCenterPanelView] = useState<CenterPanelView>('menu');
  const [isLogsVisible, setIsLogsVisible] = useState(DEFAULT_LAYOUT_SETTINGS.isLogsVisible);
  const [isDownloadManagerVisible, setIsDownloadManagerVisible] = useState(false);
  const [modelManagerWidth, setModelManagerWidth] = useState(DEFAULT_LAYOUT_SETTINGS.modelManagerWidth);
  const [chatWidth, setChatWidth] = useState(DEFAULT_LAYOUT_SETTINGS.chatWidth);
  const [logsHeight, setLogsHeight] = useState(DEFAULT_LAYOUT_SETTINGS.logsHeight);
  const [layoutLoaded, setLayoutLoaded] = useState(false);
  const isDraggingRef = useRef<'left' | 'right' | 'bottom' | null>(null);
  const startXRef = useRef(0);
  const startYRef = useRef(0);
  const startWidthRef = useRef(0);
  const startHeightRef = useRef(0);

  // Load saved layout settings on mount
  useEffect(() => {
    const loadLayoutSettings = async () => {
      try {
        if (window?.api?.getSettings) {
          const settings = await window.api.getSettings();
          if (settings.layout) {
            setIsChatVisible(settings.layout.isChatVisible ?? DEFAULT_LAYOUT_SETTINGS.isChatVisible);
            setIsModelManagerVisible(settings.layout.isModelManagerVisible ?? DEFAULT_LAYOUT_SETTINGS.isModelManagerVisible);
            // Use nullish coalescing to default to true for new v2 setting
            setIsCenterPanelVisible(settings.layout.isCenterPanelVisible_v2 ?? DEFAULT_LAYOUT_SETTINGS.isCenterPanelVisible_v2);
            setIsLogsVisible(settings.layout.isLogsVisible ?? DEFAULT_LAYOUT_SETTINGS.isLogsVisible);
            setModelManagerWidth(settings.layout.modelManagerWidth ?? DEFAULT_LAYOUT_SETTINGS.modelManagerWidth);
            setChatWidth(settings.layout.chatWidth ?? DEFAULT_LAYOUT_SETTINGS.chatWidth);
            setLogsHeight(settings.layout.logsHeight ?? DEFAULT_LAYOUT_SETTINGS.logsHeight);
          }
        }
      } catch (error) {
        console.error('Failed to load layout settings:', error);
      } finally {
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
            isChatVisible,
            isModelManagerVisible,
            isCenterPanelVisible_v2: isCenterPanelVisible,
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
  }, [layoutLoaded, isChatVisible, isModelManagerVisible, isCenterPanelVisible, isLogsVisible, modelManagerWidth, chatWidth, logsHeight]);

  // Debounced save effect
  useEffect(() => {
    if (!layoutLoaded) return;
    const timeoutId = setTimeout(saveLayoutSettings, 300);
    return () => clearTimeout(timeoutId);
  }, [saveLayoutSettings, layoutLoaded]);

  // Listen for download start events to automatically open download manager
  // and download completion events from chat to close it
  useEffect(() => {
    const handleDownloadStart = () => {
      setIsDownloadManagerVisible(true);
    };

    // When a chat-initiated download completes, minimize the download manager
    const handleChatDownloadComplete = () => {
      setIsDownloadManagerVisible(false);
    };

    window.addEventListener('download:started' as any, handleDownloadStart);
    window.addEventListener('download:chatComplete' as any, handleChatDownloadComplete);

    return () => {
      window.removeEventListener('download:started' as any, handleDownloadStart);
      window.removeEventListener('download:chatComplete' as any, handleChatDownloadComplete);
    };
  }, []);

  useEffect(() => {
    const hasMainColumn = isCenterPanelVisible || isLogsVisible;
    let computedMinWidth = 0;

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
    if (isModelManagerVisible && hasMainColumn) {
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
  }, [isModelManagerVisible, isCenterPanelVisible, isLogsVisible, isChatVisible]);

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

        const leftPanelWidth = isModelManagerVisible ? modelManagerWidth : 0;
        const hasCenterColumn = isCenterPanelVisible || isLogsVisible;
        const minCenterWidth = hasCenterColumn ? 300 : 0; // keep in sync with CSS min-width

        // Account for vertical dividers (each 4px wide)
        const dividerCount =
          (isModelManagerVisible ? 1 : 0) +
          (hasCenterColumn && isChatVisible ? 1 : 0);
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

    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);

    return () => {
      document.removeEventListener('mousemove', handleMouseMove);
      document.removeEventListener('mouseup', handleMouseUp);
    };
  }, [
    chatWidth,
    isCenterPanelVisible,
    isChatVisible,
    isModelManagerVisible,
    isLogsVisible,
    modelManagerWidth,
    logsHeight,
  ]);

  const handleLeftDividerMouseDown = (e: React.MouseEvent) => {
    isDraggingRef.current = 'left';
    startXRef.current = e.clientX;
    startWidthRef.current = modelManagerWidth;
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  };

  const handleRightDividerMouseDown = (e: React.MouseEvent) => {
    isDraggingRef.current = 'right';
    startXRef.current = e.clientX;
    startWidthRef.current = chatWidth;
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  };

  const handleBottomDividerMouseDown = (e: React.MouseEvent) => {
    isDraggingRef.current = 'bottom';
    startYRef.current = e.clientY;
    startHeightRef.current = logsHeight;
    document.body.style.cursor = 'row-resize';
    document.body.style.userSelect = 'none';
  };

  // Memoized callbacks for child components to prevent re-renders during resize
  const handleCloseCenterPanel = useCallback(() => {
    setIsCenterPanelVisible(false);
  }, []);

  const handleCenterPanelViewChange = useCallback((view: CenterPanelView) => {
    setCenterPanelView(view);
  }, []);

  // Toggle center panel visibility
  const handleToggleCenterPanel = useCallback(() => {
    setIsCenterPanelVisible(prev => !prev);
  }, []);

  // Open marketplace directly (from View menu)
  const handleOpenMarketplace = useCallback(() => {
    setIsCenterPanelVisible(true);
    setCenterPanelView('marketplace');
  }, []);

  const handleCloseDownloadManager = useCallback(() => {
    setIsDownloadManagerVisible(false);
  }, []);

  return (
    <SystemProvider>
      <ModelsProvider>
        <TitleBar
          isChatVisible={isChatVisible}
          onToggleChat={() => setIsChatVisible(!isChatVisible)}
          isModelManagerVisible={isModelManagerVisible}
          onToggleModelManager={() => setIsModelManagerVisible(!isModelManagerVisible)}
          isCenterPanelVisible={isCenterPanelVisible}
          onToggleCenterPanel={handleToggleCenterPanel}
          centerPanelView={centerPanelView}
          onOpenMarketplace={handleOpenMarketplace}
          isLogsVisible={isLogsVisible}
          onToggleLogs={() => setIsLogsVisible(!isLogsVisible)}
          isDownloadManagerVisible={isDownloadManagerVisible}
          onToggleDownloadManager={() => setIsDownloadManagerVisible(!isDownloadManagerVisible)}
        />
        <DownloadManager
          isVisible={isDownloadManagerVisible}
          onClose={handleCloseDownloadManager}
        />
        <div className="app-layout">
          {isModelManagerVisible && (
            <>
              <ModelManager isVisible={true} width={modelManagerWidth}/>
              <ResizableDivider onMouseDown={handleLeftDividerMouseDown}/>
            </>
          )}
          {(isCenterPanelVisible || isLogsVisible) && (
            <div className="main-content-container">
              {isCenterPanelVisible && (
                <div className={`main-content ${isChatVisible ? 'with-chat' : 'full-width'} ${isModelManagerVisible ? 'with-model-manager' : ''}`}>
                  <CenterPanel
                    isVisible={true}
                    currentView={centerPanelView}
                    onViewChange={handleCenterPanelViewChange}
                    onClose={handleCloseCenterPanel}
                  />
                </div>
              )}
              {isCenterPanelVisible && isLogsVisible && (
                <ResizableDivider
                  onMouseDown={handleBottomDividerMouseDown}
                  orientation="horizontal"
                />
              )}
              {isLogsVisible && (
                <LogsWindow
                  isVisible={true}
                  height={isCenterPanelVisible ? logsHeight : undefined}
                />
              )}
            </div>
          )}
          {isChatVisible && (
            <>
              {(isCenterPanelVisible || isLogsVisible) && (
                <ResizableDivider onMouseDown={handleRightDividerMouseDown}/>
              )}
              <ChatWindow
                isVisible={true}
                width={(isCenterPanelVisible || isLogsVisible) ? chatWidth : undefined}
              />
            </>
          )}
        </div>
        <StatusBar />
      </ModelsProvider>
    </SystemProvider>
  );
};

export default App;
