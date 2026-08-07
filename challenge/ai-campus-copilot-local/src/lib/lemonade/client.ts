import { LemonadeError, kindForStatus, kindFromBody, toLemonadeError } from "./errors";
import {
  chatChunkSchema,
  chatCompletionSchema,
  embeddingResponseSchema,
  healthSchema,
  modelListSchema,
  statsSchema,
  systemInfoSchema,
  systemStatsSchema,
  type LemonadeChatCompletion,
  type LemonadeEmbeddingResponse,
  type LemonadeHealth,
  type LemonadeModel,
  type LemonadeStats,
  type LemonadeSystemInfo,
  type LemonadeSystemStats,
} from "./schemas";
import type { ZodType } from "zod";

export const DEFAULT_TIMEOUT_MS = 60_000;
export const DEFAULT_CHAT_TIMEOUT_MS = 180_000;

export interface LemonadeClientOptions {
  baseUrl: string;
  apiKey?: string | undefined;
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
}

export interface RequestOptions {
  signal?: AbortSignal | undefined;
  timeoutMs?: number | undefined;
}

/** Every call returns how long it took, so the UI never has to invent numbers. */
export interface Timed<T> {
  value: T;
  durationMs: number;
}

export interface ChatMessage {
  role: "system" | "user" | "assistant";
  content: string;
}

export interface ChatCompletionRequest {
  model: string;
  messages: ChatMessage[];
  temperature?: number;
  max_completion_tokens?: number;
  stop?: string[];
}

export interface ChatStreamEvent {
  /** Incremental text delta. */
  delta: string;
  /** Milliseconds from request start to the first non-empty delta. */
  timeToFirstTokenMs: number | null;
}

export interface StreamResult {
  text: string;
  timeToFirstTokenMs: number | null;
  totalDurationMs: number;
  finishReason: string | null;
}

function joinUrl(baseUrl: string, path: string): string {
  return `${baseUrl.replace(/\/+$/, "")}/${path.replace(/^\/+/, "")}`;
}

/**
 * Links an external signal to an internal timeout controller and reports which
 * of the two fired, so callers can tell "cancelled" apart from "timed out".
 */
function makeSignal(external: AbortSignal | undefined, timeoutMs: number) {
  const controller = new AbortController();
  const state = { timedOut: false };

  const timer = setTimeout(() => {
    state.timedOut = true;
    controller.abort();
  }, timeoutMs);

  const onExternalAbort = () => controller.abort();
  if (external) {
    if (external.aborted) controller.abort();
    else external.addEventListener("abort", onExternalAbort, { once: true });
  }

  return {
    signal: controller.signal,
    state,
    dispose() {
      clearTimeout(timer);
      external?.removeEventListener("abort", onExternalAbort);
    },
  };
}

export class LemonadeClient {
  private readonly baseUrl: string;
  private readonly apiKey: string | undefined;
  private readonly defaultTimeoutMs: number;
  private readonly fetchImpl: typeof fetch;

  constructor(options: LemonadeClientOptions) {
    this.baseUrl = options.baseUrl;
    this.apiKey = options.apiKey;
    this.defaultTimeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.fetchImpl = options.fetchImpl ?? globalThis.fetch.bind(globalThis);
  }

  get serverUrl(): string {
    return this.baseUrl;
  }

  private headers(withBody: boolean): Record<string, string> {
    const headers: Record<string, string> = { Accept: "application/json" };
    if (withBody) headers["Content-Type"] = "application/json";
    if (this.apiKey) headers["Authorization"] = `Bearer ${this.apiKey}`;
    return headers;
  }

  private async rawRequest(
    path: string,
    init: { method: "GET" | "POST"; body?: unknown; accept?: string },
    options: RequestOptions,
  ): Promise<{ response: Response; startedAt: number }> {
    const timeoutMs = options.timeoutMs ?? this.defaultTimeoutMs;
    const { signal, state, dispose } = makeSignal(options.signal, timeoutMs);
    const startedAt = Date.now();

    let response: Response;
    try {
      const headers = this.headers(init.body !== undefined);
      if (init.accept) headers["Accept"] = init.accept;
      response = await this.fetchImpl(joinUrl(this.baseUrl, path), {
        method: init.method,
        headers,
        body: init.body === undefined ? undefined : JSON.stringify(init.body),
        signal,
        cache: "no-store",
      });
    } catch (cause) {
      dispose();
      throw toLemonadeError(cause, state.timedOut);
    }

    if (!response.ok) {
      const text = await response.text().catch(() => "");
      dispose();
      throw new LemonadeError(kindFromBody(text) ?? kindForStatus(response.status, text), {
        status: response.status,
        detail: extractErrorDetail(text),
      });
    }

    // Streaming callers dispose once the body is drained.
    if (init.accept === "text/event-stream") {
      return { response, startedAt };
    }
    dispose();
    return { response, startedAt };
  }

