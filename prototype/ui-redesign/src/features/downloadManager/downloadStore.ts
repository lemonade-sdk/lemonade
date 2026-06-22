import api, { DownloadProgressEvent } from '../../api';

export type DownloadStatus = 'downloading' | 'paused' | 'completed' | 'error' | 'cancelled' | 'deleting';
export type DownloadType = 'model' | 'backend';

type NumericRecord = Record<string, number>;

export interface DownloadListItem {
  id: string;
  downloadType: DownloadType;
  modelName: string;
  fileName: string;
  fileIndex: number;
  totalFiles: number;
  bytesDownloaded: number;
  bytesTotal: number;
  bytesTotalIsLowerBound?: boolean;
  percent: number;
  status: DownloadStatus;
  error?: string;
  startTime: number;
  bytesResumed: number;
  running?: boolean;
  speedBytesPerSecond?: number;
  speedSampleTime?: number;
  speedSampleBytes?: number;
  collectionComponents?: string[];
  declaredTotalBytes?: number;
  completedFilesBytes?: number;
  knownFileSizes?: NumericRecord;
  preExistingBytes?: NumericRecord;
  updatedAt: number;
  terminalAt?: number;
  raw?: DownloadProgressEvent;
}

type Listener = (downloads: DownloadListItem[]) => void;

const STORAGE_KEY = 'lemonade_download_manager_items_v1';
const DISMISSED_STORAGE_KEY = 'lemonade_download_manager_dismissed_v1';
const TERMINAL_TTL_MS = 60 * 60 * 1000;
const POLL_MS = 1000;
const SPEED_SMOOTHING_ALPHA = 0.35;

function now(): number { return Date.now(); }

function finiteNumber(value: unknown, fallback = 0): number {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function optionalNumber(value: unknown): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) ? n : undefined;
}

function positiveInt(value: unknown, fallback: number): number {
  const n = Math.floor(finiteNumber(value, fallback));
  return n > 0 ? n : fallback;
}

function clampPercent(value: unknown): number {
  const n = finiteNumber(value, 0);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.min(100, n));
}

function sumRecord(values: NumericRecord | undefined): number {
  if (!values) return 0;
  return Object.values(values).reduce((sum, value) => sum + (Number.isFinite(value) && value > 0 ? value : 0), 0);
}

function recordSize(values: NumericRecord | undefined): number {
  if (!values) return 0;
  return Object.keys(values).length;
}

function getProgressDownloadedBytes(raw: DownloadProgressEvent): number {
  const serverCumulativeBytes = optionalNumber(raw.cumulative_bytes_downloaded)
    ?? optionalNumber(raw.overall_bytes_downloaded)
    ?? optionalNumber((raw as any).cumulativeBytesDownloaded)
    ?? optionalNumber((raw as any).overallBytesDownloaded);
  if (serverCumulativeBytes != null) return Math.max(0, serverCumulativeBytes);

  const completedFilesBytes = optionalNumber((raw as any).completed_files_bytes ?? (raw as any).completedFilesBytes) ?? 0;
  const currentFileBytes = optionalNumber(raw.bytes_downloaded ?? (raw as any).bytesDownloaded) ?? 0;
  return Math.max(0, completedFilesBytes + currentFileBytes);
}

function resetActiveSpeedBaseline(download: DownloadListItem, timestamp = now()): DownloadListItem {
  if (!isDownloadActive(download)) return download;
  return {
    ...download,
    startTime: timestamp,
    bytesResumed: Math.max(0, download.bytesDownloaded || 0),
    speedBytesPerSecond: 0,
    speedSampleTime: timestamp,
    speedSampleBytes: 0,
    updatedAt: timestamp,
  };
}

export function isDownloadTerminal(download: Pick<DownloadListItem, 'status' | 'running'>): boolean {
  return download.running !== true && (
    download.status === 'completed'
    || download.status === 'error'
    || download.status === 'cancelled'
  );
}

export function isDownloadActive(download: Pick<DownloadListItem, 'status' | 'running'>): boolean {
  return download.running === true || download.status === 'downloading';
}

