import React, { useState, useEffect } from 'react';
import { serverFetch } from './utils/serverConfig';

export interface DownloadItem {
  id: string;
  modelName: string;
  fileName: string;
  fileIndex: number;
  totalFiles: number;
  bytesDownloaded: number;
  bytesTotal: number;
  percent: number;
  status: 'downloading' | 'paused' | 'completed' | 'error' | 'cancelled' | 'deleting';
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
  // Track models that are currently being deleted to prevent retry during cleanup
  const [deletingModels, setDeletingModels] = useState<Set<string>>(new Set());

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
          // Remove any previous downloads for this model before adding the new one
          const filtered = prev.filter(d => d.modelName !== downloadItem.modelName);
          return [downloadItem, ...filtered];
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
    
    // Handle edge case where bytesDownloaded > bytesTotal (incomplete byte tracking)
    if (remainingBytes <= 0) return '--';
    
    const remainingSeconds = remainingBytes / speed;
    
    if (remainingSeconds < 60) {
      return `${Math.round(remainingSeconds)}s`;
    } else if (remainingSeconds < 3600) {
      return `${Math.round(remainingSeconds / 60)}m`;
    } else {
      return `${Math.round(remainingSeconds / 3600)}h`;
    }
  };

  const handlePauseDownload = (download: DownloadItem) => {
    if (download.abortController) {
      download.abortController.abort();
    }
    setDownloads(prev => prev.map(d => 
      d.id === download.id ? { ...d, status: 'paused' as const } : d
    ));
    
    // Dispatch event for other components to react
    window.dispatchEvent(new CustomEvent('download:paused', { 
      detail: { id: download.id, modelName: download.modelName } 
    }));
  };

  const handleCancelDownload = async (download: DownloadItem) => {
    // Mark as deleting to prevent retry during cleanup
    setDeletingModels(prev => new Set(prev).add(download.modelName));
    
    // First, abort the download to stop any active streams
    if (download.abortController) {
      download.abortController.abort();
    }
    
    // Update UI to show deleting status (visual feedback during cleanup)
    setDownloads(prev => prev.map(d => 
      d.id === download.id ? { ...d, status: 'deleting' as const } : d
    ));
    
    // Dispatch event for other components to react
    window.dispatchEvent(new CustomEvent('download:cancelled', { 
      detail: { id: download.id, modelName: download.modelName } 
    }));

    // Wait for the download stream to fully close and release file handles
    // We listen for the cleanup-complete event with a timeout fallback
    await new Promise<void>((resolve) => {
      const timeout = setTimeout(() => {
        cleanup();
        resolve();
      }, 2000); // Fallback timeout if event doesn't fire

      const cleanup = () => {
        window.removeEventListener('download:cleanup-complete' as any, handler);
        clearTimeout(timeout);
      };

      const handler = (event: CustomEvent) => {
        if (event.detail.id === download.id) {
          cleanup();
          resolve();
        }
      };

      window.addEventListener('download:cleanup-complete' as any, handler);
    });

    // Call delete endpoint to clean up any partial downloads
    try {
      const response = await serverFetch('/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: download.modelName })
      });
      
      if (!response.ok) {
        const errorText = await response.text();
        console.error('Failed to delete partial download files:', response.statusText, errorText);
        alert(`Download cancelled, but failed to delete files: ${response.statusText}\nPartial files may remain on disk.`);
      }
    } catch (error) {
      console.error('Error deleting partial download files:', error);
      alert(`Download cancelled, but failed to delete files: ${error instanceof Error ? error.message : 'Unknown error'}\nPartial files may remain on disk.`);
    } finally {
      // Mark as cancelled now that deletion is complete
      setDownloads(prev => prev.map(d => 
        d.id === download.id ? { ...d, status: 'cancelled' as const } : d
      ));
      // Remove from deleting set
      setDeletingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(download.modelName);
        return newSet;
      });
    }
  };

  const handleDeleteDownload = async (download: DownloadItem) => {
    // Mark as deleting to prevent retry during cleanup
    setDeletingModels(prev => new Set(prev).add(download.modelName));
    
    // Update UI to show deleting status
    setDownloads(prev => prev.map(d => 
      d.id === download.id ? { ...d, status: 'deleting' as const } : d
    ));
    
    // Call delete endpoint to clean up files
    try {
      const response = await serverFetch('/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: download.modelName })
      });
      
      if (!response.ok) {
        const errorText = await response.text();
        console.error('Failed to delete model files:', response.statusText, errorText);
        alert(`Failed to delete model files: ${response.statusText}`);
        return;
      }

      // Only remove from downloads list if deletion was successful
      handleRemoveDownload(download.id);
    } catch (error) {
      console.error('Error deleting model files:', error);
      alert(`Error deleting model files: ${error instanceof Error ? error.message : 'Unknown error'}`);
      // Revert status on error
      setDownloads(prev => prev.map(d => 
        d.id === download.id ? { ...d, status: 'paused' as const } : d
      ));
    } finally {
      // Remove from deleting set
      setDeletingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(download.modelName);
        return newSet;
      });
    }
  };

  const handleResumeDownload = (download: DownloadItem) => {
    // Dispatch event to trigger a new download
    window.dispatchEvent(new CustomEvent('download:resume', { 
      detail: { modelName: download.modelName } 
    }));
    
    // Remove the paused download from the list as a new one will be created
    handleRemoveDownload(download.id);
  };

  const handleRetryDownload = async (download: DownloadItem) => {
    // Check if this model is currently being deleted
    if (deletingModels.has(download.modelName)) {
      // Wait for deletion to complete before retrying
      // We'll poll the deletingModels set until the model is no longer being deleted
      await new Promise<void>((resolve) => {
        const checkInterval = setInterval(() => {
          setDeletingModels(prev => {
            if (!prev.has(download.modelName)) {
              clearInterval(checkInterval);
              resolve();
            }
            return prev;
          });
        }, 100);
        
        // Timeout after 10 seconds
        setTimeout(() => {
          clearInterval(checkInterval);
          resolve();
        }, 10000);
      });
    }
    
    // Dispatch event to trigger a new download
    window.dispatchEvent(new CustomEvent('download:retry', { 
      detail: { modelName: download.modelName } 
    }));
    
    // Remove the cancelled download from the list as a new one will be created
    handleRemoveDownload(download.id);
  };

  const handleRemoveDownload = (downloadId: string) => {
    setDownloads(prev => prev.filter(d => d.id !== downloadId));
  };

  const handleClearCompleted = () => {
    setDownloads(prev => prev.filter(d => 
      d.status !== 'completed' && d.status !== 'error' && d.status !== 'cancelled' && d.status !== 'paused'
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
    <div 
      className="download-manager-overlay"
      onClick={onClose}
    >
      <div 
        className="download-manager-panel"
        onClick={(e) => e.stopPropagation()}
      >
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
                            {download.status === 'paused' && (
                              <>
                                Paused • File {download.fileIndex}/{download.totalFiles} • {formatBytes(download.bytesDownloaded)} / {formatBytes(download.bytesTotal)}
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
                            {download.status === 'deleting' && (
                              <>Deleting files...</>
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
                              className="download-action-btn pause-btn"
                              onClick={() => handlePauseDownload(download)}
                              title="Pause download"
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <rect x="6" y="4" width="4" height="16"/>
                                <rect x="14" y="4" width="4" height="16"/>
                              </svg>
                            </button>
                            <button
                              className="download-action-btn cancel-btn"
                              onClick={() => handleCancelDownload(download)}
                              title="Cancel download and delete files"
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <circle cx="12" cy="12" r="10"/>
                                <line x1="15" y1="9" x2="9" y2="15"/>
                                <line x1="9" y1="9" x2="15" y2="15"/>
                              </svg>
                            </button>
                          </>
                        )}
                        {download.status === 'paused' && (
                          <>
                            <button
                              className="download-action-btn resume-btn"
                              onClick={() => handleResumeDownload(download)}
                              title="Resume download"
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <polygon points="5,3 19,12 5,21" fill="currentColor"/>
                              </svg>
                            </button>
                            <button
                              className="download-action-btn delete-btn"
                              onClick={() => handleDeleteDownload(download)}
                              title="Delete partial download"
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <polyline points="3,6 5,6 21,6"/>
                                <path d="M19,6v14a2,2,0,0,1-2,2H7a2,2,0,0,1-2-2V6m3,0V4a2,2,0,0,1,2-2h4a2,2,0,0,1,2,2V6"/>
                              </svg>
                            </button>
                          </>
                        )}
                        {download.status === 'deleting' && (
                          <span className="download-deleting-text">Deleting...</span>
                        )}
                        {download.status === 'cancelled' && (
                          <button
                            className="download-action-btn retry-btn"
                            onClick={() => handleRetryDownload(download)}
                            title="Retry download"
                            disabled={deletingModels.has(download.modelName)}
                          >
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <polyline points="23,4 23,10 17,10"/>
                              <path d="M20.49,15a9,9,0,1,1-2.12-9.36L23,10"/>
                            </svg>
                          </button>
                        )}
                        {(download.status === 'completed' || download.status === 'error') && (
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

        {downloads.some(d => d.status === 'completed' || d.status === 'error' || d.status === 'cancelled' || d.status === 'paused' || d.status === 'deleting') && (
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