  private async requestJson<T>(
    path: string,
    schema: ZodType<T>,
    init: { method: "GET" | "POST"; body?: unknown },
    options: RequestOptions = {},
  ): Promise<Timed<T>> {
    const { response, startedAt } = await this.rawRequest(path, init, options);
    const raw: unknown = await response.json().catch(() => {
      throw new LemonadeError("invalid_response", {
        detail: "response body was not valid JSON",
      });
    });
    const parsed = schema.safeParse(raw);
    if (!parsed.success) {
      throw new LemonadeError("invalid_response", {
        detail: parsed.error.issues[0]?.message,
      });
    }
    return { value: parsed.data, durationMs: Date.now() - startedAt };
  }

  async health(options: RequestOptions = {}): Promise<Timed<LemonadeHealth>> {
    return this.requestJson("/api/v1/health", healthSchema, { method: "GET" }, options);
  }

  async listModels(options: RequestOptions = {}): Promise<Timed<LemonadeModel[]>> {
    const result = await this.requestJson(
      "/api/v1/models",
      modelListSchema,
      { method: "GET" },
      options,
    );
    return { value: result.value.data, durationMs: result.durationMs };
  }

  async systemInfo(options: RequestOptions = {}): Promise<Timed<LemonadeSystemInfo>> {
    return this.requestJson(
      "/api/v1/system-info",
      systemInfoSchema,
      { method: "GET" },
      options,
    );
  }

  async systemStats(options: RequestOptions = {}): Promise<Timed<LemonadeSystemStats>> {
    return this.requestJson(
      "/api/v1/system-stats",
      systemStatsSchema,
      { method: "GET" },
      options,
    );
  }

  /** Performance counters for the most recent inference request. */
  async stats(options: RequestOptions = {}): Promise<Timed<LemonadeStats>> {
    return this.requestJson("/api/v1/stats", statsSchema, { method: "GET" }, options);
  }

  async chatCompletion(
    request: ChatCompletionRequest,
    options: RequestOptions = {},
  ): Promise<Timed<LemonadeChatCompletion>> {
    return this.requestJson(
      "/api/v1/chat/completions",
      chatCompletionSchema,
      { method: "POST", body: { ...request, stream: false } },
      { ...options, timeoutMs: options.timeoutMs ?? DEFAULT_CHAT_TIMEOUT_MS },
    );
  }

  /** Convenience wrapper returning just the assistant text. */
  async chatText(
    request: ChatCompletionRequest,
    options: RequestOptions = {},
  ): Promise<Timed<string>> {
    const result = await this.chatCompletion(request, options);
    const content = result.value.choices[0]?.message?.content ?? "";
    return { value: content, durationMs: result.durationMs };
  }

  async embeddings(
    model: string,
    input: string[],
    options: RequestOptions = {},
  ): Promise<Timed<LemonadeEmbeddingResponse>> {
    return this.requestJson(
      "/api/v1/embeddings",
      embeddingResponseSchema,
      { method: "POST", body: { model, input, encoding_format: "float" } },
      options,
    );
  }

  /**
   * Embeds `input` and returns vectors ordered to match the input array.
   * Lemonade returns an `index` per item; relying on array order alone would be
   * fragile if a backend ever reorders them.
   */
  async embed(
    model: string,
    input: string[],
    options: RequestOptions = {},
  ): Promise<Timed<number[][]>> {
    const result = await this.embeddings(model, input, options);
    const vectors: number[][] = new Array(input.length);
    result.value.data.forEach((item, arrayIndex) => {
      const target = item.index ?? arrayIndex;
      if (target >= 0 && target < input.length) vectors[target] = item.embedding;
    });
    for (let i = 0; i < input.length; i += 1) {
      if (!vectors[i]) {
        throw new LemonadeError("invalid_response", {
          detail: `missing embedding for input ${i}`,
        });
      }
    }
    return { value: vectors, durationMs: result.durationMs };
  }