function normalizeDownloadType(raw: DownloadProgressEvent): DownloadType {
  const type = String(raw.type || '').toLowerCase();
  const id = String(raw.id || '').toLowerCase();
  if (type === 'backend' || id.startsWith('backend:')) return 'backend';
  return 'model';
}

function nameFromDownload(raw: DownloadProgressEvent, type: DownloadType): string {
  const id = String(raw.id || '');
  const name = String(raw.model_name || raw.name || '').trim();
  if (name) return name;
  if (type === 'model' && id.startsWith('model:')) return id.slice('model:'.length);
  if (type === 'backend' && id.startsWith('backend:')) return id.slice('backend:'.length);
  return id;
}

function idFromDownload(raw: DownloadProgressEvent, type: DownloadType, modelName: string): string {
  const rawId = String(raw.id || '').trim();
  const name = modelName.trim();
  const stable = name ? `${type}:${name}` : '';

  // Lemonade main keeps one logical row per model/backend. Some server/SSE
  // payloads include per-file ids that still begin with model:/backend:; using
  // those ids would split one pull into multiple UI rows and leave the model row
  // progress on the raw per-file percentage. Prefer the caller/server model name
  // whenever we have it, and only fall back to the raw id when there is no stable
  // logical name to key by.
  if (stable) return stable;
  return rawId;
}

function payloadErrorMessage(raw: DownloadProgressEvent): string | undefined {
  const statusValue = (raw as any).status;
  const s = String(statusValue || '').toLowerCase();
  const message = (raw as any).message || (raw as any).detail;
  const httpStatus = optionalNumber(
    (raw as any).status_code
    ?? (raw as any).statusCode
    ?? (raw as any).http_status
    ?? (raw as any).httpStatus
    ?? (raw as any).code
    ?? (raw as any).error_code
    ?? (raw as any).errorCode,
  );
  const numericStatus = typeof statusValue === 'number' ? statusValue : optionalNumber(/^\d{3}$/.test(s) ? s : undefined);
  const code = httpStatus ?? numericStatus;
  const messageText = typeof message === 'string' ? message.trim() : '';

  const rawError = (raw as any).error;
  let errorText = '';
  if (typeof rawError === 'string' && rawError.trim()) {
    errorText = rawError.trim();
  } else if (rawError && typeof rawError === 'object' && !Array.isArray(rawError)) {
    const nested = (rawError as any).message || (rawError as any).error || (rawError as any).detail;
    if (typeof nested === 'string' && nested.trim()) errorText = nested.trim();
  }

  const failed = Boolean(errorText)
    || (raw as any).ok === false
    || (code != null && code >= 400)
    || s === 'error'
    || s === 'failed'
    || s === 'failure'
    || s === 'not_found'
    || s === 'not-found'
    || /(^|\D)404(\D|$)|not[ _-]?found/i.test(`${s} ${messageText} ${errorText}`);
  if (!failed) return undefined;

  const detail = errorText || messageText;
  if (code != null && code >= 400) {
    if (detail && !new RegExp(`(^|\\D)${code}(\\D|$)`).test(detail)) {
      return `HTTP ${code}: ${detail}`;
    }
    return detail || `Download failed with HTTP ${code}.`;
  }
  return detail || 'Download failed.';
}

function statusFromDownload(raw: DownloadProgressEvent): DownloadStatus {
  const statusValue = (raw as any).status;
  const s = String(statusValue || '').toLowerCase();
  if (payloadErrorMessage(raw)) return 'error';
  if (raw.complete === true || s === 'completed' || s === 'complete' || s === 'success' || s === 'done') return 'completed';
  if (s === 'paused' || s === 'pausing') return 'paused';
  if (s === 'cancelled' || s === 'canceled' || s === 'canceling' || s === 'cancelling') return 'cancelled';
  if (s === 'deleting') return 'deleting';
  return 'downloading';
}

function normalizeCollectionComponents(raw: DownloadProgressEvent): string[] | undefined {
  const values = (raw as any).collection_components ?? (raw as any).collectionComponents ?? (raw as any).components;
  if (!Array.isArray(values)) return undefined;
  const components = values.map(item => String(item || '').trim()).filter(Boolean);
  return components.length > 0 ? components : undefined;
}

