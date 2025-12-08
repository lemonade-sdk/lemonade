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

  constructor() {
    this.activeDownloads = new Map();
  }

  /**
   * Start tracking a new download
   */
  startDownload(modelName: string, abortController: AbortController): string {
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
    this.emitUpdate(downloadItem);
    
    return downloadId;
  }

  /**
   * Update download progress
   */
  updateProgress(downloadId: string, progress: DownloadProgressEvent): void {
    const download = this.activeDownloads.get(downloadId);
    if (!download) return;

    const updatedDownload: DownloadItem = {
      ...download,
      fileName: progress.file,
      fileIndex: progress.file_index,
      totalFiles: progress.total_files,
      bytesDownloaded: progress.bytes_downloaded,
      bytesTotal: progress.bytes_total,
      percent: progress.percent,
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
    
    // Remove from active downloads after a delay
    setTimeout(() => {
      this.activeDownloads.delete(downloadId);
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

    while (true) {
      const { done, value } = await reader.read();
      
      if (done) break;

      buffer += decoder.decode(value, { stream: true });
      const lines = buffer.split('\n');
      buffer = lines.pop() || '';

      for (const line of lines) {
        if (line.startsWith('event:')) {
          const eventType = line.substring(6).trim();
          
          // Get next line which should be data
          const nextLineIndex = lines.indexOf(line) + 1;
          if (nextLineIndex < lines.length) {
            const dataLine = lines[nextLineIndex];
            if (dataLine.startsWith('data:')) {
              const data = JSON.parse(dataLine.substring(5).trim());
              
              if (eventType === 'progress') {
                downloadTracker.updateProgress(downloadId, data);
              } else if (eventType === 'complete') {
                downloadTracker.completeDownload(downloadId);
                return;
              } else if (eventType === 'error') {
                downloadTracker.failDownload(downloadId, data.error || 'Unknown error');
                throw new Error(data.error || 'Download failed');
              }
            }
          }
        }
      }
    }

    downloadTracker.completeDownload(downloadId);
  } catch (error: any) {
    if (error.name === 'AbortError') {
      // Download was cancelled
      downloadTracker.cancelDownload(downloadId);
    } else {
      downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
      throw error;
    }
  }
}

