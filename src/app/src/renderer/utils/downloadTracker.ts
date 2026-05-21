import { DownloadItem } from '../DownloadManager';
import { serverFetch } from './serverConfig';

const ACTIVE_SERVER_DOWNLOAD_POLL_INTERVAL_MS = 2000;
const TERMINAL_DOWNLOAD_VISIBILITY_MS = 30000;

export interface DownloadProgressEvent {
  id?: string;
  type?: 'model' | 'backend';
  model_name?: string;
  status?: DownloadItem['status'];
  running?: boolean;
  file: string;
  file_index: number;
  total_files: number;
  bytes_downloaded: number;
  bytes_total: number;
  percent: number;
  total_download_size?: number;  // Total bytes across ALL files in this download
  bytes_previously_downloaded?: number;  // Bytes already on disk for current file (resume/skip)
  completed_files_bytes?: number;  // Server-side bytes from files completed before current file
  cumulative_bytes_downloaded?: number;  // Server-side total bytes downloaded across files
  overall_bytes_downloaded?: number;  // Alias kept for older in-flight snapshots
  complete?: boolean;
  error?: string;
}

type DownloadCrossTabMessage = {
  source: string;
  type: 'remove' | 'sync';
  id: string;
  modelName?: string;
};

class DownloadTracker {
  private activeDownloads: Map<string, DownloadItem>;
  // Track cumulative data per download
  private cumulativeData: Map<string, {
    completedFilesBytes: number;  // Total bytes from completed files
    fileSizes: Map<number, number>;  // Map of file_index -> file size
    preExistingBytes: Map<number, number>;  // Map of file_index -> bytes already on disk
  }>;
  private dismissedDownloads = new Set<string>();
  private completedDownloadsFinalized = new Set<string>();
  private completedModelDownloadsNotified = new Set<string>();
  private completedBackendDownloadsNotified = new Set<string>();
  private serverPollStarted = false;
  private serverPollTimer: number | undefined;
  private readonly tabId = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  private readonly crossTabChannel?: BroadcastChannel;

  constructor() {
    this.activeDownloads = new Map();
    this.cumulativeData = new Map();

    if (typeof BroadcastChannel !== 'undefined') {
      this.crossTabChannel = new BroadcastChannel('lemonade-download-manager');
      this.crossTabChannel.onmessage = event => this.handleCrossTabMessage(event.data);
    }
  }

  getStableDownloadId(modelName: string, downloadType?: 'model' | 'backend'): string {
    return `${downloadType === 'backend' ? 'backend' : 'model'}:${modelName}`;
  }

  /**
   * Start tracking a new download
   */
  startDownload(
    modelName: string,
    abortController: AbortController,
    downloadType?: 'model' | 'backend',
    collectionComponents?: string[],
    declaredTotalBytes?: number,
  ): string {
    // Remove any existing downloads for this model (completed, error, cancelled, or paused)
    // This ensures only one entry per model is shown
    const existingDownloads = Array.from(this.activeDownloads.entries());
    for (const [id, download] of existingDownloads) {
      if (download.modelName === modelName) {
        // If there's an active download, cancel it first
        if (download.status === 'downloading') {
          if (download.abortController) {
            download.abortController.abort();
          }
        }
        // Remove the old entry
        this.removeLocalDownload(id, download.modelName);
      }
    }

    const downloadId = downloadType === 'model' || downloadType === 'backend'
      ? this.getStableDownloadId(modelName, downloadType)
      : `${modelName}-${Date.now()}`;

    const downloadItem: DownloadItem = {
      id: downloadId,
      modelName,
      fileName: '',
      fileIndex: 0,
      totalFiles: 0,
      bytesDownloaded: 0,
      bytesTotal: declaredTotalBytes ?? 0,
      percent: 0,
      status: 'downloading',
      startTime: Date.now(),
      bytesResumed: 0,
      abortController,
      downloadType,
      collectionComponents,
      declaredTotalBytes,
      bytesTotalIsLowerBound: false,
      running: downloadType === 'model' || downloadType === 'backend' ? true : undefined,
      speedBytesPerSecond: 0,
      speedSampleTime: Date.now(),
      speedSampleBytes: 0,
      updatedAt: Date.now(),
    };

    this.dismissedDownloads.delete(downloadId);
    this.completedDownloadsFinalized.delete(downloadId);
    this.completedModelDownloadsNotified.delete(downloadId);
    this.completedBackendDownloadsNotified.delete(downloadId);
    this.activeDownloads.set(downloadId, downloadItem);
    this.cumulativeData.set(downloadId, {
      completedFilesBytes: 0,
      fileSizes: new Map(),
      preExistingBytes: new Map(),
    });
    this.emitUpdate(downloadItem);
    this.scheduleServerPoll(0);
    // Wake up other tabs immediately. They still hydrate from the server as the
    // source of truth, but no longer need a refresh or a full poll interval to
    // discover that a download has started elsewhere.
    this.postCrossTabMessage({ type: 'sync', id: downloadId, modelName });

    return downloadId;
  }

