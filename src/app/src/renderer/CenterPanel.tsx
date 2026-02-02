import React, { useState, useEffect, useRef } from 'react';

interface CenterPanelProps {
  isVisible: boolean;
  onClose?: () => void;
}

// Marketplace URL with embedded mode and dark theme
const MARKETPLACE_URL = 'https://lemonade-server.ai/marketplace?embedded=true&theme=dark';

// Fallback URL for checking connectivity
const CONNECTIVITY_CHECK_URL = 'https://lemonade-server.ai/favicon.ico';

const CenterPanel: React.FC<CenterPanelProps> = ({ isVisible, onClose }) => {
  const [isOffline, setIsOffline] = useState(false);
  const [isLoading, setIsLoading] = useState(true);
  const [hasError, setHasError] = useState(false);
  const iframeRef = useRef<HTMLIFrameElement>(null);

  // Check connectivity on mount and when visibility changes
  useEffect(() => {
    if (!isVisible) return;

    const checkConnectivity = async () => {
      setIsLoading(true);
      setHasError(false);

      try {
        // Try to fetch a small resource to check connectivity
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 5000);

        await fetch(CONNECTIVITY_CHECK_URL, {
          mode: 'no-cors',
          signal: controller.signal,
        });

        clearTimeout(timeoutId);
        setIsOffline(false);
      } catch (error) {
        console.warn('Marketplace connectivity check failed:', error);
        setIsOffline(true);
      } finally {
        setIsLoading(false);
      }
    };

    checkConnectivity();
  }, [isVisible]);

  // Handle iframe load events
  const handleIframeLoad = () => {
    setIsLoading(false);
    setHasError(false);
  };

  const handleIframeError = () => {
    setIsLoading(false);
    setHasError(true);
  };

  // Retry loading
  const handleRetry = () => {
    setIsLoading(true);
    setHasError(false);
    setIsOffline(false);

    // Force iframe reload
    if (iframeRef.current) {
      iframeRef.current.src = MARKETPLACE_URL;
    }
  };

  // Open in browser
  const handleOpenInBrowser = () => {
    window.open('https://lemonade-server.ai/marketplace', '_blank', 'noopener,noreferrer');
  };

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

      {/* Offline/Error State */}
      {(isOffline || hasError) && !isLoading && (
        <div className="marketplace-offline">
          <div className="offline-icon">📡</div>
          <h2>App Marketplace</h2>
          <p className="offline-message">
            {isOffline
              ? "Unable to connect to the marketplace. Please check your internet connection."
              : "Something went wrong loading the marketplace."}
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
      {!isOffline && !hasError && (
        <iframe
          ref={iframeRef}
          src={MARKETPLACE_URL}
          className={`marketplace-iframe ${isLoading ? 'loading' : ''}`}
          onLoad={handleIframeLoad}
          onError={handleIframeError}
          title="App Marketplace"
          sandbox="allow-scripts allow-same-origin allow-popups allow-popups-to-escape-sandbox"
        />
      )}
    </div>
  );
};

export default CenterPanel;
