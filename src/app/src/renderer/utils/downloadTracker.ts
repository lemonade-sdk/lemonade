import { DownloadItem } from '../DownloadManager';

export interface DownloadProgressEvent {
  file: string;
  file_index: number;
  total_files: number;
  bytes_downloaded: number;
  bytes_total: number;
  percent: number;
}

class DownloadTracker {
  private activeDownloads: Map<string, DownloadItem>;
  // Track cumulative data per download
  private cumulativeData: Map<string, {
    completedFilesBytes: number;  // Total bytes from completed files
    fileSizes: Map<number, number>;  // Map of file_index -> file size
  }>;

  constructor() {
    this.activeDownloads = new Map();
    this.cumulativeData = new Map();
  }

  /**
   * Start tracking a new download
   */
  startDownload(modelName: string, abortController: AbortController): string {
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
      abortController,
    };

    this.activeDownloads.set(downloadId, downloadItem);
    this.cumulativeData.set(downloadId, {
      completedFilesBytes: 0,
      fileSizes: new Map(),
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
    const cumulativeBytesTotal = Array.from(cumulative.fileSizes.values()).reduce((sum, size) => sum + size, 0);

    // Calculate overall percent
    let overallPercent: number;
    if (cumulativeBytesTotal > 0) {
      // Have byte-level data: calculate from cumulative bytes
      overallPercent = Math.round((cumulativeBytesDownloaded / cumulativeBytesTotal) * 100);
    } else if (progress.total_files > 0) {
      // No byte data: estimate from file count + intra-file percent from server
      // (file_index - 1) completed files + current file progress (percent/100)
      const completedFiles = progress.file_index - 1;
      const currentFileProgress = progress.percent / 100;  // Server sends intra-file percent
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

/**
 * Helper function to start a download with SSE tracking
 */
export async function trackDownload(
  modelName: string,
  url: string,
  requestInit: RequestInit
): Promise<void> {
  const abortController = new AbortController();
  const downloadId = downloadTracker.startDownload(modelName, abortController);

  try {
    const response = await fetch(url, {
      ...requestInit,
      signal: abortController.signal,
    });

    if (!response.ok) {
      throw new Error(`Failed to download: ${response.statusText}`);
    }

    const reader = response.body?.getReader();
    if (!reader) {
      throw new Error('No response body');
    }

    const decoder = new TextDecoder();
    let buffer = '';
    let currentEventType = 'progress';

    while (true) {
      const { done, value } = await reader.read();

      if (done) break;

      buffer += decoder.decode(value, { stream: true });
      const lines = buffer.split('\n');
      buffer = lines.pop() || '';

      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];

        if (line.startsWith('event:')) {
          currentEventType = line.substring(6).trim();
        } else if (line.startsWith('data:')) {
          try {
            const data = JSON.parse(line.substring(5).trim());

            if (currentEventType === 'progress') {
              downloadTracker.updateProgress(downloadId, data);
            } else if (currentEventType === 'complete') {
              downloadTracker.completeDownload(downloadId);
              return;
            } else if (currentEventType === 'error') {
              downloadTracker.failDownload(downloadId, data.error || 'Unknown error');
              throw new Error(data.error || 'Download failed');
            }
          } catch (parseError) {
            console.error('Failed to parse SSE data:', line, parseError);
          }
        } else if (line.trim() === '') {
          // Empty line resets event type to default
          currentEventType = 'progress';
        }
      }
    }

    downloadTracker.completeDownload(downloadId);
  } catch (error: any) {
    if (error.name === 'AbortError') {
      // Download was cancelled
      downloadTracker.cancelDownload(downloadId);

      // Dispatch event to signal that download is fully cleaned up and file handles are released
      window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
        detail: { id: downloadId, modelName }
      }));
    } else {
      downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
      throw error;
    }
  }
}