function calculateProgress(raw: DownloadProgressEvent, previous: DownloadListItem | undefined, status: DownloadStatus) {
  const fileIndex = positiveInt(raw.file_index ?? (raw as any).fileIndex, previous?.fileIndex || 1);
  const totalFiles = positiveInt(raw.total_files ?? (raw as any).totalFiles, previous?.totalFiles || 1);
  const previousCurrentFileBytes = previous && fileIndex === previous.fileIndex ? finiteNumber(previous.raw?.bytes_downloaded, 0) : 0;
  const currentFileBytes = Math.max(0, finiteNumber(raw.bytes_downloaded, previousCurrentFileBytes));
  const currentFileTotal = optionalNumber(raw.bytes_total);
  const rawFilePercent = optionalNumber(raw.percent);
  const previousFilePercent = previous && fileIndex === previous.fileIndex ? optionalNumber(previous.raw?.percent) : undefined;
  const currentFilePercent = rawFilePercent
    ?? (currentFileTotal && currentFileTotal > 0 ? (currentFileBytes / currentFileTotal) * 100 : undefined)
    ?? previousFilePercent;

  const knownFileSizes: NumericRecord = { ...(previous?.knownFileSizes || {}) };
  if (currentFileTotal && currentFileTotal > 0) knownFileSizes[String(fileIndex)] = currentFileTotal;

  const preExistingBytes: NumericRecord = { ...(previous?.preExistingBytes || {}) };
  const bytesPreviouslyDownloaded = optionalNumber((raw as any).bytes_previously_downloaded ?? (raw as any).bytesPreviouslyDownloaded);
  if (bytesPreviouslyDownloaded && bytesPreviouslyDownloaded > 0 && preExistingBytes[String(fileIndex)] == null) {
    preExistingBytes[String(fileIndex)] = bytesPreviouslyDownloaded;
  }

  const previousCompleted = Math.max(0, previous?.completedFilesBytes || 0);
  const serverCompletedFilesBytes = optionalNumber((raw as any).completed_files_bytes ?? (raw as any).completedFilesBytes);
  let completedFilesBytes = previousCompleted;
  if (serverCompletedFilesBytes != null) {
    completedFilesBytes = Math.max(previousCompleted, serverCompletedFilesBytes);
  } else if (previous && fileIndex > previous.fileIndex) {
    const previousFileSize = previous.knownFileSizes?.[String(previous.fileIndex)]
      ?? optionalNumber(previous.raw?.bytes_total)
      ?? 0;
    completedFilesBytes = previousCompleted + previousFileSize;
  }

  const serverCumulativeBytes = optionalNumber(raw.cumulative_bytes_downloaded)
    ?? optionalNumber(raw.overall_bytes_downloaded)
    ?? optionalNumber((raw as any).cumulativeBytesDownloaded)
    ?? optionalNumber((raw as any).overallBytesDownloaded);
  const bytesDownloaded = Math.max(0, serverCumulativeBytes ?? (completedFilesBytes + currentFileBytes));

  const declaredTotalBytes = optionalNumber((raw as any).declared_total_bytes ?? (raw as any).declaredTotalBytes) ?? previous?.declaredTotalBytes;
  const serverTotalBytes = optionalNumber(raw.total_download_size ?? (raw as any).totalDownloadSize);
  const knownSizesTotal = sumRecord(knownFileSizes);
  const knowsEveryFileSize = totalFiles > 0 && recordSize(knownFileSizes) >= totalFiles;
  const isMultiFile = totalFiles > 1;

  let bytesTotal = 0;
  let bytesTotalIsLowerBound = false;
  let hasRealTotal = false;

  if (serverTotalBytes && serverTotalBytes > 0) {
    bytesTotal = serverTotalBytes;
    hasRealTotal = true;
  } else if (declaredTotalBytes && declaredTotalBytes > 0) {
    bytesTotal = declaredTotalBytes;
    hasRealTotal = true;
  } else if (isMultiFile) {
    if (knowsEveryFileSize && knownSizesTotal > 0) {
      bytesTotal = knownSizesTotal;
      hasRealTotal = true;
    } else {
      bytesTotal = Math.max(bytesDownloaded, knownSizesTotal, previous?.bytesTotal || 0);
      bytesTotalIsLowerBound = bytesTotal > 0;
    }
  } else if (currentFileTotal && currentFileTotal > 0) {
    bytesTotal = currentFileTotal;
    hasRealTotal = true;
  } else {
    bytesTotal = Math.max(bytesDownloaded, previous?.bytesTotal || 0);
    bytesTotalIsLowerBound = bytesTotal > 0;
  }

  let percent = 0;
  if (status === 'completed') {
    percent = 100;
  } else if (hasRealTotal && bytesTotal > 0) {
    percent = (bytesDownloaded / bytesTotal) * 100;
  } else if (isMultiFile) {
    const completedFiles = Math.max(0, fileIndex - 1);
    const perFilePercent = clampPercent(currentFilePercent ?? 0) / 100;
    percent = ((completedFiles + perFilePercent) / totalFiles) * 100;
  } else if (currentFilePercent != null) {
    percent = currentFilePercent;
  } else if (bytesTotal > 0) {
    percent = (bytesDownloaded / bytesTotal) * 100;
  }

  const progressIsTerminal = status === 'completed' || status === 'error' || status === 'cancelled' || raw.complete === true;
  percent = clampPercent(percent);
  if (!progressIsTerminal && percent >= 100) percent = 99;

  const totalPreExistingBytes = sumRecord(preExistingBytes);
  const restoredBaselineBytes = previous ? 0 : getProgressDownloadedBytes(raw);
  const speedBaselineBytes = Math.max(previous?.bytesResumed || 0, totalPreExistingBytes, restoredBaselineBytes);
  const displayBytesDownloaded = bytesTotal > 0 && hasRealTotal ? Math.min(bytesDownloaded, bytesTotal) : bytesDownloaded;
  const speedEligibleBytes = Math.max(0, displayBytesDownloaded - speedBaselineBytes);
  const timestamp = now();
  let speedBytesPerSecond = previous?.speedBytesPerSecond || 0;
  const previousSampleBytes = previous?.speedSampleBytes;
  const previousSampleTime = previous?.speedSampleTime;
  const baselineChanged = speedBaselineBytes !== (previous?.bytesResumed || 0);

  if (status !== 'downloading' || progressIsTerminal) {
    speedBytesPerSecond = 0;
  } else if (baselineChanged) {
    speedBytesPerSecond = 0;
  } else if (typeof previousSampleBytes === 'number' && typeof previousSampleTime === 'number' && timestamp > previousSampleTime) {
    const elapsedSeconds = (timestamp - previousSampleTime) / 1000;
    const deltaBytes = Math.max(0, speedEligibleBytes - previousSampleBytes);
    const instantSpeed = elapsedSeconds > 0 ? deltaBytes / elapsedSeconds : 0;
    speedBytesPerSecond = speedBytesPerSecond > 0
      ? (speedBytesPerSecond * (1 - SPEED_SMOOTHING_ALPHA)) + (instantSpeed * SPEED_SMOOTHING_ALPHA)
      : instantSpeed;
  }

  return {
    fileIndex,
    totalFiles,
    bytesDownloaded: displayBytesDownloaded,
    bytesTotal,
    bytesTotalIsLowerBound,
    percent,
    completedFilesBytes,
    knownFileSizes,
    preExistingBytes,
    declaredTotalBytes,
    bytesResumed: speedBaselineBytes,
    speedBytesPerSecond,
    speedSampleTime: timestamp,
    speedSampleBytes: speedEligibleBytes,
  };
}

