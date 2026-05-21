import React, { useState, useEffect } from 'react';
import { controlDownload, deleteModel, uninstallBackend, waitForDownloadStatus } from './utils/backendInstaller';
import { downloadTracker } from './utils/downloadTracker';

export interface DownloadItem {
  id: string;
  modelName: string;
  fileName: string;
  fileIndex: number;
  totalFiles: number;
  bytesDownloaded: number;
  bytesTotal: number;
  // True when bytesTotal is only the amount known so far, not the real final total.
  // Used for backend split archives or runtime follow-up steps whose later sizes are not known yet.
  bytesTotalIsLowerBound?: boolean;
  percent: number;
  status: 'downloading' | 'paused' | 'completed' | 'error' | 'cancelled' | 'deleting';
  error?: string;
  startTime: number;
  bytesResumed: number;  // Bytes already on disk at session start (for accurate speed)
  abortController?: AbortController;
  downloadType?: 'model' | 'backend';
  // Components when this download is a collection.
  // UI uses this to explain the collection is made up of separate models.
  collectionComponents?: string[];
  // Declared size from the model registry (bytes). Used as the total when the
  // server doesn't emit a cumulative download size, instead of extrapolating
  // from the first file or two (which overshoots badly for FLM pulls).
  declaredTotalBytes?: number;
  // Server-owned jobs can be terminal from the UI point of view while the
  // worker is still unwinding. Keep this so resume waits until pause is real.
  running?: boolean;
  // Smoothed/last-sample speed from the tracker. Calculated from byte deltas
  // between progress snapshots so restored/skipped bytes do not inflate speed.
  speedBytesPerSecond?: number;
  speedSampleTime?: number;
  speedSampleBytes?: number;
  updatedAt?: number;
}

interface DownloadManagerProps {
  isVisible: boolean;
  onClose: () => void;
}