  /**
   * Update download progress
   */
  updateProgress(downloadId: string, progress: DownloadProgressEvent): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download || this.dismissedDownloads.has(downloadId)) return;

    const cumulative = this.cumulativeData.get(downloadId);
    if (!cumulative) return;

    // Track the size of each file (only if we have real byte data)
    if (progress.bytes_total > 0 && !cumulative.fileSizes.has(progress.file_index)) {
      cumulative.fileSizes.set(progress.file_index, progress.bytes_total);
    }

    // Track pre-existing bytes per file (from backend's bytes_previously_downloaded)
    if (progress.bytes_previously_downloaded != null && progress.bytes_previously_downloaded > 0) {
      if (!cumulative.preExistingBytes.has(progress.file_index)) {
        cumulative.preExistingBytes.set(progress.file_index, progress.bytes_previously_downloaded);
      }
    }

    // If the server owns the job, prefer its cumulative snapshot. This keeps
    // reload/new-tab recovery correct for multi-file downloads.
    if (typeof progress.completed_files_bytes === 'number') {
      cumulative.completedFilesBytes = Math.max(
        cumulative.completedFilesBytes,
        progress.completed_files_bytes,
      );
    }

    const serverCumulativeBytes = typeof progress.cumulative_bytes_downloaded === 'number'
      ? progress.cumulative_bytes_downloaded
      : (typeof progress.overall_bytes_downloaded === 'number' ? progress.overall_bytes_downloaded : undefined);

    // If we moved to a new file, add the previous file's size to completed bytes
    if (serverCumulativeBytes == null && progress.file_index > download.fileIndex) {
      // Only add the previous file's size if we have it tracked
      // Use 0 as fallback - if we never got byte data for a file, we can't count it
      // Note: download.bytesTotal is the CUMULATIVE total, not the individual file size!
      const previousFileSize = cumulative.fileSizes.get(download.fileIndex) || 0;
      cumulative.completedFilesBytes += previousFileSize;
    }

    // Calculate cumulative totals
    const cumulativeBytesDownloaded = serverCumulativeBytes ??
      (cumulative.completedFilesBytes + progress.bytes_downloaded);

    // Determine total download size:
    // 1. Server-reported total (covers all files) — best option
    // 2. Declared size from the model registry — honest number, honors what the
    //    bar shows elsewhere, no extrapolation artifacts
    // 3. Local sum of known file sizes — only accurate after every file's total
    //    has been observed. Before then, keep byte-level progress as a known
    //    lower bound for display, and use file-count progress for percentage.
    let cumulativeBytesTotal: number;
    let bytesTotalIsLowerBound = false;
    const knownSizes = Array.from(cumulative.fileSizes.values());
    const knownSizesTotal = knownSizes.reduce((sum, size) => sum + size, 0);
    const knownBytesLowerBound = Math.max(cumulativeBytesDownloaded, knownSizesTotal);

    if (progress.total_download_size && progress.total_download_size > 0) {
      cumulativeBytesTotal = progress.total_download_size;
    } else if (download.declaredTotalBytes && download.declaredTotalBytes > 0) {
      cumulativeBytesTotal = download.declaredTotalBytes;
    } else {
      const knowEveryFileSize =
        progress.total_files > 0 && cumulative.fileSizes.size >= progress.total_files;
      cumulativeBytesTotal = knowEveryFileSize ? knownSizesTotal : knownBytesLowerBound;
      bytesTotalIsLowerBound = !knowEveryFileSize && knownBytesLowerBound > 0;
    }

    // Sum all pre-existing bytes across files for accurate speed calculation
    const totalPreExistingBytes = Array.from(cumulative.preExistingBytes.values())
      .reduce((sum, bytes) => sum + bytes, 0);

    // A restored tab should not treat bytes downloaded before the restore as
    // bytes/sec for this renderer session. Preserve the restored baseline and
    // still honor server-reported resume bytes for genuine resumed downloads.
    const speedBaselineBytes = Math.max(download.bytesResumed || 0, totalPreExistingBytes);

    // Calculate overall percent
    let overallPercent: number;
    if (cumulativeBytesTotal > 0 && !bytesTotalIsLowerBound) {
      // Have byte-level data against a real total: calculate from cumulative bytes
      overallPercent = Math.round((cumulativeBytesDownloaded / cumulativeBytesTotal) * 100);
    } else if (progress.total_files > 0) {
      // No byte data at all: estimate from file count + intra-file percent from server
      const completedFiles = progress.file_index - 1;
      const currentFileProgress = progress.percent / 100;
      overallPercent = Math.round(((completedFiles + currentFileProgress) / progress.total_files) * 100);
    } else {
      overallPercent = 0;
    }

    // Cap percentage at 100% to handle edge cases where byte tracking is incomplete.
    overallPercent = Math.min(overallPercent, 100);

    // Do not display a live download as 100% until the terminal completion signal arrives.
    // Backend installs can spend time extracting or installing runtime follow-up artifacts
    // after the last byte of the current file has arrived.
    const progressIsTerminal = progress.complete === true ||
      (progress.status === 'completed' && progress.running !== true);
    if (!progressIsTerminal && progress.status !== 'error' && progress.status !== 'cancelled' && overallPercent >= 100) {
      overallPercent = 99;
    }

    // Cap bytesDownloaded to not exceed bytesTotal for display consistency
    const displayBytesDownloaded = cumulativeBytesTotal > 0
      ? Math.min(cumulativeBytesDownloaded, cumulativeBytesTotal)
      : cumulativeBytesDownloaded;

    const now = Date.now();
    const speedEligibleBytes = Math.max(0, displayBytesDownloaded - speedBaselineBytes);
    const previousSpeedSampleBytes = download.speedSampleBytes;
    const previousSpeedSampleTime = download.speedSampleTime;
    const baselineChanged = speedBaselineBytes !== (download.bytesResumed || 0);
    let speedBytesPerSecond = download.speedBytesPerSecond ?? 0;

    if ((progress.status && progress.status !== 'downloading') || progress.complete) {
      speedBytesPerSecond = 0;
    } else if (
      !baselineChanged &&
      typeof previousSpeedSampleBytes === 'number' &&
      typeof previousSpeedSampleTime === 'number' &&
      now > previousSpeedSampleTime
    ) {
      const elapsedSeconds = (now - previousSpeedSampleTime) / 1000;
      const deltaBytes = Math.max(0, speedEligibleBytes - previousSpeedSampleBytes);
      speedBytesPerSecond = elapsedSeconds > 0 ? deltaBytes / elapsedSeconds : 0;
    }

    const shouldReleaseLocalOwner = progress.status != null &&
      progress.status !== 'downloading' &&
      progress.running !== true;

    const updatedDownload: DownloadItem = {
      ...download,
      fileName: progress.file,
      fileIndex: progress.file_index,
      totalFiles: progress.total_files,
      bytesDownloaded: displayBytesDownloaded,
      bytesTotal: cumulativeBytesTotal,
      bytesTotalIsLowerBound,
      percent: overallPercent,
      bytesResumed: speedBaselineBytes,
      speedBytesPerSecond,
      speedSampleTime: now,
      speedSampleBytes: speedEligibleBytes,
      status: progress.status ?? download.status,
      running: progress.running ?? download.running,
      error: progress.error ?? download.error,
      abortController: shouldReleaseLocalOwner ? undefined : download.abortController,
      updatedAt: Date.now(),
    };

    this.activeDownloads.set(downloadId, updatedDownload);
    this.emitUpdate(updatedDownload);
  }

  private getDownloadIdFromProgress(progress: DownloadProgressEvent): string | undefined {
    if (progress.id) return progress.id;
    if (!progress.model_name) return undefined;
    return this.getStableDownloadId(progress.model_name, progress.type);
  }

  private isServerActive(progress: DownloadProgressEvent): boolean {
    return progress.running === true || progress.status === 'downloading';
  }

  private getProgressDownloadedBytes(progress: DownloadProgressEvent): number {
    const serverCumulativeBytes = typeof progress.cumulative_bytes_downloaded === 'number'
      ? progress.cumulative_bytes_downloaded
      : (typeof progress.overall_bytes_downloaded === 'number' ? progress.overall_bytes_downloaded : undefined);
    if (typeof serverCumulativeBytes === 'number') {
      return Math.max(0, serverCumulativeBytes);
    }

    const completedFilesBytes = typeof progress.completed_files_bytes === 'number'
      ? progress.completed_files_bytes
      : 0;
    const currentFileBytes = typeof progress.bytes_downloaded === 'number'
      ? progress.bytes_downloaded
      : 0;
    return Math.max(0, completedFilesBytes + currentFileBytes);
  }

  private hasServerPollWork(downloads?: DownloadProgressEvent[]): boolean {
    return (downloads ?? Array.from(this.activeDownloads.values()))
      .some(download => this.isServerActive(download as DownloadProgressEvent));
  }

  private reopenIfServerActive(downloadId: string, progress: DownloadProgressEvent): void {
    if (!this.isServerActive(progress)) return;

    // A stable model id may be reused by a later attempt in another tab. A
    // fresh active server snapshot must therefore override any prior local
    // dismissal/finalization for that same logical model download.
    this.dismissedDownloads.delete(downloadId);
    this.completedDownloadsFinalized.delete(downloadId);
    this.completedModelDownloadsNotified.delete(downloadId);

    const existing = this.activeDownloads.get(downloadId);
    if (existing && existing.status !== 'downloading' && existing.running !== true) {
      this.activeDownloads.delete(downloadId);
      this.cumulativeData.delete(downloadId);
    }
  }

  applyServerDownload(progress: DownloadProgressEvent): void {
    const modelName = progress.model_name;
    const downloadId = this.getDownloadIdFromProgress(progress);
    if (!downloadId || !modelName) return;

    this.reopenIfServerActive(downloadId, progress);
    if (this.dismissedDownloads.has(downloadId)) return;

    this.ensureDownload(downloadId, modelName, progress);
    this.updateProgress(downloadId, progress);

    if ((progress.status === 'completed' || progress.complete) && progress.running !== true) {
      this.emitModelsUpdatedOnce(downloadId, progress);
      this.emitBackendUpdatedOnce(downloadId, progress);
      this.completeDownload(downloadId);
    }
  }

  applyServerDownloads(downloads: DownloadProgressEvent[]): void {
    const serverIds = new Set(
      downloads
        .map(download => this.getDownloadIdFromProgress(download))
        .filter((id): id is string => Boolean(id)),
    );
    downloads.forEach(download => this.applyServerDownload(download));

    for (const [id, download] of Array.from(this.activeDownloads.entries())) {
      if (!(id.startsWith('model:') || id.startsWith('backend:')) || serverIds.has(id)) continue;

      const localOwnerIsGone = !download.abortController || download.abortController.signal.aborted;
      const localRowIsNotActivelyDownloading = download.status !== 'downloading';
      if (localOwnerIsGone || localRowIsNotActivelyDownloading) {
        this.removeLocalDownload(id, download.modelName);
      }
    }

    this.emitSnapshot();
  }

  async hydrateFromServer(options?: { throwOnError?: boolean }): Promise<DownloadProgressEvent[]> {
    try {
      const response = await serverFetch('/downloads', { cache: 'no-store' });
      if (!response.ok) {
        throw new Error(`Failed to fetch downloads: ${response.status} ${response.statusText}`);
      }

      const downloads = await response.json();
      if (!Array.isArray(downloads)) {
        throw new Error('Download snapshot response was not an array');
      }

      this.applyServerDownloads(downloads);
      if (this.serverPollStarted && this.hasServerPollWork(downloads)) {
        this.scheduleServerPoll(ACTIVE_SERVER_DOWNLOAD_POLL_INTERVAL_MS);
      }
      return downloads;
    } catch (error) {
      if (options?.throwOnError) {
        throw error;
      }
      // Polling is best-effort; keep local state on transient server errors.
      return [];
    }
  }

  startServerPolling(): void {
    this.serverPollStarted = true;
    this.scheduleServerPoll(0);
  }

  private scheduleServerPoll(delayMs: number): void {
    if (typeof window === 'undefined') return;
    if (this.serverPollTimer !== undefined) return;

    this.serverPollTimer = window.setTimeout(() => {
      this.serverPollTimer = undefined;
      void this.pollServerDownloads();
    }, delayMs);
  }

  private async pollServerDownloads(): Promise<void> {
    const downloads = await this.hydrateFromServer();
    if (this.hasServerPollWork(downloads) || this.hasServerPollWork()) {
      this.scheduleServerPoll(ACTIVE_SERVER_DOWNLOAD_POLL_INTERVAL_MS);
    }
  }

  // Backwards-compatible name used by existing callers.
  connectServerEvents(): void {
    this.startServerPolling();
  }

  /**
   * Mark download as complete
   */
  completeDownload(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download || this.completedDownloadsFinalized.has(downloadId)) return;

    this.completedDownloadsFinalized.add(downloadId);

    const completedDownload: DownloadItem = {
      ...download,
      status: 'completed',
      running: false,
      percent: 100,
      updatedAt: Date.now(),
    };

    this.activeDownloads.set(downloadId, completedDownload);
    this.emitUpdate(completedDownload);
    this.emitComplete(downloadId);

    // Keep completed rows visible for the same terminal visibility window as
    // the server registry so reload/new-tab snapshots do not re-add a row that
    // this tab removed too early.
    setTimeout(() => {
      if (this.activeDownloads.get(downloadId)?.status === 'completed') {
        this.dismissDownload(downloadId);
        this.removeLocalDownload(downloadId, download.modelName);
      }
    }, TERMINAL_DOWNLOAD_VISIBILITY_MS);
  }

  /**
   * Mark download as failed
   */
  failDownload(downloadId: string, error: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    const failedDownload: DownloadItem = {
      ...download,
      status: 'error',
      running: false,
      error,
      updatedAt: Date.now(),
    };

    this.activeDownloads.set(downloadId, failedDownload);
    this.emitError(downloadId, error);
    this.emitUpdate(failedDownload);

    // Clean up cumulative data
    this.cumulativeData.delete(downloadId);
  }

  /**
   * Pause a download
   */
  pauseDownload(downloadId: string, abort = true): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    if (abort && download.abortController) {
      download.abortController.abort();
    }

    const pausedDownload: DownloadItem = {
      ...download,
      status: 'paused',
      running: abort ? false : download.running,
      abortController: abort ? undefined : download.abortController,
      updatedAt: Date.now(),
    };

    this.activeDownloads.set(downloadId, pausedDownload);
    this.emitUpdate(pausedDownload);
  }

  /**
   * Cancel a download
   */
  cancelDownload(downloadId: string, abort = true): void {
    const download = this.activeDownloads.get(downloadId);
    if (abort && download?.abortController) {
      download.abortController.abort();
    }

    this.dismissDownload(downloadId);
    this.removeLocalDownload(downloadId, download?.modelName, true);
  }

  requestPause(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;
    if (download.abortController) {
      window.dispatchEvent(new CustomEvent('download:paused', { detail: { id: downloadId, modelName: download.modelName } }));
    }
    this.pauseDownload(downloadId, Boolean(download.abortController));
  }

  requestCancel(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;
    if (download.abortController) {
      window.dispatchEvent(new CustomEvent('download:cancelled', { detail: { id: downloadId, modelName: download.modelName } }));
    }
    this.cancelDownload(downloadId, Boolean(download.abortController));
  }

  removeDownload(downloadId: string): void {
    const modelName = this.activeDownloads.get(downloadId)?.modelName;
    this.dismissDownload(downloadId);
    this.removeLocalDownload(downloadId, modelName, true);
  }

  /**
   * Get all active downloads
   */
  getActiveDownloads(): DownloadItem[] {
    return Array.from(this.activeDownloads.values())
      .filter(download => !this.dismissedDownloads.has(download.id));
  }

  /**
   * Get a specific download by ID
   */
  getDownload(downloadId: string): DownloadItem | undefined {
    return this.activeDownloads.get(downloadId);
  }

  /**
   * Get download by model name
   */
  getDownloadByModelName(modelName: string): DownloadItem | undefined {
    return this.getActiveDownloads().find(
      download => download.modelName === modelName
    );
  }

  /**
   * Check if a model is currently being downloaded
   */
  isDownloading(modelName: string): boolean {
    const download = this.getDownloadByModelName(modelName);
    return Boolean(download?.abortController && download.status === 'downloading' && !download.abortController.signal.aborted);
  }

  /**
   * Check whether this tab owns a live request for `modelName`.
   */
  isActive(modelName: string): boolean {
    return this.isDownloading(modelName);
  }

  clearStaleModelDownload(modelName: string): void {
    for (const [id, download] of Array.from(this.activeDownloads.entries())) {
      if (download.modelName === modelName && !download.abortController) {
        this.removeLocalDownload(id, modelName);
      }
    }
  }

  async hasActiveServerDownload(modelName: string): Promise<boolean> {
    const downloads = await this.hydrateFromServer();
    return downloads.some(download => download.model_name === modelName && (
      download.running === true ||
      download.status === 'downloading'
    ));
  }

  private emitModelsUpdatedOnce(downloadId: string, progress: DownloadProgressEvent): void {
    const downloadType = progress.type ?? this.activeDownloads.get(downloadId)?.downloadType;
    if (downloadType !== 'model' || this.completedModelDownloadsNotified.has(downloadId)) return;

    this.completedModelDownloadsNotified.add(downloadId);
    window.dispatchEvent(new CustomEvent('modelsUpdated'));
  }

  private emitBackendUpdatedOnce(downloadId: string, progress: DownloadProgressEvent): void {
    const downloadType = progress.type ?? this.activeDownloads.get(downloadId)?.downloadType;
    if (downloadType !== 'backend' || this.completedBackendDownloadsNotified.has(downloadId)) return;

    this.completedBackendDownloadsNotified.add(downloadId);
    window.dispatchEvent(new CustomEvent('backendsUpdated'));
  }

  private ensureDownload(downloadId: string, modelName: string, progress: DownloadProgressEvent): void {
    if (this.activeDownloads.has(downloadId)) return;
    const restoredBytesDownloaded = this.getProgressDownloadedBytes(progress);

    this.activeDownloads.set(downloadId, {
      id: downloadId,
      modelName,
      fileName: progress.file,
      fileIndex: progress.file_index,
      totalFiles: progress.total_files,
      bytesDownloaded: 0,
      bytesTotal: progress.total_download_size ?? progress.bytes_total ?? 0,
      bytesTotalIsLowerBound: false,
      percent: progress.percent ?? 0,
      status: progress.status ?? 'downloading',
      error: progress.error,
      startTime: Date.now(),
      bytesResumed: restoredBytesDownloaded,
      downloadType: progress.type,
      running: progress.running,
      speedBytesPerSecond: 0,
      speedSampleTime: Date.now(),
      speedSampleBytes: 0,
      updatedAt: Date.now(),
    });
    this.cumulativeData.set(downloadId, {
      completedFilesBytes: 0,
      fileSizes: new Map(),
      preExistingBytes: new Map(),
    });
  }

  private postCrossTabMessage(message: Omit<DownloadCrossTabMessage, 'source'>): void {
    this.crossTabChannel?.postMessage({ ...message, source: this.tabId });
  }

  private handleCrossTabMessage(message: DownloadCrossTabMessage): void {
    if (!message || message.source === this.tabId) return;

    if (message.type === 'remove') {
      this.dismissDownload(message.id);
      this.removeLocalDownload(message.id, message.modelName);
      return;
    }

    if (message.type === 'sync') {
      void this.hydrateFromServer();
    }
  }

  private dismissDownload(downloadId: string): void {
    this.dismissedDownloads.add(downloadId);
  }

  private removeLocalDownload(downloadId: string, modelName?: string, broadcast = false): void {
    this.activeDownloads.delete(downloadId);
    this.cumulativeData.delete(downloadId);
    this.completedDownloadsFinalized.delete(downloadId);
    this.completedModelDownloadsNotified.delete(downloadId);
    this.completedBackendDownloadsNotified.delete(downloadId);
    if (broadcast) {
      this.postCrossTabMessage({ type: 'remove', id: downloadId, modelName });
    }
    window.dispatchEvent(new CustomEvent('download:removed', { detail: { id: downloadId, modelName } }));
    this.emitSnapshot();
  }

  /**
   * Emit download update event
   */
  private emitUpdate(download: DownloadItem): void {
    window.dispatchEvent(
      new CustomEvent('download:update', {
        detail: download,
      })
    );
  }

  /**
   * Emit download complete event
   */
  private emitComplete(downloadId: string): void {
    window.dispatchEvent(
      new CustomEvent('download:complete', {
        detail: { id: downloadId },
      })
    );
  }

  /**
   * Emit download error event
   */
  private emitError(downloadId: string, error: string): void {
    window.dispatchEvent(
      new CustomEvent('download:error', {
        detail: { id: downloadId, error },
      })
    );
  }

  private emitSnapshot(): void {
    window.dispatchEvent(new CustomEvent('download:snapshot', { detail: { downloads: this.getActiveDownloads() } }));
  }
}

// Create and export a singleton instance
export const downloadTracker = new DownloadTracker();
