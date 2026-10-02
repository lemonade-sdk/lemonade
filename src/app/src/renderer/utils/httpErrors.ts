/**
 * Structured, user-facing descriptions for failed server requests.
 *
 * Chat requests used to surface the raw `HTTP error! status: 403` string and
 * then append the selected model's backend install command to every failure,
 * which reads as nonsense when the server refused the request for an unrelated
 * reason (a blocked browser origin, a bad API key).
 */

export interface ChatErrorInfo {
  title: string;
  detail: string;
  /** Shell command the user can run to resolve the failure, when one exists. */
  command?: string;
}

export class HttpError extends Error {
  readonly status: number;
  readonly serverMessage: string;
  readonly originBlocked: boolean;

  constructor(status: number, serverMessage: string, originBlocked: boolean) {
    super(serverMessage || `HTTP ${status}`);
    this.name = 'HttpError';
    this.status = status;
    this.serverMessage = serverMessage;
    this.originBlocked = originBlocked;
  }
}

function extractMessage(body: unknown): string {
  if (typeof body === 'string') return body.trim();
  if (!body || typeof body !== 'object') return '';

  const record = body as Record<string, unknown>;
  const error = record.error;
  if (typeof error === 'string') return error.trim();
  if (error && typeof error === 'object') {
    const message = (error as Record<string, unknown>).message;
    if (typeof message === 'string') return message.trim();
  }
  if (typeof record.message === 'string') return record.message.trim();
  if (typeof record.detail === 'string') return record.detail.trim();
  return '';
}

/** Read an error response into an `HttpError` carrying the server's own message. */
export async function readHttpError(response: Response, fallback = ''): Promise<HttpError> {
  let raw = '';
  try {
    raw = await response.text();
  } catch {
    raw = '';
  }

  let parsedMessage = '';
  if (raw) {
    try {
      parsedMessage = extractMessage(JSON.parse(raw));
    } catch {
      parsedMessage = raw.trim();
    }
  }

  // A 403 on an API route is the origin guard rejecting the browser origin. The
  // only other 403 the server emits is the static-asset path-traversal guard,
  // which never reaches this fetch path; still, require the origin message when
  // one is present so the two can never be confused.
  const originBlocked =
    response.status === 403 &&
    (parsedMessage === '' || /origin/i.test(parsedMessage));

  return new HttpError(response.status, parsedMessage || fallback, originBlocked);
}

const SERVER_REJECTION_STATUSES = new Set([401, 403]);

/** True when the server refused the request for an auth or origin reason. */
export function isServerRejection(error: unknown): boolean {
  return error instanceof HttpError && SERVER_REJECTION_STATUSES.has(error.status);
}

function normalizeOrigin(origin: string): string {
  return origin.replace(/\/+$/, '');
}

/** Map any thrown error to a title, explanation, and optional fix command. */
export function describeChatError(error: unknown, origin: string): ChatErrorInfo {
  if (error instanceof HttpError) {
    if (error.originBlocked) {
      const pageOrigin = normalizeOrigin(origin);
      return {
        title: 'Blocked by the Lemonade server',
        detail:
          `This page's origin (${pageOrigin || 'unknown'}) is not in the server's ` +
          'allowed origins list, so Lemonade refused the request. Browser pages are ' +
          'restricted this way; command-line and SDK clients are not. Add the origin ' +
          'to `allowed_origins` on the server, then reload this page. If the server ' +
          'already allows other origins, add this one to that comma-separated list ' +
          'instead of replacing it.',
        command: pageOrigin
          ? `lemonade config set allowed_origins="${pageOrigin}"`
          : undefined,
      };
    }

    if (error.status === 401) {
      return {
        title: 'Authentication failed',
        detail:
          error.serverMessage ||
          'The server rejected the API key for this page. Check that the correct key is configured.',
      };
    }

    return {
      title: `The server returned an error (HTTP ${error.status})`,
      detail: error.serverMessage || 'The request could not be completed.',
    };
  }

  // fetch() rejects with a TypeError when the server is unreachable.
  if (error instanceof TypeError) {
    return {
      title: 'Cannot reach the Lemonade server',
      detail:
        'The server did not respond. Check that Lemonade is running and that the server address is correct.',
    };
  }

  const message = error instanceof Error ? error.message : '';
  return {
    title: 'Something went wrong',
    detail: message || 'The request could not be completed.',
  };
}

/**
 * Describe an error for the chat and, when it is a model/backend failure,
 * append the backend setup command. Server rejections (blocked origin, bad API
 * key) have nothing to do with backends, so the hint is withheld for them.
 */
export function buildChatErrorInfo(
  error: unknown,
  origin: string,
  backendAction?: string,
): ChatErrorInfo {
  const info = describeChatError(error, origin);
  if (!backendAction || isServerRejection(error)) {
    return info;
  }
  return { ...info, detail: `${info.detail}\n\n${backendAction}` };
}
