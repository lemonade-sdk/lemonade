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
  cancel(id: string): Promise<void>;
}

export interface VideoJobResult {
  record: VideoJobRecord;
  blob: Blob;
}

export interface VideoJobOptions {
  signal?: AbortSignal;
  onStatus?: (record: VideoJobRecord) => void;
  pollMs?: number;
  maxPollFailures?: number;
  maxCancelPolls?: number;
  sleep?: (ms: number, signal?: AbortSignal) => Promise<void>;
}

export class VideoJobCanceledError extends Error {
  constructor() {
    super('Video generation was canceled.');
    this.name = 'VideoJobCanceledError';
  }
}

export function isVideoJobCanceled(err: unknown): err is VideoJobCanceledError {
  return err instanceof VideoJobCanceledError;
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
    maxCancelPolls = 30,
    sleep = abortableSleep,
  } = options;
  if (signal?.aborted) throw new VideoJobCanceledError();

  let record = await transport.create(body);
  const id = record.id;
  // The backend can take seconds to stop, and the model must not be reused
  // until the job has actually ended.
  const stop = async (): Promise<never> => {
    await transport.cancel(id).catch(() => undefined);
    onStatus?.({ ...record, status: 'canceling' });
    for (let i = 0; i < maxCancelPolls; i += 1) {
      await sleep(pollMs);
      const latest = await transport.get(id).catch(() => null);
      if (!latest || TERMINAL.has(latest.status)) break;
    }
    throw new VideoJobCanceledError();
  };
  onStatus?.(record);

  let failures = 0;
  while (!TERMINAL.has(record.status)) {
    if (signal?.aborted) return stop();
    await sleep(pollMs, signal);
    if (signal?.aborted) return stop();
    try {
      record = await transport.get(id, signal);
      failures = 0;
    } catch (err) {
      if (signal?.aborted) return stop();
      failures += 1;
      if (failures >= maxPollFailures) throw err;
      continue;
    }
    onStatus?.(record);
  }

  if (record.status === 'canceled') throw new VideoJobCanceledError();
  if (record.status === 'failed') {
    throw new Error(record.error ? `Video generation failed: ${record.error}` : 'Video generation failed.');
  }
  let blob: Blob;
  try {
    blob = await transport.content(id, signal);
  } catch (err) {
    if (signal?.aborted) throw new VideoJobCanceledError();
    throw err;
  }
  if (blob.size === 0) throw new Error('Video endpoint returned an empty clip.');
  return { record, blob };
}