const DownloadManager: React.FC<DownloadManagerProps> = ({ isVisible, onClose }) => {
  const [downloads, setDownloads] = useState<DownloadItem[]>(() => downloadTracker.getActiveDownloads());
  const [expandedDownloads, setExpandedDownloads] = useState<Set<string>>(new Set());
  // Track models that are currently being deleted to prevent retry during cleanup
  const [deletingModels, setDeletingModels] = useState<Set<string>>(new Set());

  const getPanelDownloads = (): DownloadItem[] => downloadTracker.getActiveDownloads();

  const forceServerSync = async (): Promise<DownloadItem[]> => {
    await downloadTracker.hydrateFromServer();
    return getPanelDownloads();
  };

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

    const handleDownloadRemoved = (event: CustomEvent<{ id: string }>) => {
      const { id } = event.detail;
      setDownloads(prev => prev.filter(d => d.id !== id));
    };

    const handleDownloadSnapshot = () => {
      setDownloads(getPanelDownloads());
    };

    window.addEventListener('download:update' as any, handleDownloadUpdate);
    window.addEventListener('download:complete' as any, handleDownloadComplete);
    window.addEventListener('download:error' as any, handleDownloadError);
    window.addEventListener('download:removed' as any, handleDownloadRemoved);
    window.addEventListener('download:snapshot' as any, handleDownloadSnapshot);

    downloadTracker.connectServerEvents();
    void forceServerSync().then(setDownloads);

    return () => {
      window.removeEventListener('download:update' as any, handleDownloadUpdate);
      window.removeEventListener('download:complete' as any, handleDownloadComplete);
      window.removeEventListener('download:error' as any, handleDownloadError);
      window.removeEventListener('download:removed' as any, handleDownloadRemoved);
      window.removeEventListener('download:snapshot' as any, handleDownloadSnapshot);
    };
  }, []);

  useEffect(() => {
    if (!isVisible) return;
    void forceServerSync().then(setDownloads);
  }, [isVisible]);

  const formatBytes = (bytes: number): string => {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.min(Math.floor(Math.log(bytes) / Math.log(k)), sizes.length - 1);
    return `${(bytes / Math.pow(k, i)).toFixed(2)} ${sizes[i]}`;
  };

  const formatTotalBytes = (download: DownloadItem): string => {
    if (download.bytesTotalIsLowerBound && download.bytesTotal > 0) {
      return `${formatBytes(download.bytesTotal)}+`;
    }
    return formatBytes(download.bytesTotal);
  };

  const formatSpeed = (bytesPerSecond: number): string => {
    return `${formatBytes(bytesPerSecond)}/s`;
  };

  const getDownloadDisplayName = (modelName: string): string => {
    return modelName.startsWith('user.') ? modelName.slice('user.'.length) : modelName;
  };

  const calculateSpeed = (download: DownloadItem): number => {
    if (typeof download.speedBytesPerSecond === 'number') {
      return Math.max(0, download.speedBytesPerSecond);
    }

    const elapsedSeconds = (Date.now() - download.startTime) / 1000;
    if (elapsedSeconds === 0) return 0;
    // Only count bytes downloaded in this session, not bytes already on disk from a prior run
    const sessionBytes = download.bytesDownloaded - (download.bytesResumed || 0);
    return Math.max(0, sessionBytes) / elapsedSeconds;
  };

  const calculateETA = (download: DownloadItem): string => {
    if (download.status !== 'downloading' || download.bytesDownloaded === 0) {
      return '--';
    }

    // Unknown lower-bound totals cannot produce a meaningful remaining-time estimate.
    if (download.bytesTotalIsLowerBound) {
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

  const isServerDownloadId = (downloadId?: string): boolean =>
    downloadId?.startsWith('model:') === true || downloadId?.startsWith('backend:') === true;

  const usesServerDownloadControl = (download: DownloadItem | undefined, downloadId?: string): boolean => {
    return download?.downloadType === 'model' ||
      download?.downloadType === 'backend' ||
      isServerDownloadId(download?.id ?? downloadId);
  };

  const hasLocalDownloadOwner = (download: DownloadItem): boolean => {
    return !!download.abortController && !download.abortController.signal.aborted;
  };

  const handlePauseDownload = async (download: DownloadItem) => {
    // UI feedback must be immediate. Keep the tracker owner intact until the
    // server confirms; otherwise the local fallback cannot abort the owner.
    setDownloads(prev => prev.map(d =>
      d.id === download.id ? { ...d, status: 'paused' as const, running: true } : d
    ));

    if (!usesServerDownloadControl(download)) {
      downloadTracker.requestPause(download.id);
      return;
    }

    try {
      const snapshot = await controlDownload(download.id, 'pause');
      if (snapshot) {
        downloadTracker.applyServerDownload(snapshot);
      }
    } catch (error) {
      console.error('Error pausing model download via server registry:', error);
      if (hasLocalDownloadOwner(download)) {
        downloadTracker.requestPause(download.id);
        return;
      }

      alert('Could not pause the server-owned download. Refreshing download state.');
      void forceServerSync().then(setDownloads);
    }
  };

  const waitForLocalDownloadCleanup = (download: DownloadItem, timeoutMs = 30000): Promise<boolean> => {
    return new Promise<boolean>((resolve) => {
      const cleanup = () => {
        window.removeEventListener('download:cleanup-complete' as any, handler);
        clearTimeout(timeout);
      };

      const handler = (event: CustomEvent) => {
        if (event.detail?.id === download.id || event.detail?.modelName === download.modelName) {
          cleanup();
          resolve(true);
        }
      };

      const timeout = setTimeout(() => {
        cleanup();
        resolve(false);
      }, timeoutMs);

      window.addEventListener('download:cleanup-complete' as any, handler);
    });
  };

  const cleanupDownloadedFiles = async (download: DownloadItem): Promise<void> => {
    if (download.downloadType === 'backend') {
      const [recipe, backend] = download.modelName.split(':');
      await uninstallBackend(recipe, backend);
    } else {
      await deleteModel(download.modelName);
    }
  };

  const ensureDownloadStopped = async (
    download: DownloadItem,
    statuses: Array<'paused' | 'cancelled' | 'completed' | 'error'>,
    actionDescription: string,
  ): Promise<boolean> => {
    if (download.running !== true) return true;

    try {
      await waitForDownloadStatus(download.id, statuses, 30000, false);
      return true;
    } catch (error) {
      console.warn(`Timed out waiting for download to stop before ${actionDescription}:`, error);
      alert(`Cannot ${actionDescription} yet: the download worker is still stopping. Please try again once it finishes.`);
      void forceServerSync().then(setDownloads);
      return false;
    }
  };

  const cancelViaLocalOwner = async (download: DownloadItem): Promise<void> => {
    const cleanupComplete = waitForLocalDownloadCleanup(download);
    downloadTracker.requestCancel(download.id);
    const released = await cleanupComplete;

    if (!released) {
      alert('Download cancellation was requested, but the worker did not confirm that files were released. Partial files were not deleted.');
      void forceServerSync().then(setDownloads);
      return;
    }

    try {
      await cleanupDownloadedFiles(download);
      downloadTracker.removeDownload(download.id);
      setDownloads(prev => prev.filter(d => d.id !== download.id));
    } catch (deleteError) {
      console.error('Error deleting partial download files:', deleteError);
      alert(`Download cancelled, but failed to delete files: ${deleteError instanceof Error ? deleteError.message : 'Unknown error'}
Partial files may remain on disk.`);
    }
  };

  const handleCancelDownload = async (download: DownloadItem) => {
    // Cancel is a two-step operation: first ask the owner to stop, then delete
    // partial files only after the worker reports running=false. This avoids
    // deleting files while the downloader still has open handles.
    setDeletingModels(prev => new Set(prev).add(download.modelName));
    setDownloads(prev => prev.map(d =>
      d.id === download.id ? { ...d, status: 'cancelled' as const, running: true } : d
    ));

    try {
      if (!usesServerDownloadControl(download)) {
        await cancelViaLocalOwner(download);
        return;
      }

      const snapshot = await controlDownload(download.id, 'cancel');
      if (snapshot) {
        downloadTracker.applyServerDownload(snapshot);
      }

      const stopped = snapshot?.running === true
        ? await ensureDownloadStopped(
            { ...download, running: true },
            ['cancelled', 'completed', 'error'],
            'delete partial files',
          )
        : true;

      if (!stopped) return;

      const latestSnapshot = (await downloadTracker.hydrateFromServer())
        .find(item => item.id === download.id);
      const finalStatus = latestSnapshot?.status
        ?? downloadTracker.getDownload(download.id)?.status
        ?? snapshot?.status
        ?? 'cancelled';

      if (finalStatus !== 'completed') {
        await cleanupDownloadedFiles(download);
      }

      downloadTracker.removeDownload(download.id);
      setDownloads(prev => prev.filter(d => d.id !== download.id));
      void controlDownload(download.id, 'remove').catch(() => {});
    } catch (error) {
      console.error('Error cancelling model download via server registry:', error);
      if (hasLocalDownloadOwner(download)) {
        await cancelViaLocalOwner(download);
      } else {
        alert('Could not cancel the server-owned download. Refreshing download state.');
        void forceServerSync().then(setDownloads);
      }
    } finally {
      setDeletingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(download.modelName);
        return newSet;
      });
    }
  };

  const handleDeleteDownload = async (download: DownloadItem) => {
    const stopped = await ensureDownloadStopped(download, ['paused'], 'delete partial files');
    if (!stopped) return;

    // Mark as deleting to prevent retry during cleanup
    setDeletingModels(prev => new Set(prev).add(download.modelName));

    // Update UI to show deleting status
    setDownloads(prev => prev.map(d =>
      d.id === download.id ? { ...d, status: 'deleting' as const, running: false } : d
    ));

    try {
      await cleanupDownloadedFiles(download);

      // Only remove from downloads list if deletion was successful
      await handleRemoveDownload(download.id);
    } catch (error) {
      console.error('Error deleting download files:', error);
      alert(`Error deleting files: ${error instanceof Error ? error.message : 'Unknown error'}`);
      // Revert status on error
      setDownloads(prev => prev.map(d =>
        d.id === download.id ? { ...d, status: 'paused' as const, running: false } : d
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

  const handleResumeDownload = async (download: DownloadItem) => {
    // The server marks the row as paused as soon as pause is requested, but the
    // worker can still be unwinding. If we immediately POST /pull again, the
    // resume request can block while start_download_job joins the old worker.
    // Wait for running=false first when the snapshot tells us it is still
    // pausing; if the server row briefly disappears we still proceed.
    if (download.running === true) {
      try {
        await waitForDownloadStatus(download.id, ['paused'], 30000, true);
      } catch (error) {
        console.warn('Timed out waiting for paused download to stop before resume; trying resume anyway:', error);
      }
    }

    setDownloads(prev => prev.map(d =>
      d.id === download.id ? { ...d, status: 'downloading' as const, running: true, startTime: Date.now(), bytesResumed: d.bytesDownloaded } : d
    ));

    // Dispatch event to trigger the same model/backend pull path. The server
    // deduplicates by stable download id, so this resumes the existing job
    // instead of creating a second independent renderer-owned download.
    window.dispatchEvent(new CustomEvent('download:resume', {
      detail: { modelName: download.modelName, downloadType: download.downloadType }
    }));
  };

  const waitForDeleteCleanup = async (modelName: string): Promise<void> => {
    if (!deletingModels.has(modelName)) return;

    // Wait for deletion to complete before retrying. This preserves the
    // existing UX without letting retry race with partial-file cleanup.
    await new Promise<void>((resolve) => {
      const checkInterval = setInterval(() => {
        setDeletingModels(prev => {
          if (!prev.has(modelName)) {
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
  };

  const removeServerDownloadBeforeRetry = async (download: DownloadItem): Promise<boolean> => {
    const stopped = await ensureDownloadStopped(
      download,
      ['paused', 'cancelled', 'completed', 'error'],
      'retry download',
    );
    if (!stopped) return false;

    // Critical ordering for stable model download ids:
    // remove/dismiss the terminal server row BEFORE emitting download:retry.
    // Emitting first can start a new /pull with the same id, and a later
    // /downloads/control remove for the old row can erase/cancel the fresh job.
    try {
      await controlDownload(download.id, 'remove');
      setDownloads(prev => prev.filter(d => d.id !== download.id));
      downloadTracker.removeDownload(download.id);
      return true;
    } catch (error) {
      console.error('Error removing old server download before retry:', error);
      alert('Could not clear the old download entry before retrying. Refreshing download state.');
      void forceServerSync().then(setDownloads);
      return false;
    }
  };

  const handleRetryDownload = async (download: DownloadItem) => {
    await waitForDeleteCleanup(download.modelName);

    if (usesServerDownloadControl(download)) {
      const removed = await removeServerDownloadBeforeRetry(download);
      if (!removed) return;
    }

    // Dispatch event to trigger a new download. For server-owned model
    // downloads, the old terminal row has already been removed above, so there
    // is no delayed remove request that can hit the freshly-created job.
    window.dispatchEvent(new CustomEvent('download:retry', {
      detail: { modelName: download.modelName, downloadType: download.downloadType }
    }));

    if (!usesServerDownloadControl(download)) {
      // Renderer-owned backend downloads still use unique ids, so removing the
      // old local row after starting the retry cannot delete the fresh attempt.
      void handleRemoveDownload(download.id);
    }
  };

  const handleRemoveDownload = async (downloadId: string) => {
    const download = downloads.find(d => d.id === downloadId) || downloadTracker.getActiveDownloads().find(d => d.id === downloadId);
    if (download?.running === true) {
      alert('Cannot remove this download yet: the download worker is still stopping.');
      void forceServerSync().then(setDownloads);
      return;
    }

    setDownloads(prev => prev.filter(d => d.id !== downloadId));
    downloadTracker.removeDownload(downloadId);
    if (usesServerDownloadControl(download, downloadId)) {
      void controlDownload(downloadId, 'remove').catch(() => {});
    }
  };

  const handleClearCompleted = () => {
    const removable = downloads.filter(d =>
      d.running !== true && (d.status === 'completed' || d.status === 'error')
    );
    for (const download of removable) {
      void handleRemoveDownload(download.id);
    }
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

  const activeDownloads = downloads.filter(d => d.status === 'downloading' || d.running === true).length;
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
                          <span className="download-model-name">
                            {download.collectionComponents && download.collectionComponents.length > 0
                              ? `Setting up ${getDownloadDisplayName(download.modelName)}`
                              : getDownloadDisplayName(download.modelName)}
                          </span>
                          {download.collectionComponents && download.collectionComponents.length > 0 && (
                            <span
                              className="download-file-info"
                              style={{ fontStyle: 'italic', opacity: 0.8 }}
                              title={download.collectionComponents.join('\n')}
                            >
                              {download.collectionComponents.length} models: {download.collectionComponents.map(getDownloadDisplayName).join(', ')}
                            </span>
                          )}
                          <span className="download-file-info">
                            {download.status === 'downloading' && (
                              <>
                                File {download.fileIndex}/{download.totalFiles} • {formatBytes(download.bytesDownloaded)} / {formatTotalBytes(download)}
                              </>
                            )}
                            {download.status === 'paused' && (
                              <>
                                {download.running === true ? 'Pausing' : 'Paused'} • File {download.fileIndex}/{download.totalFiles} • {formatBytes(download.bytesDownloaded)} / {formatTotalBytes(download)}
                              </>
                            )}
                            {download.status === 'completed' && (
                              <>Completed • {formatTotalBytes(download)}</>
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
                            {download.running === true && (
                              <span className="download-eta">Pausing...</span>
                            )}
                            <button
                              className="download-action-btn resume-btn"
                              onClick={() => handleResumeDownload(download)}
                              title={download.running === true ? "Finish pausing, then resume" : "Resume download"}
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <polygon points="5,3 19,12 5,21" fill="currentColor"/>
                              </svg>
                            </button>
                            <button
                              className="download-action-btn delete-btn"
                              onClick={() => handleDeleteDownload(download)}
                              title={download.running === true ? "Finish pausing before deleting" : "Delete partial download"}
                              disabled={download.running === true}
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <polyline points="3,6 5,6 21,6"/>
                                <path d="M19,6v14a2,2,0,0,1-2,2H7a2,2,0,0,1-2-2V6m3,0V4a2,2,0,0,1,2-2h4a2,2,0,0,1,2,2V6"/>
                              </svg>
                            </button>
                            <button
                              className="download-action-btn remove-btn"
                              onClick={() => void handleRemoveDownload(download.id)}
                              title={download.running === true ? "Finish pausing before removing" : "Remove from list"}
                              disabled={download.running === true}
                            >
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <line x1="18" y1="6" x2="6" y2="18"/>
                                <line x1="6" y1="6" x2="18" y2="18"/>
                              </svg>
                            </button>
                          </>
                        )}
                        {download.status === 'deleting' && (
                          <span className="download-deleting-text">Deleting...</span>
                        )}
                        {download.status === 'cancelled' && (
                          download.running === true ? (
                            <span className="download-deleting-text">Cancelling...</span>
                          ) : (
                            <>
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
                              <button
                                className="download-action-btn delete-btn"
                                onClick={() => handleDeleteDownload(download)}
                                title="Delete partial download"
                                disabled={deletingModels.has(download.modelName)}
                              >
                                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <polyline points="3,6 5,6 21,6"/>
                                  <path d="M19,6v14a2,2,0,0,1-2,2H7a2,2,0,0,1-2-2V6m3,0V4a2,2,0,0,1,2-2h4a2,2,0,0,1,2,2V6"/>
                                </svg>
                              </button>
                              <button
                                className="download-action-btn remove-btn"
                                onClick={() => void handleRemoveDownload(download.id)}
                                title="Remove from list"
                                disabled={deletingModels.has(download.modelName)}
                              >
                                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <line x1="18" y1="6" x2="6" y2="18"/>
                                  <line x1="6" y1="6" x2="18" y2="18"/>
                                </svg>
                              </button>
                            </>
                          )
                        )}
                        {(download.status === 'completed' || download.status === 'error') && (
                          <button
                            className="download-action-btn remove-btn"
                            onClick={() => void handleRemoveDownload(download.id)}
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
                              <span className="download-detail-value">{formatTotalBytes(download)}</span>
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

        {downloads.some(d => d.status === 'completed' || d.status === 'error') && (
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
