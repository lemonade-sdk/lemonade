import React, { useState, useEffect, useRef, memo, useCallback } from 'react';
import CenterPanelMenu from './CenterPanelMenu';

export type CenterPanelView = 'menu' | 'marketplace';

interface CenterPanelProps {
  isVisible: boolean;
  currentView: CenterPanelView;
  onViewChange: (view: CenterPanelView) => void;
  onClose?: () => void;
}

// Remote marketplace URL
const REMOTE_MARKETPLACE_URL = 'https://lemonade-server.ai/marketplace?embedded=true&theme=dark';

const CenterPanel: React.FC<CenterPanelProps> = memo(({ isVisible, currentView, onViewChange, onClose }) => {
  const [marketplaceUrl, setMarketplaceUrl] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [hasError, setHasError] = useState(false);
  const iframeRef = useRef<HTMLIFrameElement>(null);

  // Determine which URL to use when marketplace view is shown
  useEffect(() => {
    if (!isVisible || currentView !== 'marketplace') return;
    if (marketplaceUrl) return; // Already loaded

    const determineUrl = async () => {
      setIsLoading(true);
      setHasError(false);

      // Try local marketplace first, fall back to remote
      if (window.api?.getLocalMarketplaceUrl) {
        const localUrl = await window.api.getLocalMarketplaceUrl();
        if (localUrl) {
          console.log('Using local marketplace URL:', localUrl);
          setMarketplaceUrl(localUrl);
          return;
        }
      }

      // Fall back to remote marketplace
      try {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 3000);

        const response = await fetch(REMOTE_MARKETPLACE_URL, {
          method: 'HEAD',
          signal: controller.signal,
        });

        clearTimeout(timeoutId);

        if (response.ok) {
          console.log('Using remote marketplace URL');
          setMarketplaceUrl(REMOTE_MARKETPLACE_URL);
        } else {
          throw new Error(`HTTP ${response.status}`);
        }
      } catch (error) {
        console.warn('Remote marketplace also unavailable:', error);
        setHasError(true);
        setIsLoading(false);
      }
    };

    determineUrl();
  }, [isVisible, currentView, marketplaceUrl]);

  // Handle iframe load events
  const handleIframeLoad = useCallback(() => {
    setIsLoading(false);
    setHasError(false);
  }, []);

  const handleIframeError = useCallback(() => {
    setIsLoading(false);
    setHasError(true);
  }, []);

  // Retry loading
  const handleRetry = async () => {
    setIsLoading(true);
    setHasError(false);
    setMarketplaceUrl(null);

    // Try local first, fall back to remote
    if (window.api?.getLocalMarketplaceUrl) {
      const localUrl = await window.api.getLocalMarketplaceUrl();
      if (localUrl) {
        setMarketplaceUrl(localUrl);
        return;
      }
    }

    try {
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 3000);

      const response = await fetch(REMOTE_MARKETPLACE_URL, {
        method: 'HEAD',
        signal: controller.signal,
      });

      clearTimeout(timeoutId);

      if (response.ok) {
        setMarketplaceUrl(REMOTE_MARKETPLACE_URL);
      } else {
        throw new Error(`HTTP ${response.status}`);
      }
    } catch {
      setHasError(true);
      setIsLoading(false);
    }
  };

  // Open in browser
  const handleOpenInBrowser = useCallback(() => {
    window.open('https://lemonade-server.ai/marketplace', '_blank', 'noopener,noreferrer');
  }, []);

  // Handle opening marketplace from menu
  const handleOpenMarketplace = useCallback(() => {
    onViewChange('marketplace');
  }, [onViewChange]);

  // Handle going back to menu from marketplace
  const handleBackToMenu = useCallback(() => {
    onViewChange('menu');
  }, [onViewChange]);

  if (!isVisible) return null;

  return (
    <div className="center-panel">
      {/* Close button on menu view */}
      {currentView === 'menu' && onClose && (
        <button
          className="center-panel-close-btn"
          onClick={onClose}
          title="Close panel"
        >
          ×
        </button>
      )}

      {/* Menu View */}
      {currentView === 'menu' && (
        <CenterPanelMenu onOpenMarketplace={handleOpenMarketplace} />
      )}

      {/* Marketplace View */}
      {currentView === 'marketplace' && (
        <>
          {/* Back button header */}
          <div className="center-panel-back-header">
            <button
              className="center-panel-back-btn"
              onClick={handleBackToMenu}
              title="Back to menu"
            >
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <line x1="19" y1="12" x2="5" y2="12"/>
                <polyline points="12 19 5 12 12 5"/>
              </svg>
            </button>
          </div>

          {/* Loading State */}
          {isLoading && (
            <div className="marketplace-loading">
              <div className="marketplace-loading-spinner"></div>
              <p>Loading Marketplace...</p>
            </div>
          )}

          {/* Error State */}
          {hasError && !isLoading && (
            <div className="marketplace-offline">
              <div className="offline-icon">⚠️</div>
              <h2>App Marketplace</h2>
              <p className="offline-message">
                Something went wrong loading the marketplace.
              </p>
              <div className="offline-actions">
                <button className="offline-btn primary" onClick={handleRetry}>
                  Try Again
                </button>
                <button className="offline-btn secondary" onClick={handleOpenInBrowser}>
                  Open in Browser
                </button>
              </div>
            </div>
          )}

          {/* Marketplace iframe */}
          {marketplaceUrl && !hasError && (
            <iframe
              ref={iframeRef}
              src={marketplaceUrl}
              className={`marketplace-iframe ${isLoading ? 'loading' : ''}`}
              onLoad={handleIframeLoad}
              onError={handleIframeError}
              title="App Marketplace"
              sandbox="allow-scripts allow-same-origin allow-popups allow-popups-to-escape-sandbox"
              loading="lazy"
            />
          )}
        </>
      )}
    </div>
  );
});

CenterPanel.displayName = 'CenterPanel';

export default CenterPanel;
