import React, { useState, useEffect, useRef, useCallback } from 'react';
import TitleBar from './TitleBar';
import ChatWindow from './ChatWindow';
import ModelManager from './ModelManager';
import LogsWindow from './LogsWindow';
import CenterPanel from './CenterPanel';
import ResizableDivider from './ResizableDivider';
import DownloadManager from './DownloadManager';
import '../../styles.css';

const LAYOUT_CONSTANTS = {
  modelManagerMinWidth: 200,
  mainContentMinWidth: 300,
  chatMinWidth: 250,
  dividerWidth: 4,
  absoluteMinWidth: 400,
};

const App: React.FC = () => {
  const [isChatVisible, setIsChatVisible] = useState(true);
  const [isModelManagerVisible, setIsModelManagerVisible] = useState(true);
  const [isCenterPanelVisible, setIsCenterPanelVisible] = useState(true);
  const [isLogsVisible, setIsLogsVisible] = useState(false);
  const [isDownloadManagerVisible, setIsDownloadManagerVisible] = useState(false);
  const [modelManagerWidth, setModelManagerWidth] = useState(280);
  const [chatWidth, setChatWidth] = useState(350);
  const [logsHeight, setLogsHeight] = useState(200);
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
            setIsChatVisible(settings.layout.isChatVisible);
            setIsModelManagerVisible(settings.layout.isModelManagerVisible);
            setIsCenterPanelVisible(settings.layout.isCenterPanelVisible);
            setIsLogsVisible(settings.layout.isLogsVisible);
            setModelManagerWidth(settings.layout.modelManagerWidth);
            setChatWidth(settings.layout.chatWidth);
            setLogsHeight(settings.layout.logsHeight);
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
            isCenterPanelVisible,
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
  useEffect(() => {
    const handleDownloadStart = () => {
      setIsDownloadManagerVisible(true);
    };

    window.addEventListener('download:started' as any, handleDownloadStart);

    return () => {
      window.removeEventListener('download:started' as any, handleDownloadStart);
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

  return (
    <>
      <TitleBar 
        isChatVisible={isChatVisible}
        onToggleChat={() => setIsChatVisible(!isChatVisible)}
        isModelManagerVisible={isModelManagerVisible}
        onToggleModelManager={() => setIsModelManagerVisible(!isModelManagerVisible)}
        isCenterPanelVisible={isCenterPanelVisible}
        onToggleCenterPanel={() => setIsCenterPanelVisible(!isCenterPanelVisible)}
        isLogsVisible={isLogsVisible}
        onToggleLogs={() => setIsLogsVisible(!isLogsVisible)}
        isDownloadManagerVisible={isDownloadManagerVisible}
        onToggleDownloadManager={() => setIsDownloadManagerVisible(!isDownloadManagerVisible)}
      />
      <DownloadManager 
        isVisible={isDownloadManagerVisible}
        onClose={() => setIsDownloadManagerVisible(false)}
      />
      <div className="app-layout">
        {isModelManagerVisible && (
          <>
            <ModelManager isVisible={true} width={modelManagerWidth} />
            <ResizableDivider onMouseDown={handleLeftDividerMouseDown} />
          </>
        )}
        {(isCenterPanelVisible || isLogsVisible) && (
          <div className="main-content-container">
            {isCenterPanelVisible && (
              <div className={`main-content ${isChatVisible ? 'with-chat' : 'full-width'} ${isModelManagerVisible ? 'with-model-manager' : ''}`}>
                <CenterPanel isVisible={true} onClose={() => setIsCenterPanelVisible(false)} />
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
              <ResizableDivider onMouseDown={handleRightDividerMouseDown} />
            )}
            <ChatWindow 
              isVisible={true} 
              width={(isCenterPanelVisible || isLogsVisible) ? chatWidth : undefined}
            />
          </>
        )}
      </div>
    </>
  );
};

export default App;

