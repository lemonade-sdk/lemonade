import { DownloadItem } from '../DownloadManager';

export interface DownloadProgressEvent {
  file: string;
  file_index: number;
  total_files: number;
  bytes_downloaded: number;
  bytes_total: number;
  percent: number;
  total_download_size?: number;  // Total bytes across ALL files in this download
  bytes_previously_downloaded?: number;  // Bytes already on disk for current file (resume/skip)
}

class DownloadTracker {
  private activeDownloads: Map<string, DownloadItem>;
  // Track cumulative data per download
  private cumulativeData: Map<string, {
    completedFilesBytes: number;  // Total bytes from completed files
    fileSizes: Map<number, number>;  // Map of file_index -> file size
    preExistingBytes: Map<number, number>;  // Map of file_index -> bytes already on disk
  }>;

  constructor() {
    this.activeDownloads = new Map();
    this.cumulativeData = new Map();
  }

  /**
   * Start tracking a new download
   */
  startDownload(modelName: string, abortController: AbortController, downloadType?: 'model' | 'backend'): string {
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
        this.activeDownloads.delete(id);
        this.cumulativeData.delete(id);
      }
    }

    const downloadId = `${modelName}-${Date.now()}`;

    const downloadItem: DownloadItem = {
      id: downloadId,
      modelName,
      fileName: '',
      fileIndex: 0,
      totalFiles: 0,
      bytesDownloaded: 0,
      bytesTotal: 0,
      percent: 0,
      status: 'downloading',
      startTime: Date.now(),
      bytesResumed: 0,
      abortController,
      downloadType,
    };

    this.activeDownloads.set(downloadId, downloadItem);
    this.cumulativeData.set(downloadId, {
      completedFilesBytes: 0,
      fileSizes: new Map(),
      preExistingBytes: new Map(),
    });
    this.emitUpdate(downloadItem);

    return downloadId;
  }

  /**
   * Update download progress
   */
  updateProgress(downloadId: string, progress: DownloadProgressEvent): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

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

    // If we moved to a new file, add the previous file's size to completed bytes
    if (progress.file_index > download.fileIndex) {
      // Only add the previous file's size if we have it tracked
      // Use 0 as fallback - if we never got byte data for a file, we can't count it
      // Note: download.bytesTotal is the CUMULATIVE total, not the individual file size!
      const previousFileSize = cumulative.fileSizes.get(download.fileIndex) || 0;
      cumulative.completedFilesBytes += previousFileSize;
    }

    // Calculate cumulative totals
    const cumulativeBytesDownloaded = cumulative.completedFilesBytes + progress.bytes_downloaded;

    // Determine total download size:
    // 1. Server-reported total (covers all files) — best option
    // 2. Local sum of known file sizes — only accurate once all files have been seen
    // 3. File-count-based estimation — use known sizes to estimate unknown files
    let cumulativeBytesTotal: number;
    if (progress.total_download_size && progress.total_download_size > 0) {
      cumulativeBytesTotal = progress.total_download_size;
    } else {
      const knownSizes = Array.from(cumulative.fileSizes.values());
      const knownTotal = knownSizes.reduce((sum, size) => sum + size, 0);
      const knownCount = knownSizes.filter(s => s > 0).length;

      if (knownCount > 0 && progress.total_files > knownCount) {
        // Estimate total: extrapolate from known file sizes to all files
        const avgFileSize = knownTotal / knownCount;
        cumulativeBytesTotal = knownTotal + avgFileSize * (progress.total_files - knownCount);
      } else {
        cumulativeBytesTotal = knownTotal;
      }
    }

    // Sum all pre-existing bytes across files for accurate speed calculation
    const totalPreExistingBytes = Array.from(cumulative.preExistingBytes.values())
      .reduce((sum, bytes) => sum + bytes, 0);

    // Calculate overall percent
    let overallPercent: number;
    if (cumulativeBytesTotal > 0) {
      // Have byte-level data: calculate from cumulative bytes
      overallPercent = Math.round((cumulativeBytesDownloaded / cumulativeBytesTotal) * 100);
    } else if (progress.total_files > 0) {
      // No byte data at all: estimate from file count + intra-file percent from server
      const completedFiles = progress.file_index - 1;
      const currentFileProgress = progress.percent / 100;
      overallPercent = Math.round(((completedFiles + currentFileProgress) / progress.total_files) * 100);
    } else {
      overallPercent = 0;
    }

    // Cap percentage at 100% to handle edge cases where byte tracking is incomplete
    overallPercent = Math.min(overallPercent, 100);

    // Cap bytesDownloaded to not exceed bytesTotal for display consistency
    const displayBytesDownloaded = cumulativeBytesTotal > 0
      ? Math.min(cumulativeBytesDownloaded, cumulativeBytesTotal)
      : cumulativeBytesDownloaded;

    const updatedDownload: DownloadItem = {
      ...download,
      fileName: progress.file,
      fileIndex: progress.file_index,
      totalFiles: progress.total_files,
      bytesDownloaded: displayBytesDownloaded,
      bytesTotal: cumulativeBytesTotal,
      percent: overallPercent,
      bytesResumed: totalPreExistingBytes,
    };

    this.activeDownloads.set(downloadId, updatedDownload);
    this.emitUpdate(updatedDownload);
  }

  /**
   * Mark download as complete
   */
  completeDownload(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    const completedDownload: DownloadItem = {
      ...download,
      status: 'completed',
      percent: 100,
    };

    this.activeDownloads.set(downloadId, completedDownload);
    this.emitComplete(downloadId);

    // Remove from active downloads and cumulative data after a delay
    setTimeout(() => {
      this.activeDownloads.delete(downloadId);
      this.cumulativeData.delete(downloadId);
    }, 1000);
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
      error,
    };

    this.activeDownloads.set(downloadId, failedDownload);
    this.emitError(downloadId, error);

    // Clean up cumulative data
    this.cumulativeData.delete(downloadId);
  }

  /**
   * Pause a download
   */
  pauseDownload(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    if (download.abortController) {
      download.abortController.abort();
    }

    const pausedDownload: DownloadItem = {
      ...download,
      status: 'paused',
    };

    this.activeDownloads.set(downloadId, pausedDownload);
    this.emitUpdate(pausedDownload);
  }

  /**
   * Cancel a download
   */
  cancelDownload(downloadId: string): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    if (download.abortController) {
      download.abortController.abort();
    }

    const cancelledDownload: DownloadItem = {
      ...download,
      status: 'cancelled',
    };

    this.activeDownloads.set(downloadId, cancelledDownload);
    this.emitUpdate(cancelledDownload);

    // Clean up cumulative data
    this.cumulativeData.delete(downloadId);
  }

  /**
   * Get all active downloads
   */
  getActiveDownloads(): DownloadItem[] {
    return Array.from(this.activeDownloads.values());
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
    return Array.from(this.activeDownloads.values()).find(
      download => download.modelName === modelName
    );
  }

  /**
   * Check if a model is currently being downloaded
   */
  isDownloading(modelName: string): boolean {
    const download = this.getDownloadByModelName(modelName);
    return download?.status === 'downloading';
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
}

// Create and export a singleton instance
export const downloadTracker = new DownloadTracker();
