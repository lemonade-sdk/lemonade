import type { Chunk } from "@/lib/documents/types";
import type { LemonadeClient } from "@/lib/lemonade/client";
import type { ChatTurn } from "@/lib/db/schema";
import { embedQuestion } from "./embed";
import { buildGroundedMessages } from "./prompt";
import { retrieveRelevantChunks, type RetrievedSource } from "./retrieve";

export interface AskMetrics {
  questionEmbeddingMs: number | null;
  retrievalMs: number | null;
  timeToFirstTokenMs: number | null;
  generationMs: number | null;
  totalMs: number;
  tokensPerSecond: number | null;
  outputTokens: number | null;
}

export interface AskResult {
  answer: string;
  sources: RetrievedSource[];
  metrics: AskMetrics;
}

export interface AskOptions {
  client: LemonadeClient;
  chatModel: string;
  embeddingModel: string;
  question: string;
  chunks: Chunk[];
  filename: string;
  history?: { role: "user" | "assistant"; content: string }[];
  signal?: AbortSignal | undefined;
  onDelta?: ((delta: string) => void) | undefined;
  topK?: number;
  minScore?: number;
}

/**
 * One full grounded question: embed → retrieve → generate. Every phase is
 * measured; `tokensPerSecond` comes from Lemonade's own /stats endpoint rather
 * than being estimated locally, and stays null when unavailable.
 */
export async function askQuestion(options: AskOptions): Promise<AskResult> {
  const {
    client,
    chatModel,
    embeddingModel,
    question,
    chunks,
    filename,
    history = [],
    signal,
    onDelta,
  } = options;

  const startedAt = Date.now();

  const embedding = await embedQuestion(client, embeddingModel, question, signal);

  const retrieval = retrieveRelevantChunks(embedding.vector, chunks, {
    ...(options.topK !== undefined ? { topK: options.topK } : {}),
    ...(options.minScore !== undefined ? { minScore: options.minScore } : {}),
  });

  const messages = buildGroundedMessages({
    question,
    sources: retrieval.sources,
    filename,
    history,
  });

  const generationStartedAt = Date.now();
  const stream = await client.chatCompletionStream(
    { model: chatModel, messages, temperature: 0.2, max_completion_tokens: 900 },
    (event) => onDelta?.(event.delta),
    { signal },
  );

  const serverStats = await client
    .stats({ signal })
    .then((result) => result.value)
    .catch(() => null);

  return {
    answer: stream.text,
    sources: retrieval.sources,
    metrics: {
      questionEmbeddingMs: embedding.durationMs,
      retrievalMs: retrieval.durationMs,
      timeToFirstTokenMs:
        stream.timeToFirstTokenMs ?? secondsToMs(serverStats?.time_to_first_token),
      generationMs: Date.now() - generationStartedAt,
      totalMs: Date.now() - startedAt,
      tokensPerSecond: serverStats?.tokens_per_second ?? null,
      outputTokens: serverStats?.output_tokens ?? null,
    },
  };
}

function secondsToMs(seconds: number | null | undefined): number | null {
  return seconds === null || seconds === undefined ? null : seconds * 1000;
}

export function toChatTurnSources(sources: RetrievedSource[]): ChatTurn["sources"] {
  return sources.map((source) => ({
    chunkId: source.chunkId,
    page: source.page,
    section: source.section,
    chunkNumber: source.chunkNumber,
    score: source.score,
    excerpt: source.excerpt,
  }));
}
