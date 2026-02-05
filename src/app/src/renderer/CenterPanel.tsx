import React, { useState, useEffect, useRef, memo, useCallback } from 'react';

interface CenterPanelProps {
  isVisible: boolean;
  onClose?: () => void;
}

// Remote marketplace URL
const REMOTE_MARKETPLACE_URL = 'https://lemonade-server.ai/marketplace?embedded=true&theme=dark';

const CenterPanel: React.FC<CenterPanelProps> = memo(({ isVisible, onClose }) => {
  const [marketplaceUrl, setMarketplaceUrl] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const [hasError, setHasError] = useState(false);
  const iframeRef = useRef<HTMLIFrameElement>(null);

  // Determine which URL to use on mount
  useEffect(() => {
    if (!isVisible) return;

    const determineUrl = async () => {
      setIsLoading(true);
      setHasError(false);

      try {
        // Try to fetch the remote marketplace page
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
        console.warn('Remote marketplace unavailable, falling back to local:', error);

        // Get local marketplace URL via Electron API
        if (window.api?.getLocalMarketplaceUrl) {
          const localUrl = await window.api.getLocalMarketplaceUrl();
          if (localUrl) {
            console.log('Using local marketplace URL:', localUrl);
            setMarketplaceUrl(localUrl);
          } else {
            console.error('Local marketplace file not found');
            setHasError(true);
            setIsLoading(false);
          }
        } else {
          // Fallback for non-Electron environment (shouldn't happen)
          console.error('Electron API not available');
          setHasError(true);
          setIsLoading(false);
        }
      }
    };

    determineUrl();
  }, [isVisible]);

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

    // Re-trigger URL determination by trying remote first
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
      // Fall back to local
      if (window.api?.getLocalMarketplaceUrl) {
        const localUrl = await window.api.getLocalMarketplaceUrl();
        if (localUrl) {
          setMarketplaceUrl(localUrl);
        } else {
          setHasError(true);
          setIsLoading(false);
        }
      } else {
        setHasError(true);
        setIsLoading(false);
      }
    }
  };

  // Open in browser
  const handleOpenInBrowser = useCallback(() => {
    window.open('https://lemonade-server.ai/marketplace', '_blank', 'noopener,noreferrer');
  }, []);

  if (!isVisible) return null;

  return (
    <div className="center-panel">
      {onClose && (
        <button
          className="center-panel-close-btn"
          onClick={onClose}
          title="Close panel"
        >
          ×
        </button>
      )}

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
    </div>
  );
});

CenterPanel.displayName = 'CenterPanel';

export default CenterPanel;
