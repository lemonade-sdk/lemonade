import React, { useState, useEffect } from 'react';

export interface DownloadItem {
  id: string;
  modelName: string;
  fileName: string;
  fileIndex: number;
  totalFiles: number;
  bytesDownloaded: number;
  bytesTotal: number;
  percent: number;
  status: 'downloading' | 'paused' | 'completed' | 'error' | 'cancelled';
  error?: string;
  startTime: number;
  abortController?: AbortController;
}

interface DownloadManagerProps {
  isVisible: boolean;
  onClose: () => void;
}

const DownloadManager: React.FC<DownloadManagerProps> = ({ isVisible, onClose }) => {
  const [downloads, setDownloads] = useState<DownloadItem[]>([]);
  const [expandedDownloads, setExpandedDownloads] = useState<Set<string>>(new Set());

  useEffect(() => {
    // Listen for download events from the global download tracker
    const handleDownloadUpdate = (event: CustomEvent<DownloadItem>) => {
      const downloadItem = event.detail;
      setDownloads(prev => {
        const existingIndex = prev.findIndex(d => d.id === downloadItem.id);
        if (existingIndex >= 0) {
          const newDownloads = [...prev];
          newDownloads[existingIndex] = downloadItem;
          return newDownloads;
        } else {
          return [downloadItem, ...prev];
        }
      });
    };

    const handleDownloadComplete = (event: CustomEvent<{ id: string }>) => {
      const { id } = event.detail;
      setDownloads(prev => prev.map(d => 
        d.id === id ? { ...d, status: 'completed' as const, percent: 100 } : d
      ));
    };

    const handleDownloadError = (event: CustomEvent<{ id: string; error: string }>) => {
      const { id, error } = event.detail;
      setDownloads(prev => prev.map(d => 
        d.id === id ? { ...d, status: 'error' as const, error } : d
      ));
    };

    window.addEventListener('download:update' as any, handleDownloadUpdate);
    window.addEventListener('download:complete' as any, handleDownloadComplete);
    window.addEventListener('download:error' as any, handleDownloadError);

    return () => {
      window.removeEventListener('download:update' as any, handleDownloadUpdate);
      window.removeEventListener('download:complete' as any, handleDownloadComplete);
      window.removeEventListener('download:error' as any, handleDownloadError);
    };
  }, []);

  const formatBytes = (bytes: number): string => {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${(bytes / Math.pow(k, i)).toFixed(2)} ${sizes[i]}`;
  };

  const formatSpeed = (bytesPerSecond: number): string => {
    return `${formatBytes(bytesPerSecond)}/s`;
  };

  const calculateSpeed = (download: DownloadItem): number => {
    const elapsedSeconds = (Date.now() - download.startTime) / 1000;
    if (elapsedSeconds === 0) return 0;
    return download.bytesDownloaded / elapsedSeconds;
  };

  const calculateETA = (download: DownloadItem): string => {
    if (download.status !== 'downloading' || download.bytesDownloaded === 0) {
      return '--';
    }
    
    const speed = calculateSpeed(download);
    if (speed === 0) return '--';
    
    const remainingBytes = download.bytesTotal - download.bytesDownloaded;
    const remainingSeconds = remainingBytes / speed;
    
    if (remainingSeconds < 60) {
      return `${Math.round(remainingSeconds)}s`;
    } else if (remainingSeconds < 3600) {
      return `${Math.round(remainingSeconds / 60)}m`;
    } else {
      return `${Math.round(remainingSeconds / 3600)}h`;
    }
  };

  const handleCancelDownload = (download: DownloadItem) => {
    if (download.abortController) {
      download.abortController.abort();
    }
    setDownloads(prev => prev.map(d => 
      d.id === download.id ? { ...d, status: 'cancelled' as const } : d
    ));
    
    // Dispatch event for other components to react
    window.dispatchEvent(new CustomEvent('download:cancelled', { 
      detail: { id: download.id, modelName: download.modelName } 
    }));
  };

  const handleRemoveDownload = (downloadId: string) => {
    setDownloads(prev => prev.filter(d => d.id !== downloadId));
  };

  const handleClearCompleted = () => {
    setDownloads(prev => prev.filter(d => 
      d.status !== 'completed' && d.status !== 'error' && d.status !== 'cancelled'
    ));
  };

  const toggleExpanded = (downloadId: string) => {
    setExpandedDownloads(prev => {
      const newSet = new Set(prev);
      if (newSet.has(downloadId)) {
        newSet.delete(downloadId);
      } else {
        newSet.add(downloadId);
      }
      return newSet;
    });
  };

  const activeDownloads = downloads.filter(d => d.status === 'downloading').length;
  const completedDownloads = downloads.filter(d => d.status === 'completed').length;

  if (!isVisible) return null;

  return (
    <div className="download-manager-overlay">
      <div className="download-manager-panel">
        <div className="download-manager-header">
          <h3>DOWNLOAD MANAGER</h3>
          <div className="download-manager-stats">
            <span className="download-stat">
              {activeDownloads} active
            </span>
            <span className="download-stat">
              {completedDownloads} completed
            </span>
          </div>
          <button 
            className="download-manager-close"
            onClick={onClose}
            title="Close"
          >
            <svg width="16" height="16" viewBox="0 0 16 16">
              <path d="M 3,3 L 13,13 M 13,3 L 3,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
            </svg>
          </button>
        </div>

        <div className="download-manager-content">
          {downloads.length === 0 ? (
            <div className="download-manager-empty">
              <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                <polyline points="7 10 12 15 17 10" />
                <line x1="12" y1="15" x2="12" y2="3" />
              </svg>
              <p>No downloads yet</p>
              <span className="download-manager-empty-hint">
                Download models from the Model Manager to see them here
              </span>
            </div>
          ) : (
            <div className="download-list">
              {downloads.map(download => {
                const isExpanded = expandedDownloads.has(download.id);
                const speed = calculateSpeed(download);
                const eta = calculateETA(download);
                
                return (
                  <div 
                    key={download.id} 
                    className={`download-item ${download.status}`}
                  >
                    <div className="download-item-header">
                      <div className="download-item-info">
                        <button
                          className="download-expand-btn"
                          onClick={() => toggleExpanded(download.id)}
                          title={isExpanded ? "Collapse" : "Expand"}
                        >
                          <svg 
                            width="12" 
                            height="12" 
                            viewBox="0 0 12 12"
                            style={{ 
                              transform: isExpanded ? 'rotate(90deg)' : 'rotate(0deg)',
                              transition: 'transform 0.2s'
                            }}
                          >
                            <path d="M 4,2 L 8,6 L 4,10" stroke="currentColor" strokeWidth="1.5" fill="none"/>
                          </svg>
                        </button>
                        <div className="download-item-text">
                          <span className="download-model-name">{download.modelName}</span>
                          <span className="download-file-info">
                            {download.status === 'downloading' && (
                              <>
                                File {download.fileIndex}/{download.totalFiles} • {formatBytes(download.bytesDownloaded)} / {formatBytes(download.bytesTotal)}
                              </>
                            )}
                            {download.status === 'completed' && (
                              <>Completed • {formatBytes(download.bytesTotal)}</>
                            )}
                            {download.status === 'error' && (
                              <>Error: {download.error || 'Unknown error'}</>
                            )}
                            {download.status === 'cancelled' && (
                              <>Cancelled</>
                            )}
                          </span>
                        </div>
                      </div>
                      <div className="download-item-actions">
                        {download.status === 'downloading' && (
                          <>
                            <span className="download-speed">{formatSpeed(speed)}</span>
                            <span className="download-eta">{eta}</span>
                            <button
                              className="download-action-btn cancel-btn"
                              onClick={() => handleCancelDownload(download)}
                              title="Cancel download"
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <circle cx="12" cy="12" r="10"/>
                                <line x1="8" y1="12" x2="16" y2="12"/>
                              </svg>
                            </button>
                          </>
                        )}
                        {(download.status === 'completed' || download.status === 'error' || download.status === 'cancelled') && (
                          <button
                            className="download-action-btn remove-btn"
                            onClick={() => handleRemoveDownload(download.id)}
                            title="Remove from list"
                          >
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <line x1="18" y1="6" x2="6" y2="18"/>
                              <line x1="6" y1="6" x2="18" y2="18"/>
                            </svg>
                          </button>
                        )}
                      </div>
                    </div>
                    
                    {download.status === 'downloading' && (
                      <div className="download-progress-container">
                        <div className="download-progress-bar">
                          <div 
                            className="download-progress-fill"
                            style={{ width: `${download.percent}%` }}
                          />
                        </div>
                        <span className="download-progress-text">{download.percent}%</span>
                      </div>
                    )}
                    
                    {isExpanded && (
                      <div className="download-item-details">
                        <div className="download-detail-row">
                          <span className="download-detail-label">Status:</span>
                          <span className="download-detail-value">{download.status}</span>
                        </div>
                        <div className="download-detail-row">
                          <span className="download-detail-label">Current File:</span>
                          <span className="download-detail-value">{download.fileName}</span>
                        </div>
                        <div className="download-detail-row">
                          <span className="download-detail-label">Files:</span>
                          <span className="download-detail-value">{download.fileIndex} of {download.totalFiles}</span>
                        </div>
                        {download.status === 'downloading' && (
                          <>
                            <div className="download-detail-row">
                              <span className="download-detail-label">Downloaded:</span>
                              <span className="download-detail-value">{formatBytes(download.bytesDownloaded)}</span>
                            </div>
                            <div className="download-detail-row">
                              <span className="download-detail-label">Total Size:</span>
                              <span className="download-detail-value">{formatBytes(download.bytesTotal)}</span>
                            </div>
                            <div className="download-detail-row">
                              <span className="download-detail-label">Speed:</span>
                              <span className="download-detail-value">{formatSpeed(speed)}</span>
                            </div>
                          </>
                        )}
                      </div>
                    )}
                  </div>
                );
              })}
            </div>
          )}
        </div>

        {downloads.some(d => d.status === 'completed' || d.status === 'error' || d.status === 'cancelled') && (
          <div className="download-manager-footer">
            <button 
              className="clear-completed-btn"
              onClick={handleClearCompleted}
            >
              Clear Completed
            </button>
          </div>
        )}
      </div>
    </div>
  );
};

export default DownloadManager;

