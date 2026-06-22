import React, { useCallback, useEffect, useMemo, useState } from 'react';
import api, { friendlyErrorMessage } from '../api';
import { Icon } from './Icon';
import { DownloadListItem, downloadStore, isDownloadActive } from '../features/downloadManager/downloadStore';

interface DownloadManagerProps {
  isVisible: boolean;
  onClose: () => void;
}

function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.max(Math.floor(Math.log(bytes) / Math.log(k)), 0), sizes.length - 1);
  return `${(bytes / Math.pow(k, i)).toFixed(2)} ${sizes[i]}`;
}

function formatTotalBytes(download: DownloadListItem): string {
  if (download.bytesTotalIsLowerBound && download.bytesTotal > 0) return `${formatBytes(download.bytesTotal)}+`;
  return formatBytes(download.bytesTotal);
}

function formatSpeed(download: DownloadListItem): string {
  const speed = calculateSpeed(download);
  if (!Number.isFinite(speed) || speed <= 0) return '--';
  if (speed < 1) return '<1 B/s';
  return `${formatBytes(speed)}/s`;
}

function displayName(modelName: string): string {
  return modelName.startsWith('user.') ? modelName.slice('user.'.length) : modelName;
}

function calculateSpeed(download: DownloadListItem): number {
  if (typeof download.speedBytesPerSecond === 'number' && download.speedBytesPerSecond > 0) return download.speedBytesPerSecond;
  const elapsedSeconds = (Date.now() - download.startTime) / 1000;
  if (elapsedSeconds <= 0) return 0;
  const sessionBytes = download.bytesDownloaded - (download.bytesResumed || 0);
  return Math.max(0, sessionBytes) / elapsedSeconds;
}

function calculateETA(download: DownloadListItem): string {
  if (download.status !== 'downloading' || download.bytesDownloaded === 0 || download.bytesTotalIsLowerBound) return '--';
  const speed = calculateSpeed(download);
  if (!Number.isFinite(speed) || speed < 1) return '--';
  const remainingBytes = download.bytesTotal - download.bytesDownloaded;
  if (remainingBytes <= 0) return '--';
  const remainingSeconds = remainingBytes / speed;
  if (remainingSeconds < 60) return `${Math.round(remainingSeconds)}s`;
  if (remainingSeconds < 3600) return `${Math.round(remainingSeconds / 60)}m`;
  return `${Math.round(remainingSeconds / 3600)}h`;
}

function isFinalizing(download: DownloadListItem): boolean {
  return download.status === 'downloading'
    && !download.bytesTotalIsLowerBound
    && download.bytesTotal > 0
    && download.bytesDownloaded >= download.bytesTotal;
}

async function removeDownload(download: DownloadListItem): Promise<void> {
  downloadStore.remove(download.id);
  await api.controlDownload(download.id, 'remove').catch(() => undefined);
}