export function normalizeDownload(raw: DownloadProgressEvent, previous?: DownloadListItem): DownloadListItem | null {
  const type = normalizeDownloadType(raw);
  const modelName = nameFromDownload(raw, type);
  const id = idFromDownload(raw, type, modelName);
  if (!id && !modelName) return null;
  const timestamp = now();
  const status = statusFromDownload(raw);
  const progress = calculateProgress(raw, previous, status);
  let running = typeof raw.running === 'boolean' ? raw.running : previous?.running;
  if (status === 'completed' || status === 'error' || status === 'cancelled') {
    running = false;
  }
  const terminalAt = isDownloadTerminal({ status, running })
    ? (previous?.terminalAt || previous?.updatedAt || timestamp)
    : undefined;
  const startTime = previous?.startTime
    ?? (status === 'downloading' ? timestamp : finiteNumber((raw as any).start_time ?? (raw as any).startTime, timestamp));
  const normalizedError = payloadErrorMessage(raw) || (typeof raw.error === 'string' ? raw.error : undefined);
  const error = normalizedError && normalizedError !== 'Download failed.'
    ? normalizedError
    : (previous?.error || normalizedError);

  return {
    id: id || `${type}:${modelName}`,
    downloadType: type,
    modelName: modelName || id,
    fileName: String(raw.file || (raw as any).file_name || (raw as any).filename || previous?.fileName || '').trim(),
    fileIndex: progress.fileIndex,
    totalFiles: progress.totalFiles,
    bytesDownloaded: progress.bytesDownloaded,
    bytesTotal: progress.bytesTotal,
    bytesTotalIsLowerBound: progress.bytesTotalIsLowerBound,
    percent: progress.percent,
    status,
    error,
    startTime,
    bytesResumed: progress.bytesResumed,
    running,
    speedBytesPerSecond: progress.speedBytesPerSecond,
    speedSampleTime: progress.speedSampleTime,
    speedSampleBytes: progress.speedSampleBytes,
    collectionComponents: normalizeCollectionComponents(raw) || previous?.collectionComponents,
    declaredTotalBytes: progress.declaredTotalBytes,
    completedFilesBytes: progress.completedFilesBytes,
    knownFileSizes: progress.knownFileSizes,
    preExistingBytes: progress.preExistingBytes,
    updatedAt: timestamp,
    terminalAt,
    raw,
  };
}

