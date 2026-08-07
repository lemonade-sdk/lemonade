import type { LemonadeClient } from "@/lib/lemonade/client";
import type { Chunk } from "@/lib/documents/types";

/**
 * Lemonade loads the embedding model on first use and processes a batch in one
 * backend call. Small batches keep memory predictable and give the UI frequent
 * progress updates.
 */
export const EMBEDDING_BATCH_SIZE = 8;

export interface EmbedProgress {
  completed: number;
  total: number;
}

export interface EmbedChunksResult {
  chunks: Chunk[];
  dimensions: number;
  durationMs: number;
}

export async function embedChunks(
  client: LemonadeClient,
  model: string,
  chunks: Chunk[],
  options: {
    signal?: AbortSignal | undefined;
    onProgress?: ((progress: EmbedProgress) => void) | undefined;
    batchSize?: number;
  } = {},
): Promise<EmbedChunksResult> {
  const batchSize = options.batchSize ?? EMBEDDING_BATCH_SIZE;
  const startedAt = Date.now();
  const embedded: Chunk[] = [];
  let dimensions = 0;

  for (let start = 0; start < chunks.length; start += batchSize) {
    const batch = chunks.slice(start, start + batchSize);
    const { value: vectors } = await client.embed(
      model,
      batch.map((chunk) => chunk.text),
      { signal: options.signal },
    );

    batch.forEach((chunk, index) => {
      const embedding = vectors[index] as number[];
      if (dimensions === 0) dimensions = embedding.length;
      embedded.push({ ...chunk, embedding });
    });

    options.onProgress?.({ completed: embedded.length, total: chunks.length });
  }

  return { chunks: embedded, dimensions, durationMs: Date.now() - startedAt };
}

export async function embedQuestion(
  client: LemonadeClient,
  model: string,
  question: string,
  signal?: AbortSignal,
): Promise<{ vector: number[]; durationMs: number }> {
  const { value, durationMs } = await client.embed(model, [question], { signal });
  return { vector: value[0] as number[], durationMs };
}