const DownloadManager: React.FC<DownloadManagerProps> = ({ isVisible, onClose }) => {
  const [downloads, setDownloads] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set());
  const [busyIds, setBusyIds] = useState<Set<string>>(() => new Set());

  useEffect(() => downloadStore.subscribe(setDownloads), []);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && isVisible) onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [isVisible, onClose]);

  useEffect(() => {
    if (isVisible) void downloadStore.refresh();
  }, [isVisible]);

  const activeDownloads = useMemo(
    () => downloads.filter(isDownloadActive).length,
    [downloads],
  );
  const completedDownloads = useMemo(
    () => downloads.filter(d => d.status === 'completed').length,
    [downloads],
  );

  const withBusy = useCallback(async (id: string, task: () => Promise<void>) => {
    setBusyIds(prev => new Set(prev).add(id));
    try {
      await task();
    } finally {
      setBusyIds(prev => {
        const next = new Set(prev);
        next.delete(id);
        return next;
      });
    }
  }, []);

  const handlePause = (download: DownloadListItem) => withBusy(download.id, async () => {
    downloadStore.markLocal(download.modelName, 'paused', download.downloadType);
    await api.controlDownload(download.id, 'pause').catch(() => undefined);
    await downloadStore.refresh();
  });

  const handleCancel = (download: DownloadListItem) => withBusy(download.id, async () => {
    downloadStore.markLocal(download.modelName, 'cancelled', download.downloadType);
    await api.controlDownload(download.id, 'cancel').catch(() => undefined);
    await downloadStore.refresh();
  });

  const handleDeletePartial = (download: DownloadListItem) => withBusy(download.id, async () => {
    downloadStore.markLocal(download.modelName, 'deleting', download.downloadType);
    try {
      if (download.downloadType === 'model') {
        await api.deleteModel(download.modelName);
      } else {
        const [recipe, backend] = download.modelName.split(':');
        if (recipe && backend) await api.uninstallBackend(recipe, backend);
      }
    } finally {
      await removeDownload(download);
      await downloadStore.refresh();
    }
  });

  const handleResume = (download: DownloadListItem) => withBusy(download.id, async () => {
    downloadStore.markLocal(download.modelName, 'downloading', download.downloadType);
    if (download.downloadType === 'model') {
      void api.pullModel(download.modelName, {
        onProgress: data => downloadStore.upsertFromPull(download.modelName, data, 'model'),
        onComplete: data => downloadStore.upsertFromPull(download.modelName, { ...data, status: 'completed', complete: true, percent: 100 }, 'model'),
        onError: err => downloadStore.upsertFromPull(download.modelName, { status: 'error', error: friendlyErrorMessage(err) }, 'model'),
      }).catch(err => downloadStore.upsertFromPull(download.modelName, { status: 'error', error: friendlyErrorMessage(err) }, 'model'));
    }
    await downloadStore.refresh();
  });

  const handleRetry = (download: DownloadListItem) => withBusy(download.id, async () => {
    await removeDownload(download);
    if (download.downloadType === 'model') {
      downloadStore.markLocal(download.modelName, 'downloading', 'model');
      void api.pullModel(download.modelName, {
        onProgress: data => downloadStore.upsertFromPull(download.modelName, data, 'model'),
        onComplete: data => downloadStore.upsertFromPull(download.modelName, { ...data, status: 'completed', complete: true, percent: 100 }, 'model'),
        onError: err => downloadStore.upsertFromPull(download.modelName, { status: 'error', error: friendlyErrorMessage(err) }, 'model'),
      }).catch(err => downloadStore.upsertFromPull(download.modelName, { status: 'error', error: friendlyErrorMessage(err) }, 'model'));
    }
    await downloadStore.refresh();
  });

  const handleRemove = (download: DownloadListItem) => withBusy(download.id, async () => {
    await removeDownload(download);
  });

  const removableTerminalDownloads = useMemo(
    () => downloads.filter(download => download.running !== true && (
      download.status === 'completed' || download.status === 'cancelled' || download.status === 'error'
    )),
    [downloads],
  );

  const handleClearCompleted = () => {
    const removable = removableTerminalDownloads;
    if (removable.length === 0) return;

    // Match main's behavior: remove the visible terminal rows immediately and
    // tell the server in the background. Do not refresh right away, because the
    // server may still return the terminal row for a short window and that makes
    // the UI appear to jump into an unclear state. The dismissed ids suppress
    // stale terminal snapshots across tabs until the server forgets them.
    downloadStore.removeMany(removable.map(download => download.id));
    void Promise.allSettled(removable.map(download => api.controlDownload(download.id, 'remove')));
  };

  const toggleExpanded = (downloadId: string) => {
    setExpanded(prev => {
      const next = new Set(prev);
      if (next.has(downloadId)) next.delete(downloadId);
      else next.add(downloadId);
      return next;
    });
  };

  if (!isVisible) return null;

  return (
    <div className="download-manager" role="dialog" aria-modal="false" aria-label="Download manager" onClick={onClose}>
      <div className="download-manager__panel" onClick={event => event.stopPropagation()}>
        <div className="download-manager__header">
          <div>
            <div className="download-manager__eyebrow">Download Manager</div>
            <div className="download-manager__counts">{activeDownloads} active · {completedDownloads} completed</div>
          </div>
          <button type="button" className="download-manager__close" onClick={onClose} aria-label="Close download manager">
            <Icon name="x" size={15} />
          </button>
        </div>

        <div className="download-manager__body">
          {downloads.length === 0 ? (
            <div className="download-manager__empty">
              <Icon name="download" size={26} />
              <strong>No downloads yet</strong>
              <span>Download models from the Model Manager to see them here.</span>
            </div>
          ) : (
            downloads.map(download => {
              const itemExpanded = expanded.has(download.id);
              const finalizing = isFinalizing(download);
              const busy = busyIds.has(download.id);
              const eta = finalizing ? 'finalizing...' : calculateETA(download);
              const canRemove = download.running !== true && !busy;
              return (
                <div className={`download-item download-item--${download.status}`} key={download.id}>
                  <div className="download-item__top">
                    <button
                      type="button"
                      className="download-item__summary"
                      onClick={() => toggleExpanded(download.id)}
                      aria-expanded={itemExpanded}
                    >
                      <Icon name={itemExpanded ? 'chevron-down' : 'chevron-right'} size={14} />
                      <span className="download-item__names">
                        <strong>{download.collectionComponents?.length ? `Setting up ${displayName(download.modelName)}` : displayName(download.modelName)}</strong>
                        {download.collectionComponents?.length ? (
                          <span>{download.collectionComponents.length} models: {download.collectionComponents.map(displayName).join(', ')}</span>
                        ) : (
                          <span>
                            {download.status === 'downloading' && <>File {download.fileIndex}/{download.totalFiles} · {formatBytes(download.bytesDownloaded)} / {formatTotalBytes(download)}</>}
                            {download.status === 'paused' && <>{download.running === true ? 'Pausing' : 'Paused'} · File {download.fileIndex}/{download.totalFiles} · {formatBytes(download.bytesDownloaded)} / {formatTotalBytes(download)}</>}
                            {download.status === 'completed' && <>Completed · {formatTotalBytes(download)}</>}
                            {download.status === 'error' && <>Error: {download.error || 'Unknown error'}</>}
                            {download.status === 'cancelled' && <>Cancelled</>}
                            {download.status === 'deleting' && <>Deleting files...</>}
                          </span>
                        )}
                      </span>
                    </button>

                    <div className="download-item__actions">
                      {download.status === 'downloading' && (
                        <>
                          {!finalizing && <span className="download-item__metric">{formatSpeed(download)}</span>}
                          <span className="download-item__metric">{eta}</span>
                          <button type="button" onClick={() => handlePause(download)} disabled={busy} title="Pause download" aria-label="Pause download"><Icon name="pause" size={13} /></button>
                          <button type="button" onClick={() => handleCancel(download)} disabled={busy} title="Cancel download and delete files" aria-label="Cancel download"><Icon name="x" size={13} /></button>
                        </>
                      )}
                      {download.status === 'paused' && (
                        <>
                          {download.running === true && <span className="download-item__metric">Pausing...</span>}
                          <button type="button" onClick={() => handleResume(download)} disabled={busy || download.running === true} title="Resume download" aria-label="Resume download"><Icon name="play" size={13} /></button>
                          <button type="button" onClick={() => handleDeletePartial(download)} disabled={!canRemove} title="Delete partial download" aria-label="Delete partial download"><Icon name="trash" size={13} /></button>
                          <button type="button" onClick={() => handleRemove(download)} disabled={!canRemove} title="Remove from list" aria-label="Remove from list"><Icon name="x" size={13} /></button>
                        </>
                      )}
                      {download.status === 'deleting' && <span className="download-item__metric">Deleting...</span>}
                      {download.status === 'cancelled' && (
                        download.running === true ? <span className="download-item__metric">Cancelling...</span> : (
                          <>
                            <button type="button" onClick={() => handleRetry(download)} disabled={busy} title="Retry download" aria-label="Retry download"><Icon name="rotate-ccw" size={13} /></button>
                            <button type="button" onClick={() => handleDeletePartial(download)} disabled={busy} title="Delete partial download" aria-label="Delete partial download"><Icon name="trash" size={13} /></button>
                            <button type="button" onClick={() => handleRemove(download)} disabled={busy} title="Remove from list" aria-label="Remove from list"><Icon name="x" size={13} /></button>
                          </>
                        )
                      )}
                      {(download.status === 'completed' || download.status === 'error') && (
                        <button type="button" onClick={() => handleRemove(download)} disabled={busy} title="Remove from list" aria-label="Remove from list"><Icon name="x" size={13} /></button>
                      )}
                    </div>
                  </div>

                  {download.status === 'downloading' && (
                    <div className="download-item__progress" aria-label={`${download.percent.toFixed(0)}%`}>
                      <div className="download-item__progress-track"><div className="download-item__progress-fill" style={{ width: `${download.percent}%` }} /></div>
                      <span>{download.percent.toFixed(0)}%</span>
                    </div>
                  )}

                  {itemExpanded && (
                    <div className="download-item__details">
                      <span>Status: {download.status}</span>
                      <span>Current file: {download.fileName || '—'}</span>
                      <span>Files: {download.fileIndex} of {download.totalFiles}</span>
                      {download.status === 'downloading' && (
                        <>
                          <span>Downloaded: {formatBytes(download.bytesDownloaded)}</span>
                          <span>Total size: {formatTotalBytes(download)}</span>
                          {!finalizing && <span>Speed: {formatSpeed(download)}</span>}
                        </>
                      )}
                    </div>
                  )}
                </div>
              );
            })
          )}
        </div>

        {removableTerminalDownloads.length > 0 && (
          <button type="button" className="download-manager__clear" onClick={handleClearCompleted}>Clear completed</button>
        )}
      </div>
    </div>
  );
};

export default DownloadManager;