function sortDownloads(downloads: DownloadListItem[]): DownloadListItem[] {
  return [...downloads].sort((a, b) => {
    const aActive = isDownloadActive(a);
    const bActive = isDownloadActive(b);
    if (aActive !== bActive) return aActive ? -1 : 1;
    const aTerminal = isDownloadTerminal(a);
    const bTerminal = isDownloadTerminal(b);
    if (aTerminal !== bTerminal) return aTerminal ? 1 : -1;
    return b.updatedAt - a.updatedAt;
  });
}

function prune(downloads: DownloadListItem[], timestamp = now()): DownloadListItem[] {
  return downloads.filter(download => {
    if (!isDownloadTerminal(download)) return true;
    const terminalAt = download.terminalAt || download.updatedAt;
    return timestamp - terminalAt < TERMINAL_TTL_MS;
  });
}

function coerceStoredDownload(download: DownloadListItem, timestamp = now()): DownloadListItem {
  if (download.status === 'completed' || download.status === 'error' || download.status === 'cancelled') {
    return {
      ...download,
      running: false,
      speedBytesPerSecond: 0,
      terminalAt: download.terminalAt || download.updatedAt || timestamp,
    };
  }
  return download;
}

function readStored(resetActiveBaselines = false): DownloadListItem[] {
  if (typeof localStorage === 'undefined') return [];
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    const timestamp = now();
    const stored = prune((parsed.filter(Boolean) as DownloadListItem[]).map(item => coerceStoredDownload(item, timestamp)), timestamp);
    if (!resetActiveBaselines) return stored;
    return stored.map(download => resetActiveSpeedBaseline(download, timestamp));
  } catch {
    return [];
  }
}

function writeStored(downloads: DownloadListItem[]): void {
  if (typeof localStorage === 'undefined') return;
  try {
    const visible = sortDownloads(prune(downloads));
    const serialized = JSON.stringify(visible);
    if (localStorage.getItem(STORAGE_KEY) !== serialized) {
      localStorage.setItem(STORAGE_KEY, serialized);
    }
  } catch {
    // Storage may be unavailable in privacy modes. The live in-memory store still works.
  }
}

