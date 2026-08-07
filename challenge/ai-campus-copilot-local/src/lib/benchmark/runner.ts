import type { BenchmarkQuestionResult, BenchmarkRun } from "@/lib/db/schema";
import type { Chunk, StoredDocument } from "@/lib/documents/types";
import type { LemonadeClient } from "@/lib/lemonade/client";
import { askQuestion } from "@/lib/rag/ask";
import { createId } from "@/lib/utils/cn";

/**
 * Fixed question set so runs are comparable across machines and models. These
 * target the kinds of facts a campus notice actually contains.
 */
export const BENCHMARK_QUESTIONS = [
  "What are the key dates mentioned in this document?",
  "Who is eligible, and what are the eligibility conditions?",
  "What actions must a student take, and by when?",
  "Are any fees, stipends or amounts of money mentioned?",
  "Who should a student contact for questions about this document?",
] as const;

export interface BenchmarkOptions {
  client: LemonadeClient;
  chatModel: string;
  embeddingModel: string;
  document: StoredDocument;
  chunks: Chunk[];
  signal?: AbortSignal | undefined;
  onProgress?: ((completed: number, total: number, question: string) => void) | undefined;
}

/**
 * Runs every benchmark question through the real pipeline and records only
 * measured values. A failed question is recorded with its error rather than
 * dropped, so an export always accounts for all five.
 */
export async function runBenchmark(options: BenchmarkOptions): Promise<BenchmarkRun> {
  const { client, chatModel, embeddingModel, document, chunks, signal, onProgress } = options;

  const results: BenchmarkQuestionResult[] = [];

  const [health, systemInfo] = await Promise.all([
    client.health({ signal }).then((r) => r.value).catch(() => null),
    client.systemInfo({ signal }).then((r) => r.value).catch(() => null),
  ]);

  for (const [index, question] of BENCHMARK_QUESTIONS.entries()) {
    onProgress?.(index, BENCHMARK_QUESTIONS.length, question);
    const startedAt = Date.now();

    try {
      const answer = await askQuestion({
        client,
        chatModel,
        embeddingModel,
        question,
        chunks,
        filename: document.filename,
        signal,
      });

      results.push({
        question,
        answerChars: answer.answer.length,
        questionEmbeddingMs: answer.metrics.questionEmbeddingMs,
        retrievalMs: answer.metrics.retrievalMs,
        timeToFirstTokenMs: answer.metrics.timeToFirstTokenMs,
        generationMs: answer.metrics.generationMs,
        totalMs: answer.metrics.totalMs,
        tokensPerSecond: answer.metrics.tokensPerSecond,
        outputTokens: answer.metrics.outputTokens,
        retrievedChunks: answer.sources.length,
        topScore: answer.sources[0]?.score ?? null,
        error: null,
      });
    } catch (error) {
      results.push({
        question,
        answerChars: 0,
        questionEmbeddingMs: null,
        retrievalMs: null,
        timeToFirstTokenMs: null,
        generationMs: null,
        totalMs: Date.now() - startedAt,
        tokensPerSecond: null,
        outputTokens: null,
        retrievedChunks: 0,
        topScore: null,
        error: error instanceof Error ? error.message : "unknown error",
      });
    }
  }

  onProgress?.(BENCHMARK_QUESTIONS.length, BENCHMARK_QUESTIONS.length, "done");

  return {
    id: createId("bench"),
    documentId: document.id,
    documentName: document.filename,
    chatModel,
    embeddingModel,
    createdAt: Date.now(),
    serverVersion: health?.version ?? null,
    osVersion: systemInfo?.["OS Version"] ?? null,
    processor: systemInfo?.["Processor"] ?? null,
    results,
  };
}

/** Averages a numeric field over successful questions only. */
export function averageOf(
  results: BenchmarkQuestionResult[],
  field: keyof BenchmarkQuestionResult,
): number | null {
  const values = results
    .filter((result) => result.error === null)
    .map((result) => result[field])
    .filter((value): value is number => typeof value === "number");

  if (values.length === 0) return null;
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}
