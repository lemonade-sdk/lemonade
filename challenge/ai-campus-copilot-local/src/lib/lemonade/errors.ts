export type LemonadeErrorKind =
  | "server_unavailable"
  | "model_unavailable"
  | "timeout"
  | "cancelled"
  | "bad_request"
  | "unauthorized"
  | "server_error"
  | "invalid_response";

const KIND_MESSAGES: Record<LemonadeErrorKind, string> = {
  server_unavailable:
    "Could not reach Lemonade Server. Start it and confirm the URL is correct.",
  model_unavailable:
    "Lemonade Server does not have that model available. Pick another model or download it first.",
  timeout: "Lemonade Server did not respond in time.",
  cancelled: "The request was cancelled.",
  bad_request: "Lemonade Server rejected the request.",
  unauthorized:
    "Lemonade Server rejected the request as unauthorized. Check LEMONADE_API_KEY.",
  server_error: "Lemonade Server returned an internal error.",
  invalid_response: "Lemonade Server returned a response in an unexpected shape.",
};

export class LemonadeError extends Error {
  readonly kind: LemonadeErrorKind;
  readonly status: number | undefined;
  readonly detail: string | undefined;

  constructor(
    kind: LemonadeErrorKind,
    options: { status?: number; detail?: string; cause?: unknown } = {},
  ) {
    super(KIND_MESSAGES[kind]);
    this.name = "LemonadeError";
    this.kind = kind;
    this.status = options.status;
    this.detail = options.detail;
    if (options.cause !== undefined) this.cause = options.cause;
  }

  /** Message plus server-supplied detail, safe to render in the UI. */
  get displayMessage(): string {
    return this.detail ? `${this.message} (${this.detail})` : this.message;
  }

  toJSON() {
    return {
      error: true as const,
      kind: this.kind,
      message: this.message,
      status: this.status ?? null,
      detail: this.detail ?? null,
    };
  }
}

export function isLemonadeError(value: unknown): value is LemonadeError {
  return value instanceof LemonadeError;
}

const KINDS = new Set<string>(Object.keys(KIND_MESSAGES));

/**
 * Recovers the original error kind from a proxied error body. Without this the
 * browser would re-derive the kind from the HTTP status alone and report a
 * genuinely-unreachable server as a generic upstream failure.
 */
export function kindFromBody(body: string): LemonadeErrorKind | null {
  try {
    const parsed: unknown = JSON.parse(body);
    if (parsed && typeof parsed === "object") {
      const kind = (parsed as Record<string, unknown>)["kind"];
      if (typeof kind === "string" && KINDS.has(kind)) return kind as LemonadeErrorKind;
    }
  } catch {
    // Not a JSON error envelope.
  }
  return null;
}

/** Maps an HTTP status (plus body text) onto the closest error kind. */
export function kindForStatus(status: number, body: string): LemonadeErrorKind {
  if (status === 401 || status === 403) return "unauthorized";
  if (status === 404) return "model_unavailable";
  if (status === 400 && /model/i.test(body)) return "model_unavailable";
  if (status >= 500) return "server_error";
  return "bad_request";
}

/** Normalises fetch/abort rejections into a typed LemonadeError. */
export function toLemonadeError(cause: unknown, timedOut: boolean): LemonadeError {
  if (isLemonadeError(cause)) return cause;
  if (timedOut) return new LemonadeError("timeout", { cause });
  if (cause instanceof DOMException && cause.name === "AbortError") {
    return new LemonadeError("cancelled", { cause });
  }
  if (cause instanceof Error && cause.name === "AbortError") {
    return new LemonadeError("cancelled", { cause });
  }
  return new LemonadeError("server_unavailable", {
    detail: cause instanceof Error ? cause.message : undefined,
    cause,
  });
}
