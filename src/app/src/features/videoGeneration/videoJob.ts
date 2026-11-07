export interface VideoJobRecord {
  id: string;
  status: string;
  error?: string;
  mime_type?: string;
  output_format?: string;
  frame_count?: number;
  fps?: number;
  duration?: number;
}

export interface VideoJobTransport {
  create(body: Record<string, unknown>): Promise<VideoJobRecord>;
  get(id: string, signal?: AbortSignal): Promise<VideoJobRecord>;
  content(id: string, signal?: AbortSignal): Promise<Blob>;
  cancel(id: string, signal?: AbortSignal): Promise<void>;
}

export interface VideoJobResult {
  record: VideoJobRecord;
  blob: Blob;
}

/**
 * Server statuses plus three the client adds: 'reconnecting' while status
 * polls fail, 'canceling' after Stop, and 'downloading' once the clip is done.
 */
export interface VideoJobOptions {
  signal?: AbortSignal;
  onStatus?: (record: VideoJobRecord) => void;
  pollMs?: number;
  maxPollFailures?: number;
  maxPollBackoffMs?: number;
  maxCancelPolls?: number;
  maxDownloadAttempts?: number;
  requestTimeoutMs?: number;
  sleep?: (ms: number, signal?: AbortSignal) => Promise<void>;
}

export class VideoJobCanceledError extends Error {
  constructor() {
    super('Video generation was canceled.');
    this.name = 'VideoJobCanceledError';
  }
}

/** Stop was pressed but the server never confirmed the job ended. */
export class VideoJobStopUnconfirmedError extends Error {
  constructor(readonly jobId: string) {
    super(`Could not confirm that video job ${jobId} stopped; the server may still be generating it.`);
    this.name = 'VideoJobStopUnconfirmedError';
  }
}

/** The clip finished on the server but could not be downloaded. */
export class VideoJobDownloadError extends Error {
  constructor(readonly jobId: string, cause: unknown) {
    const reason = cause instanceof Error ? cause.message : String(cause);
    super(`Video job ${jobId} finished but its clip could not be downloaded: ${reason}`);
    this.name = 'VideoJobDownloadError';
  }
}

export function isVideoJobCanceled(err: unknown): err is VideoJobCanceledError {
  return err instanceof VideoJobCanceledError;
}

export function isVideoJobStopUnconfirmed(err: unknown): err is VideoJobStopUnconfirmedError {
  return err instanceof VideoJobStopUnconfirmedError;
}

const TERMINAL = new Set(['completed', 'failed', 'canceled']);

function abortableSleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise(resolve => {
    if (signal?.aborted) {
      resolve();
      return;
    }
    const done = () => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', done);
      resolve();
    };
    const timer = setTimeout(done, ms);
    signal?.addEventListener('abort', done, { once: true });
  });
}

function timeoutSignal(ms: number): AbortSignal | undefined {
  return typeof AbortSignal.timeout === 'function' ? AbortSignal.timeout(ms) : undefined;
}

function withTimeout(signal: AbortSignal | undefined, ms: number): AbortSignal | undefined {
  const timeout = timeoutSignal(ms);
  if (!signal) return timeout;
  if (!timeout || typeof AbortSignal.any !== 'function') return signal;
  return AbortSignal.any([signal, timeout]);
}

function isGone(err: unknown): boolean {
  return (err as { status?: number } | null)?.status === 404;
}

export function videoFileExtension(record: VideoJobRecord): string {
  const format = String(record.output_format || '').toLowerCase();
  if (/^[a-z0-9]{2,5}$/.test(format)) return format;
  const subtype = String(record.mime_type || '').toLowerCase().split('/')[1] || '';
  if (subtype === 'x-msvideo') return 'avi';
  return /^[a-z0-9]{2,5}$/.test(subtype) ? subtype : 'webm';
}

/**
 * The server keeps generating after the browser stops listening, so an abort
 * must reach /cancel or the GPU stays busy for minutes.
 */
export async function runVideoJob(
  transport: VideoJobTransport,
  body: Record<string, unknown>,
  options: VideoJobOptions = {},
): Promise<VideoJobResult> {
  const {
    signal,
    onStatus,
    pollMs = 2000,
    maxPollFailures = 3,
    maxPollBackoffMs = 30000,
    maxCancelPolls = 30,
    maxDownloadAttempts = 3,
    requestTimeoutMs = 15000,
    sleep = abortableSleep,
  } = options;
  if (signal?.aborted) throw new VideoJobCanceledError();

  let record = await transport.create(body);
  const id = record.id;
  // The model must not be reused until the server reports the job ended, so a
  // stop that the server never confirms is reported as unconfirmed, not done.
  const stop = async (): Promise<never> => {
    onStatus?.({ ...record, status: 'canceling' });
    await transport.cancel(id, timeoutSignal(requestTimeoutMs)).catch(() => undefined);
    for (let i = 0; i < maxCancelPolls; i += 1) {
      await sleep(pollMs);
      const latest = await transport.get(id, timeoutSignal(requestTimeoutMs)).catch(() => null);
      if (latest && TERMINAL.has(latest.status)) throw new VideoJobCanceledError();
    }
    throw new VideoJobStopUnconfirmedError(id);
  };
  onStatus?.(record);

  // A server that stops answering may still be generating, so polling keeps
  // the job and backs off instead of giving up; Stop stays available.
  let failures = 0;
  while (!TERMINAL.has(record.status)) {
    if (signal?.aborted) return stop();
    const wait = failures === 0 ? pollMs : Math.min(pollMs * 2 ** failures, maxPollBackoffMs);
    await sleep(wait, signal);
    if (signal?.aborted) return stop();
    try {
      record = await transport.get(id, withTimeout(signal, requestTimeoutMs));
      failures = 0;
    } catch (err) {
      if (signal?.aborted) return stop();
      if (isGone(err)) throw err;
      failures += 1;
      if (failures >= maxPollFailures) onStatus?.({ ...record, status: 'reconnecting' });
      continue;
    }
    onStatus?.(record);
  }

  if (record.status === 'canceled') throw new VideoJobCanceledError();
  if (record.status === 'failed') {
    throw new Error(record.error ? `Video generation failed: ${record.error}` : 'Video generation failed.');
  }

  // The clip already exists, so Stop no longer applies: the download ignores
  // the caller's signal and is retried rather than thrown away.
  onStatus?.({ ...record, status: 'downloading' });
  let lastError: unknown = null;
  for (let attempt = 0; attempt < maxDownloadAttempts; attempt += 1) {
    if (attempt > 0) await sleep(pollMs);
    try {
      const blob = await transport.content(id, timeoutSignal(requestTimeoutMs * 4));
      if (blob.size === 0) throw new Error('Video endpoint returned an empty clip.');
      return { record, blob };
    } catch (err) {
      lastError = err;
      if (isGone(err)) break;
    }
  }
  throw new VideoJobDownloadError(id, lastError);
}
