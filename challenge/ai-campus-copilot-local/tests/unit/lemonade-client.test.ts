/**
 * These tests exercise the Lemonade integration path against MOCKED responses
 * shaped from the parent repository's own API documentation
 * (docs/api/openai.md and docs/api/lemonade.md).
 *
 * They do NOT prove a live Lemonade Server works — see tests/README.md for how
 * to verify against a running server.
 */
import { describe, expect, it, vi } from "vitest";
import { consumeChatStream, LemonadeClient } from "@/lib/lemonade/client";
import { LemonadeError } from "@/lib/lemonade/errors";
import { classifyModels, chatModels, embeddingModels } from "@/lib/lemonade/models";

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function clientWith(fetchImpl: typeof fetch, timeoutMs = 5000) {
  return new LemonadeClient({ baseUrl: "http://127.0.0.1:13305", fetchImpl, timeoutMs });
}

describe("health", () => {
  it("parses the documented health payload", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({
        status: "ok",
        version: "9.3.3",
        model_loaded: "Qwen3-0.6B-GGUF",
        all_models_loaded: [{ model_name: "Qwen3-0.6B-GGUF", type: "llm", device: "gpu" }],
      }),
    ) as unknown as typeof fetch;

    const result = await clientWith(fetchImpl).health();

    expect(result.value.status).toBe("ok");
    expect(result.value.version).toBe("9.3.3");
    expect(result.durationMs).toBeGreaterThanOrEqual(0);
  });

  it("calls the quad-prefixed /api/v1 path", async () => {
    const fetchImpl = vi.fn(async () => jsonResponse({ status: "ok" })) as unknown as typeof fetch;
    await clientWith(fetchImpl).health();

    expect(vi.mocked(fetchImpl).mock.calls[0]?.[0]).toBe(
      "http://127.0.0.1:13305/api/v1/health",
    );
  });

  it("reports server_unavailable when the connection is refused", async () => {
    const fetchImpl = vi.fn(async () => {
      throw new TypeError("fetch failed");
    }) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl).health()).rejects.toMatchObject({
      kind: "server_unavailable",
    });
  });

  it("honours the kind sent by this app's proxy over the raw HTTP status", async () => {
    // The proxy reports an unreachable server as 503 + kind:"server_unavailable".
    // Deriving from the status alone would downgrade that to a generic 5xx.
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ error: true, kind: "server_unavailable", message: "…" }, 503),
    ) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl).health()).rejects.toMatchObject({
      kind: "server_unavailable",
      status: 503,
    });
  });

  it("ignores an unrecognised kind and falls back to the status mapping", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ error: true, kind: "not_a_real_kind" }, 500),
    ) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl).health()).rejects.toMatchObject({ kind: "server_error" });
  });

  it("reports invalid_response when the shape is wrong", async () => {
    const fetchImpl = vi.fn(async () => jsonResponse({ unexpected: true })) as unknown as typeof fetch;
    await expect(clientWith(fetchImpl).health()).rejects.toMatchObject({
      kind: "invalid_response",
    });
  });
});

describe("listModels", () => {
  it("returns the data array from the OpenAI-compatible envelope", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({
        object: "list",
        data: [
          { id: "Qwen3-0.6B-GGUF", recipe: "llamacpp", size: 0.38, labels: ["reasoning"] },
          { id: "nomic-embed-text-v1-GGUF", recipe: "llamacpp", labels: ["embeddings"] },
        ],
      }),
    ) as unknown as typeof fetch;

    const result = await clientWith(fetchImpl).listModels();
    expect(result.value.map((model) => model.id)).toEqual([
      "Qwen3-0.6B-GGUF",
      "nomic-embed-text-v1-GGUF",
    ]);
  });

  it("maps 404 to model_unavailable", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ error: { message: "model not found" } }, 404),
    ) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl).listModels()).rejects.toMatchObject({
      kind: "model_unavailable",
      status: 404,
    });
  });
});

describe("model classification", () => {
  const models = classifyModels([
    { id: "Qwen3-0.6B-GGUF", recipe: "llamacpp", labels: ["reasoning"] },
    { id: "nomic-embed-text-v1-GGUF", recipe: "llamacpp", labels: ["embeddings"] },
    { id: "Whisper-Large-v3-Turbo", recipe: "whispercpp", labels: ["transcription"] },
    { id: "SD-Turbo", recipe: "sd-cpp", labels: ["image"] },
    { id: "kokoro-v1", recipe: "kokoro", labels: ["tts"] },
    { id: "some-reranker", recipe: "llamacpp", labels: ["reranking"] },
  ]);

  it("offers only text-generation models as chat models", () => {
    expect(chatModels(models).map((model) => model.id)).toEqual(["Qwen3-0.6B-GGUF"]);
  });

  it("offers only embedding-labelled models as embedding models", () => {
    expect(embeddingModels(models).map((model) => model.id)).toEqual([
      "nomic-embed-text-v1-GGUF",
    ]);
  });

  it("excludes embedding models on recipes Lemonade does not support for embeddings", () => {
    const onnx = classifyModels([
      { id: "embed-onnx", recipe: "ryzenai-llm", labels: ["embeddings"] },
    ]);
    expect(embeddingModels(onnx)).toHaveLength(0);
  });
});

