import { citationLabel } from "@/lib/documents/chunk";
import type { Chunk } from "@/lib/documents/types";
import { topKBySimilarity } from "./similarity";

export const RETRIEVAL_DEFAULTS = {
  topK: 5,
  /**
   * Below this cosine score a chunk is treated as unrelated. Kept deliberately
   * low because embedding models differ widely in score distribution; the
   * grounding instructions, not the threshold, are what stop fabrication.
   */
  minScore: 0.2,
  excerptChars: 240,
} as const;

export interface RetrievedSource {
  chunkId: string;
  page: number;
  section: string | null;
  chunkNumber: number;
  score: number;
  excerpt: string;
  text: string;
}

export interface RetrievalResult {
  sources: RetrievedSource[];
  durationMs: number;
  /** Chunks that carried an embedding and were therefore searchable. */
  searchedChunks: number;
}

export function retrieveRelevantChunks(
  questionVector: readonly number[],
  chunks: readonly Chunk[],
  options: { topK?: number; minScore?: number } = {},
): RetrievalResult {
  const startedAt = Date.now();
  const searchedChunks = chunks.filter((chunk) => chunk.embedding !== null).length;

  const ranked = topKBySimilarity(questionVector, chunks, (chunk) => chunk.embedding, {
    topK: options.topK ?? RETRIEVAL_DEFAULTS.topK,
    minScore: options.minScore ?? RETRIEVAL_DEFAULTS.minScore,
  });

  const sources = ranked.map(({ item, score }) => ({
    chunkId: item.id,
    page: item.page,
    section: item.section,
    chunkNumber: item.chunkNumber,
    score,
    excerpt: truncate(item.text, RETRIEVAL_DEFAULTS.excerptChars),
    text: item.text,
  }));

  return { sources, durationMs: Date.now() - startedAt, searchedChunks };
}

export function truncate(text: string, maxChars: number): string {
  if (text.length <= maxChars) return text;
  return `${text.slice(0, maxChars).trimEnd()}…`;
}

/** Renders retrieved chunks as a labelled context block for the chat model. */
export function formatContext(sources: RetrievedSource[]): string {
  return sources
    .map(
      (source, index) =>
        `[${index + 1}] ${citationLabel(source)}, chunk ${source.chunkNumber}\n${source.text}`,
    )
    .join("\n\n---\n\n");
}
