import React, { useState, useRef, useEffect } from 'react';
import logo from '../../assets/logo.svg';
import SettingsModal from './SettingsModal';
import AboutModal from './AboutModal';

type MenuType = 'view' | 'help' | null;

interface TitleBarProps {
  isChatVisible: boolean;
  onToggleChat: () => void;
  isModelManagerVisible: boolean;
  onToggleModelManager: () => void;
  isCenterPanelVisible: boolean;
  onToggleCenterPanel: () => void;
  isLogsVisible: boolean;
  onToggleLogs: () => void;
}

const TitleBar: React.FC<TitleBarProps> = ({ 
  isChatVisible, 
  onToggleChat, 
  isModelManagerVisible, 
  onToggleModelManager,
  isCenterPanelVisible,
  onToggleCenterPanel,
  isLogsVisible,
  onToggleLogs
}) => {
  const [activeMenu, setActiveMenu] = useState<MenuType>(null);
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [isAboutOpen, setIsAboutOpen] = useState(false);
  const [isMaximized, setIsMaximized] = useState(false);
  const menuRef = useRef<HTMLDivElement>(null);
  const platform = window.api?.platform ?? navigator?.platform ?? '';
  const normalizedPlatform = platform.toLowerCase();
  const isMacPlatform = normalizedPlatform.includes('darwin') || normalizedPlatform.includes('mac');
  const isWindowsPlatform = normalizedPlatform.includes('win');
  const zoomInShortcutLabel = isMacPlatform ? '⌘ +' : isWindowsPlatform ? 'Ctrl Shift +' : 'Ctrl +';
  const zoomOutShortcutLabel = isMacPlatform ? '⌘ -' : 'Ctrl -';

  useEffect(() => {
    const handleClickOutside = (event: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(event.target as Node)) {
        setActiveMenu(null);
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

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
    // Handle keyboard shortcuts for View menu items
    const handleKeyDown = (event: KeyboardEvent) => {
      // Check for Ctrl+Shift combinations (or Cmd+Shift on Mac)
      if ((event.ctrlKey || event.metaKey) && event.shiftKey) {
        switch (event.key.toLowerCase()) {
          case 'm':
            event.preventDefault();
            onToggleModelManager();
            setActiveMenu(null);
            break;
          case 'p':
            event.preventDefault();
            onToggleCenterPanel();
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
  }, [onToggleModelManager, onToggleCenterPanel, onToggleChat, onToggleLogs]);

  const handleMenuClick = (menu: MenuType) => {
    setActiveMenu(activeMenu === menu ? null : menu);
  };

  const handleMenuItemClick = (action: string) => {
    console.log('Menu action:', action);
    setActiveMenu(null);
    // Add your menu action handlers here
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
        <div className="title-bar-left" ref={menuRef}>
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
                    <span>{isModelManagerVisible ? '✓ ' : ''}Model Manager</span>
                    <span className="menu-shortcut">Ctrl+Shift+M</span>
                  </div>
                  <div className="menu-option" onClick={() => { onToggleCenterPanel(); setActiveMenu(null); }}>
                    <span>{isCenterPanelVisible ? '✓ ' : ''}Center Panel</span>
                    <span className="menu-shortcut">Ctrl+Shift+P</span>
                  </div>
                  <div className="menu-option" onClick={() => { onToggleChat(); setActiveMenu(null); }}>
                    <span>{isChatVisible ? '✓ ' : ''}Chat Window</span>
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
                  <div className="menu-option" onClick={() => { setIsAboutOpen(prev => !prev); setActiveMenu(null); }}>
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
            className="title-bar-button settings"
            onClick={() => setIsSettingsOpen(true)}
            title="Settings"
          >
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg">
              <path d="M8 10C9.10457 10 10 9.10457 10 8C10 6.89543 9.10457 6 8 6C6.89543 6 6 6.89543 6 8C6 9.10457 6.89543 10 8 10Z" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
              <path d="M13 8C13 8.5 12.9 9 12.8 9.4L14 10.3C14.1 10.4 14.1 10.5 14.1 10.6L13 12.4C12.9 12.5 12.8 12.6 12.7 12.5L11.3 12C10.9 12.3 10.5 12.5 10 12.7L9.8 14.2C9.8 14.3 9.7 14.4 9.6 14.4H7.4C7.3 14.4 7.2 14.3 7.2 14.2L7 12.7C6.5 12.5 6.1 12.3 5.7 12L4.3 12.5C4.2 12.6 4.1 12.5 4 12.4L2.9 10.6C2.9 10.5 2.9 10.4 3 10.3L4.2 9.4C4.1 9 4 8.5 4 8C4 7.5 4.1 7 4.2 6.6L3 5.7C2.9 5.6 2.9 5.5 2.9 5.4L4 3.6C4.1 3.5 4.2 3.4 4.3 3.5L5.7 4C6.1 3.7 6.5 3.5 7 3.3L7.2 1.8C7.2 1.7 7.3 1.6 7.4 1.6H9.6C9.7 1.6 9.8 1.7 9.8 1.8L10 3.3C10.5 3.5 10.9 3.7 11.3 4L12.7 3.5C12.8 3.4 12.9 3.5 13 3.6L14.1 5.4C14.1 5.5 14.1 5.6 14 5.7L12.8 6.6C12.9 7 13 7.5 13 8Z" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
            </svg>
          </button>
          <button 
            className="title-bar-button minimize"
            onClick={() => window.api.minimizeWindow()}
            title="Minimize"
          >
            <svg width="12" height="12" viewBox="0 0 12 12">
              <rect x="0" y="5" width="12" height="1" fill="currentColor"/>
            </svg>
          </button>
          <button 
            className="title-bar-button maximize"
            onClick={() => window.api.maximizeWindow()}
            title={isMaximized ? "Restore Down" : "Maximize"}
          >
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
          <button 
            className="title-bar-button close"
            onClick={() => window.api.closeWindow()}
            title="Close"
          >
            <svg width="12" height="12" viewBox="0 0 12 12">
              <path d="M 1,1 L 11,11 M 11,1 L 1,11" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
            </svg>
          </button>
        </div>
      </div>
      <SettingsModal isOpen={isSettingsOpen} onClose={() => setIsSettingsOpen(false)} />
      <AboutModal isOpen={isAboutOpen} onClose={() => setIsAboutOpen(false)} />
    </>
  );
};

export default TitleBar;