describe("embeddings", () => {
  it("orders vectors by the index field rather than array position", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({
        object: "list",
        data: [
          { object: "embedding", index: 1, embedding: [0.4, 0.5, 0.6] },
          { object: "embedding", index: 0, embedding: [0.1, 0.2, 0.3] },
        ],
        model: "nomic-embed-text-v1-GGUF",
      }),
    ) as unknown as typeof fetch;

    const result = await clientWith(fetchImpl).embed("nomic-embed-text-v1-GGUF", ["a", "b"]);

    expect(result.value[0]).toEqual([0.1, 0.2, 0.3]);
    expect(result.value[1]).toEqual([0.4, 0.5, 0.6]);
  });

  it("sends the documented request body", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ data: [{ index: 0, embedding: [1] }] }),
    ) as unknown as typeof fetch;

    await clientWith(fetchImpl).embed("m", ["hello"]);

    const init = vi.mocked(fetchImpl).mock.calls[0]?.[1];
    expect(JSON.parse(String(init?.body))).toEqual({
      model: "m",
      input: ["hello"],
      encoding_format: "float",
    });
  });

  it("fails loudly when the server returns fewer vectors than inputs", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ data: [{ index: 0, embedding: [1, 2] }] }),
    ) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl).embed("m", ["a", "b"])).rejects.toMatchObject({
      kind: "invalid_response",
    });
  });
});

describe("chatCompletion", () => {
  it("extracts the assistant message content", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({
        id: "0",
        object: "chat.completion",
        model: "Qwen3-0.6B-GGUF",
        choices: [
          {
            index: 0,
            message: { role: "assistant", content: "Exams begin on 12 May (page 1)." },
            finish_reason: "stop",
          },
        ],
      }),
    ) as unknown as typeof fetch;

    const result = await clientWith(fetchImpl).chatText({
      model: "Qwen3-0.6B-GGUF",
      messages: [{ role: "user", content: "When do exams begin?" }],
    });

    expect(result.value).toBe("Exams begin on 12 May (page 1).");
  });

  it("sends stream:false for the non-streaming path", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ choices: [{ message: { content: "hi" } }] }),
    ) as unknown as typeof fetch;

    await clientWith(fetchImpl).chatCompletion({
      model: "m",
      messages: [{ role: "user", content: "hi" }],
    });

    expect(JSON.parse(String(vi.mocked(fetchImpl).mock.calls[0]?.[1]?.body)).stream).toBe(false);
  });
});

describe("timeouts and cancellation", () => {
  it("raises a timeout error when the server does not answer in time", async () => {
    const fetchImpl = vi.fn(
      (_url: string, init?: RequestInit) =>
        new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener("abort", () =>
            reject(new DOMException("aborted", "AbortError")),
          );
        }),
    ) as unknown as typeof fetch;

    await expect(clientWith(fetchImpl, 20).health()).rejects.toMatchObject({ kind: "timeout" });
  });

  it("reports cancellation distinctly from timeout", async () => {
    const controller = new AbortController();
    const fetchImpl = vi.fn(
      (_url: string, init?: RequestInit) =>
        new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener("abort", () =>
            reject(new DOMException("aborted", "AbortError")),
          );
        }),
    ) as unknown as typeof fetch;

    const pending = clientWith(fetchImpl, 10_000).health({ signal: controller.signal });
    controller.abort();

    await expect(pending).rejects.toMatchObject({ kind: "cancelled" });
  });
});

describe("consumeChatStream", () => {
  function sseStream(lines: string[]): ReadableStream<Uint8Array> {
    const encoder = new TextEncoder();
    return new ReadableStream({
      start(controller) {
        for (const line of lines) controller.enqueue(encoder.encode(line));
        controller.close();
      },
    });
  }

  it("assembles deltas into the full answer", async () => {
    const stream = sseStream([
      'data: {"choices":[{"delta":{"content":"Exams "}}]}\n',
      'data: {"choices":[{"delta":{"content":"begin "}}]}\n',
      'data: {"choices":[{"delta":{"content":"soon."},"finish_reason":"stop"}]}\n',
      "data: [DONE]\n",
    ]);

    const deltas: string[] = [];
    const result = await consumeChatStream(stream, Date.now(), (event) => deltas.push(event.delta));

    expect(deltas).toEqual(["Exams ", "begin ", "soon."]);
    expect(result.text).toBe("Exams begin soon.");
    expect(result.finishReason).toBe("stop");
    expect(result.timeToFirstTokenMs).not.toBeNull();
  });

  it("handles chunks split across network reads", async () => {
    const stream = sseStream(['data: {"choices":[{"delta":{"co', 'ntent":"split"}}]}\n']);
    const result = await consumeChatStream(stream, Date.now(), () => {});
    expect(result.text).toBe("split");
  });

  it("ignores malformed and keep-alive lines instead of throwing", async () => {
    const stream = sseStream([
      ": keep-alive\n",
      "data: not-json\n",
      "\n",
      'data: {"choices":[{"delta":{"content":"ok"}}]}\n',
    ]);

    const result = await consumeChatStream(stream, Date.now(), () => {});
    expect(result.text).toBe("ok");
  });
});

describe("LemonadeError", () => {
  it("serialises to a client-safe payload", () => {
    const error = new LemonadeError("model_unavailable", { status: 404, detail: "no such model" });
    expect(error.toJSON()).toEqual({
      error: true,
      kind: "model_unavailable",
      message: expect.stringContaining("model"),
      status: 404,
      detail: "no such model",
    });
  });
});