function readDismissed(timestamp = now()): Record<string, number> {
  if (typeof localStorage === 'undefined') return {};
  try {
    const raw = localStorage.getItem(DISMISSED_STORAGE_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
    const kept: Record<string, number> = {};
    Object.entries(parsed as Record<string, unknown>).forEach(([id, value]) => {
      const dismissedAt = finiteNumber(value, 0);
      if (dismissedAt > 0 && timestamp - dismissedAt < TERMINAL_TTL_MS) kept[id] = dismissedAt;
    });
    return kept;
  } catch {
    return {};
  }
}

function writeDismissed(dismissed: Record<string, number>): void {
  if (typeof localStorage === 'undefined') return;
  try {
    const serialized = JSON.stringify(dismissed);
    if (localStorage.getItem(DISMISSED_STORAGE_KEY) !== serialized) {
      localStorage.setItem(DISMISSED_STORAGE_KEY, serialized);
    }
  } catch {
    // Ignore unavailable storage; the in-memory removal still applies.
  }
}

function isDismissedTerminal(download: DownloadListItem, dismissed: Record<string, number>): boolean {
  return Boolean(dismissed[download.id]) && !isDownloadActive(download);
}

class DownloadStore {
  private downloads: DownloadListItem[] = [];
  private listeners = new Set<Listener>();
  private pollTimer: ReturnType<typeof setTimeout> | null = null;
  private refreshInFlight: Promise<void> | null = null;
  private started = false;

  constructor() {
    this.downloads = sortDownloads(readStored(false));
    if (typeof window !== 'undefined') {
      window.addEventListener('storage', (event) => {
        if (event.key !== STORAGE_KEY && event.key !== DISMISSED_STORAGE_KEY) return;
        this.mergeDownloads([], true);
      });
      window.addEventListener('focus', () => { if (this.started) void this.refresh(); });
      window.addEventListener('online', () => { if (this.started) void this.refresh(); });
    }
  }

  snapshot(): DownloadListItem[] {
    return this.downloads;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.downloads);
    this.start();
    return () => {
      this.listeners.delete(listener);
    };
  }

  start(): void {
    if (this.started) return;
    this.started = true;
    void this.refresh();
  }

  async refresh(): Promise<void> {
    if (this.refreshInFlight) return this.refreshInFlight;
    this.refreshInFlight = (async () => {
      if (!api.isConnected) {
        this.mergeDownloads([], true);
        this.scheduleNext();
        return;
      }
      try {
        const serverDownloads = await api.downloads();
        const normalized = serverDownloads
          .map(raw => normalizeDownload(raw, this.findExisting(raw)))
          .filter((item): item is DownloadListItem => Boolean(item));
        this.mergeDownloads(normalized, true);
      } catch {
        this.mergeDownloads([], true);
      } finally {
        this.scheduleNext();
      }
    })();
    try {
      await this.refreshInFlight;
    } finally {
      this.refreshInFlight = null;
    }
  }

  upsertFromPull(modelName: string, data: Record<string, unknown> = {}, type: DownloadType = 'model'): DownloadListItem | null {
    const id = String(data.id || `${type}:${modelName}`);
    const rawStatus = data.status as DownloadProgressEvent['status'] | undefined;
    const raw: DownloadProgressEvent = {
      ...(data as DownloadProgressEvent),
      id,
      type,
      model_name: modelName,
      status: rawStatus || (data.complete === true ? 'completed' : 'downloading'),
    };
    if (payloadErrorMessage(raw)) {
      raw.status = 'error';
      raw.complete = false;
    }
    const percent = optionalNumber(data.percent);
    if (percent != null) raw.percent = percent;
    const previous = this.findExisting(raw);
    const normalized = normalizeDownload(raw, previous);
    if (normalized) this.mergeDownloads([normalized], false);
    return normalized;
  }

  markLocal(modelName: string, status: DownloadStatus, type: DownloadType = 'model'): void {
    const id = `${type}:${modelName}`;
    const previous = this.downloads.find(item => item.id === id || (item.modelName === modelName && item.downloadType === type));
    const timestamp = now();
    const item: DownloadListItem = {
      ...(previous || {
        id,
        downloadType: type,
        modelName,
        fileName: '',
        fileIndex: 1,
        totalFiles: 1,
        bytesDownloaded: 0,
        bytesTotal: 0,
        percent: 0,
        startTime: timestamp,
        bytesResumed: 0,
        updatedAt: timestamp,
      }),
      id: previous?.id || id,
      status,
      running: status === 'downloading' ? true : (status === 'paused' || status === 'deleting' ? previous?.running : false),
      percent: status === 'completed' ? 100 : (previous?.percent || 0),
      speedBytesPerSecond: status === 'downloading' ? previous?.speedBytesPerSecond : 0,
      updatedAt: timestamp,
      terminalAt: ['completed', 'error', 'cancelled'].includes(status) ? (previous?.terminalAt || timestamp) : undefined,
    };
    this.mergeDownloads([item], false);
  }

  remove(downloadId: string): void {
    this.removeMany([downloadId]);
  }

  removeMany(downloadIds: string[]): void {
    const ids = Array.from(new Set(downloadIds.filter(Boolean)));
    if (ids.length === 0) return;
    const dismissed = readDismissed();
    const timestamp = now();
    ids.forEach(id => { dismissed[id] = timestamp; });
    writeDismissed(dismissed);
    const idSet = new Set(ids);
    this.downloads = this.downloads.filter(download => !idSet.has(download.id));
    writeStored(this.downloads);
    this.emit();
  }

  private findExisting(raw: DownloadProgressEvent): DownloadListItem | undefined {
    const type = normalizeDownloadType(raw);
    const modelName = nameFromDownload(raw, type);
    const id = idFromDownload(raw, type, modelName);
    return this.downloads.find(item => item.id === id || (modelName && item.downloadType === type && item.modelName === modelName));
  }

  private mergeDownloads(incoming: DownloadListItem[], includeStored: boolean): void {
    const timestamp = now();
    const stored = includeStored ? readStored() : [];
    const dismissed = readDismissed(timestamp);
    const map = new Map<string, DownloadListItem>();

    const putDownload = (rawItem: DownloadListItem) => {
      const item = coerceStoredDownload(rawItem, timestamp);
      const existing = map.get(item.id);
      if (!existing) {
        map.set(item.id, item);
        return;
      }

      const itemTerminal = isDownloadTerminal(item);
      const existingTerminal = isDownloadTerminal(existing);
      const itemUpdated = item.updatedAt || 0;
      const existingUpdated = existing.updatedAt || 0;

      // Cross-tab error/completion snapshots are written to localStorage before
      // the other tab receives its next server poll. Do not let an older in-memory
      // 0 B active placeholder overwrite the newer terminal row. Conversely, a
      // genuinely newer active server snapshot should reopen the row.
      if (itemTerminal !== existingTerminal) {
        if (itemUpdated >= existingUpdated) map.set(item.id, item);
        return;
      }

      if (itemUpdated >= existingUpdated) map.set(item.id, item);
    };

    for (const item of prune([...stored, ...this.downloads], timestamp)) {
      if (!isDismissedTerminal(item, dismissed)) putDownload(item);
    }
    for (const rawItem of incoming) {
      const item = coerceStoredDownload(rawItem, timestamp);
      if (isDownloadActive(item)) {
        delete dismissed[item.id];
        putDownload(item);
      } else if (!isDismissedTerminal(item, dismissed)) {
        putDownload(item);
      }
    }
    writeDismissed(dismissed);
    this.downloads = sortDownloads(prune(Array.from(map.values()), timestamp));
    writeStored(this.downloads);
    this.emit();
  }

  private emit(): void {
    const snapshot = this.downloads;
    this.listeners.forEach(listener => listener(snapshot));
  }

  private scheduleNext(): void {
    if (!this.started) return;
    if (this.pollTimer) clearTimeout(this.pollTimer);
    this.pollTimer = setTimeout(() => void this.refresh(), POLL_MS);
  }
}

export const downloadStore = new DownloadStore();

export function downloadsForModel(downloads: DownloadListItem[], modelName: string): DownloadListItem[] {
  const target = modelName.trim().toLowerCase();
  return downloads.filter(download => {
    if (download.downloadType !== 'model') return false;
    const name = download.modelName.trim().toLowerCase();
    const id = download.id.trim().toLowerCase();
    return name === target || id === `model:${target}` || id.endsWith(`:${target}`);
  });
}

export function activeDownloadForModel(downloads: DownloadListItem[], modelName: string): DownloadListItem | undefined {
  return downloadsForModel(downloads, modelName).find(download => !isDownloadTerminal(download));
}
