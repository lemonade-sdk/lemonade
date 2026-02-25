import React, { useState, useEffect } from 'react';
import logo from '../../assets/logo.svg';
import AboutModal from './AboutModal';

type MenuType = 'view' | 'help' | null;

interface TitleBarProps {
  isChatVisible: boolean;
  onToggleChat: () => void;
  isModelManagerVisible: boolean;
  onToggleModelManager: () => void;
  isLogsVisible: boolean;
  onToggleLogs: () => void;
  isDownloadManagerVisible: boolean;
  onToggleDownloadManager: () => void;
}

const TitleBar: React.FC<TitleBarProps> = ({
  isChatVisible,
  onToggleChat,
  isModelManagerVisible,
  onToggleModelManager,
  isLogsVisible,
  onToggleLogs,
  isDownloadManagerVisible,
  onToggleDownloadManager
}) => {
  const [activeMenu, setActiveMenu] = useState<MenuType>(null);
  const [isAboutOpen, setIsAboutOpen] = useState(false);
  const [isMaximized, setIsMaximized] = useState(false);
  const platform = window.api?.platform ?? navigator?.platform ?? '';
  const normalizedPlatform = platform.toLowerCase();
  const isMacPlatform = normalizedPlatform.includes('darwin') || normalizedPlatform.includes('mac');
  const isWindowsPlatform = normalizedPlatform.includes('win');
  const zoomInShortcutLabel = isMacPlatform ? '⌘ +' : isWindowsPlatform ? 'Ctrl Shift +' : 'Ctrl +';
  const zoomOutShortcutLabel = isMacPlatform ? '⌘ -' : 'Ctrl -';
  const isWebApp = window.api?.isWebApp === true;

  useEffect(() => {
    if (!window.api?.onMaximizeChange) {
      console.warn('window.api.onMaximizeChange is unavailable. Running outside Electron?');
      return;
    }

    const handleMaximizeChange = (maximized: boolean) => {
      setIsMaximized(maximized);
    };

    window.api.onMaximizeChange(handleMaximizeChange);
  }, []);

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.shiftKey) {
        switch (event.key.toLowerCase()) {
          case 'm':
            event.preventDefault();
            onToggleModelManager();
            setActiveMenu(null);
            break;
          case 'h':
            event.preventDefault();
            onToggleChat();
            setActiveMenu(null);
            break;
          case 'l':
            event.preventDefault();
            onToggleLogs();
            setActiveMenu(null);
            break;
        }
      }
    };

    document.addEventListener('keydown', handleKeyDown);
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [onToggleModelManager, onToggleChat, onToggleLogs]);

  const handleMenuClick = (menu: MenuType) => {
    setActiveMenu(activeMenu === menu ? null : menu);
  };

  const handleZoom = (direction: 'in' | 'out') => {
    if (!window.api?.zoomIn || !window.api?.zoomOut) {
      console.warn('Zoom controls are unavailable outside Electron.');
      setActiveMenu(null);
      return;
    }

    if (direction === 'in') {
      window.api.zoomIn();
    } else {
      window.api.zoomOut();
    }

    setActiveMenu(null);
  };

  return (
    <>
      <div className="title-bar">
        <div className="title-bar-left">
          <img src={logo} alt="Lemonade" className="title-bar-logo" />
          <div className="menu-items">
            <div className="menu-item-wrapper">
              <span
                className={`menu-item ${activeMenu === 'view' ? 'active' : ''}`}
                onClick={() => handleMenuClick('view')}
              >
                View
              </span>
              {activeMenu === 'view' && (
                <div className="menu-dropdown">
                  <div className="menu-option" onClick={() => { onToggleModelManager(); setActiveMenu(null); }}>
                    <span>{isModelManagerVisible ? '✓ ' : ''}Left Panel</span>
                    <span className="menu-shortcut">Ctrl+Shift+M</span>
                  </div>
                  <div className="menu-option" onClick={() => { onToggleChat(); setActiveMenu(null); }}>
                    <span>{isChatVisible ? '✓ ' : ''}Chat</span>
                    <span className="menu-shortcut">Ctrl+Shift+H</span>
                  </div>
                  <div className="menu-option" onClick={() => { onToggleLogs(); setActiveMenu(null); }}>
                    <span>{isLogsVisible ? '✓ ' : ''}Logs</span>
                    <span className="menu-shortcut">Ctrl+Shift+L</span>
                  </div>
                  <div className="menu-separator"></div>
                  <div className="menu-option" onClick={() => handleZoom('in')}>
                    <span>Zoom In</span>
                    <span className="menu-shortcut">{zoomInShortcutLabel}</span>
                  </div>
                  <div className="menu-option" onClick={() => handleZoom('out')}>
                    <span>Zoom Out</span>
                    <span className="menu-shortcut">{zoomOutShortcutLabel}</span>
                  </div>
                </div>
              )}
            </div>
            <div className="menu-item-wrapper">
              <span
                className={`menu-item ${activeMenu === 'help' ? 'active' : ''}`}
                onClick={() => handleMenuClick('help')}
              >
                Help
              </span>
              {activeMenu === 'help' && (
                <div className="menu-dropdown">
                  <div className="menu-option" onClick={() => { window.api.openExternal('https://lemonade-server.ai/docs/'); setActiveMenu(null); }}>
                    Documentation
                  </div>
                  <div className="menu-option" onClick={() => { window.api.openExternal('https://github.com/lemonade-sdk/lemonade/releases'); setActiveMenu(null); }}>
                    Release Notes
                  </div>
                  <div className="menu-separator"></div>
                  <div className="menu-option" onClick={() => { setIsAboutOpen(true); setActiveMenu(null); }}>
                    About
                  </div>
                </div>
              )}
            </div>
          </div>
        </div>
        <div className="title-bar-center">
          <span className="app-title">Lemonade</span>
        </div>
        <div className="title-bar-right">
          <button
            className={`title-bar-button downloads ${isDownloadManagerVisible ? 'active' : ''}`}
            onClick={onToggleDownloadManager}
            title="Downloads"
          >
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg">
              <path d="M14 10V12.6667C14 13.0203 13.8595 13.3594 13.6095 13.6095C13.3594 13.8595 13.0203 14 12.6667 14H3.33333C2.97971 14 2.64057 13.8595 2.39052 13.6095C2.14048 13.3594 2 13.0203 2 12.6667V10" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
              <path d="M4.66675 6.66667L8.00008 10L11.3334 6.66667" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
              <path d="M8 10V2" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
            </svg>
          </button>
          {!isWebApp && (
            <>
              <button className="title-bar-button minimize" onClick={() => window.api.minimizeWindow()} title="Minimize">
                <svg width="12" height="12" viewBox="0 0 12 12">
                  <rect x="0" y="5" width="12" height="1" fill="currentColor"/>
                </svg>
              </button>
              <button className="title-bar-button maximize" onClick={() => window.api.maximizeWindow()} title={isMaximized ? "Restore Down" : "Maximize"}>
                {isMaximized ? (
                  <svg width="12" height="12" viewBox="0 0 12 12">
                    <rect x="2.5" y="0.5" width="9" height="9" fill="none" stroke="currentColor" strokeWidth="1"/>
                    <rect x="0.5" y="2.5" width="9" height="9" fill="black" stroke="currentColor" strokeWidth="1"/>
                  </svg>
                ) : (
                  <svg width="12" height="12" viewBox="0 0 12 12">
                    <rect x="0.5" y="0.5" width="11" height="11" fill="none" stroke="currentColor" strokeWidth="1"/>
                  </svg>
                )}
              </button>
              <button className="title-bar-button close" onClick={() => window.api.closeWindow()} title="Close">
                <svg width="12" height="12" viewBox="0 0 12 12">
                  <path d="M 1,1 L 11,11 M 11,1 L 1,11" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
                </svg>
              </button>
            </>
          )}
        </div>
      </div>
      <AboutModal isOpen={isAboutOpen} onClose={() => setIsAboutOpen(false)} />
    </>
  );
};

export default TitleBar;
