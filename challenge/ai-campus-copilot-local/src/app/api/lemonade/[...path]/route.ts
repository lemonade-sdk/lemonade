import { NextResponse } from "next/server";
import { z } from "zod";
import { createServerClient, DEFAULT_CHAT_TIMEOUT_MS } from "@/lib/lemonade/client";
import { LemonadeError, isLemonadeError } from "@/lib/lemonade/errors";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

/**
 * Only these Lemonade paths are reachable through the proxy. Anything else —
 * including model deletion, pulls and the /internal/* surface — is refused, so
 * a bug in the browser bundle cannot mutate the user's Lemonade install.
 */
const ALLOWED_GET = new Set([
  "api/v1/health",
  "api/v1/models",
  "api/v1/system-info",
  "api/v1/system-stats",
  "api/v1/stats",
]);

const ALLOWED_POST = new Set(["api/v1/chat/completions", "api/v1/embeddings"]);

const chatMessageSchema = z.object({
  role: z.enum(["system", "user", "assistant"]),
  content: z.string().max(200_000),
});

const chatRequestSchema = z.object({
  model: z.string().min(1).max(300),
  messages: z.array(chatMessageSchema).min(1).max(200),
  stream: z.boolean().optional(),
  temperature: z.number().min(0).max(2).optional(),
  max_completion_tokens: z.number().int().positive().max(32_768).optional(),
  stop: z.array(z.string().max(200)).max(4).optional(),
});

const embeddingsRequestSchema = z.object({
  model: z.string().min(1).max(300),
  input: z.array(z.string().max(100_000)).min(1).max(64),
});

function errorResponse(error: unknown) {
  const lemonadeError = isLemonadeError(error)
    ? error
    : new LemonadeError("server_unavailable", {
        detail: error instanceof Error ? error.message : undefined,
      });

  const status =
    lemonadeError.kind === "server_unavailable"
      ? 503
      : lemonadeError.kind === "timeout"
        ? 504
        : lemonadeError.kind === "model_unavailable"
          ? 404
          : lemonadeError.kind === "unauthorized"
            ? 401
            : lemonadeError.kind === "bad_request"
              ? 400
              : 502;

  return NextResponse.json(lemonadeError.toJSON(), { status });
}

export async function GET(_request: Request, context: { params: Promise<{ path: string[] }> }) {
  const { path } = await context.params;
  const joined = path.join("/");
  if (!ALLOWED_GET.has(joined)) {
    return NextResponse.json({ error: true, message: "Unsupported path" }, { status: 404 });
  }

  const client = createServerClient();
  try {
    switch (joined) {
      case "api/v1/health":
        return NextResponse.json((await client.health()).value);
      case "api/v1/models":
        return NextResponse.json({ object: "list", data: (await client.listModels()).value });
      case "api/v1/system-info":
        return NextResponse.json((await client.systemInfo()).value);
      case "api/v1/system-stats":
        return NextResponse.json((await client.systemStats()).value);
      default:
        return NextResponse.json((await client.stats()).value);
    }
  } catch (error) {
    return errorResponse(error);
  }
}

export async function POST(request: Request, context: { params: Promise<{ path: string[] }> }) {
  const { path } = await context.params;
  const joined = path.join("/");
  if (!ALLOWED_POST.has(joined)) {
    return NextResponse.json({ error: true, message: "Unsupported path" }, { status: 404 });
  }

  let body: unknown;
  try {
    body = await request.json();
  } catch {
    return NextResponse.json({ error: true, message: "Invalid JSON body" }, { status: 400 });
  }

  const client = createServerClient();

  if (joined === "api/v1/embeddings") {
    const parsed = embeddingsRequestSchema.safeParse(body);
    if (!parsed.success) {
      return NextResponse.json(
        { error: true, message: "Invalid embeddings request", detail: parsed.error.issues[0]?.message },
        { status: 400 },
      );
    }
    try {
      const result = await client.embeddings(parsed.data.model, parsed.data.input, {
        signal: request.signal,
      });
      return NextResponse.json(result.value);
    } catch (error) {
      return errorResponse(error);
    }
  }

  const parsed = chatRequestSchema.safeParse(body);
  if (!parsed.success) {
    return NextResponse.json(
      { error: true, message: "Invalid chat request", detail: parsed.error.issues[0]?.message },
      { status: 400 },
    );
  }

  const { stream, ...chatRequest } = parsed.data;

  try {
    if (!stream) {
      const result = await client.chatCompletion(chatRequest, { signal: request.signal });
      return NextResponse.json(result.value);
    }

    const upstream = await client.chatCompletionStreamResponse(chatRequest, {
      signal: request.signal,
      timeoutMs: DEFAULT_CHAT_TIMEOUT_MS,
    });

    if (!upstream.body) {
      return NextResponse.json({ error: true, message: "Empty stream" }, { status: 502 });
    }

    return new Response(upstream.body, {
      status: 200,
      headers: {
        "Content-Type": upstream.headers.get("content-type") ?? "text/event-stream",
        "Cache-Control": "no-cache, no-transform",
        Connection: "keep-alive",
      },
    });
  } catch (error) {
    return errorResponse(error);
  }
}