  /** Raw SSE passthrough, used by the Next.js proxy route. */
  async chatCompletionStreamResponse(
    request: ChatCompletionRequest,
    options: RequestOptions = {},
  ): Promise<Response> {
    const { response } = await this.rawRequest(
      "/api/v1/chat/completions",
      { method: "POST", body: { ...request, stream: true }, accept: "text/event-stream" },
      { ...options, timeoutMs: options.timeoutMs ?? DEFAULT_CHAT_TIMEOUT_MS },
    );
    return response;
  }

  /**
   * Streams a chat completion, invoking `onEvent` for each text delta.
   * Falls back to a non-streaming request when the server does not return SSE.
   */
  async chatCompletionStream(
    request: ChatCompletionRequest,
    onEvent: (event: ChatStreamEvent) => void,
    options: RequestOptions = {},
  ): Promise<StreamResult> {
    const startedAt = Date.now();
    const response = await this.chatCompletionStreamResponse(request, options);

    const contentType = response.headers.get("content-type") ?? "";
    if (!response.body || !contentType.includes("text/event-stream")) {
      const fallback = await this.chatCompletion(request, options);
      const text = fallback.value.choices[0]?.message?.content ?? "";
      if (text) onEvent({ delta: text, timeToFirstTokenMs: null });
      return {
        text,
        timeToFirstTokenMs: null,
        totalDurationMs: Date.now() - startedAt,
        finishReason: fallback.value.choices[0]?.finish_reason ?? null,
      };
    }

    return consumeChatStream(response.body, startedAt, onEvent);
  }
}

/** Parses an SSE chat stream into text deltas. Exported for unit testing. */
export async function consumeChatStream(
  body: ReadableStream<Uint8Array>,
  startedAt: number,
  onEvent: (event: ChatStreamEvent) => void,
): Promise<StreamResult> {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let text = "";
  let timeToFirstTokenMs: number | null = null;
  let finishReason: string | null = null;

  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });

      let newlineIndex = buffer.indexOf("\n");
      while (newlineIndex !== -1) {
        const line = buffer.slice(0, newlineIndex).trim();
        buffer = buffer.slice(newlineIndex + 1);
        newlineIndex = buffer.indexOf("\n");

        if (!line.startsWith("data:")) continue;
        const payload = line.slice(5).trim();
        if (!payload || payload === "[DONE]") continue;

        let json: unknown;
        try {
          json = JSON.parse(payload);
        } catch {
          continue;
        }
        const chunk = chatChunkSchema.safeParse(json);
        if (!chunk.success) continue;

        const choice = chunk.data.choices?.[0];
        if (choice?.finish_reason) finishReason = choice.finish_reason;
        const delta = choice?.delta?.content;
        if (!delta) continue;

        if (timeToFirstTokenMs === null) timeToFirstTokenMs = Date.now() - startedAt;
        text += delta;
        onEvent({ delta, timeToFirstTokenMs });
      }
    }
  } finally {
    reader.releaseLock();
  }

  return { text, timeToFirstTokenMs, totalDurationMs: Date.now() - startedAt, finishReason };
}

function extractErrorDetail(body: string): string | undefined {
  if (!body) return undefined;
  try {
    const parsed: unknown = JSON.parse(body);
    if (parsed && typeof parsed === "object") {
      const record = parsed as Record<string, unknown>;
      const error = record["error"];
      if (typeof error === "string") return error.slice(0, 300);
      if (error && typeof error === "object") {
        const message = (error as Record<string, unknown>)["message"];
        if (typeof message === "string") return message.slice(0, 300);
      }
      if (typeof record["message"] === "string") {
        return (record["message"] as string).slice(0, 300);
      }
    }
  } catch {
    // Fall through to the raw body.
  }
  return body.slice(0, 300);
}

/** Server-side factory. Reads the URL that never reaches the browser. */
export function createServerClient(overrides: Partial<LemonadeClientOptions> = {}) {
  return new LemonadeClient({
    baseUrl: process.env["LEMONADE_SERVER_URL"] ?? "http://127.0.0.1:13305",
    apiKey: process.env["LEMONADE_API_KEY"],
    ...overrides,
  });
}

/**
 * Browser-side factory. Points at this app's own proxy routes, which mirror
 * Lemonade's paths, so both sides share one implementation.
 */
export function createBrowserClient(overrides: Partial<LemonadeClientOptions> = {}) {
  return new LemonadeClient({ baseUrl: "/api/lemonade", ...overrides });
}
