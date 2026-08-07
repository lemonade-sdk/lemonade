/**
 * Cosine similarity. Returns 0 for zero-magnitude vectors rather than NaN so a
 * degenerate embedding can never poison a ranking.
 */
export function cosineSimilarity(a: readonly number[], b: readonly number[]): number {
  if (a.length !== b.length) {
    throw new Error(`Vector length mismatch: ${a.length} vs ${b.length}`);
  }

  let dot = 0;
  let magnitudeA = 0;
  let magnitudeB = 0;

  for (let i = 0; i < a.length; i += 1) {
    const x = a[i] as number;
    const y = b[i] as number;
    dot += x * y;
    magnitudeA += x * x;
    magnitudeB += y * y;
  }

  if (magnitudeA === 0 || magnitudeB === 0) return 0;
  return dot / (Math.sqrt(magnitudeA) * Math.sqrt(magnitudeB));
}

export interface ScoredItem<T> {
  item: T;
  score: number;
}

/**
 * Ranks `items` against `query` and returns the best `topK` above `minScore`.
 * Ties break on the earlier item so results are deterministic.
 */
export function topKBySimilarity<T>(
  query: readonly number[],
  items: readonly T[],
  getVector: (item: T) => readonly number[] | null,
  options: { topK: number; minScore: number },
): ScoredItem<T>[] {
  const scored: (ScoredItem<T> & { index: number })[] = [];

  items.forEach((item, index) => {
    const vector = getVector(item);
    if (!vector || vector.length !== query.length) return;
    const score = cosineSimilarity(query, vector);
    if (score < options.minScore) return;
    scored.push({ item, score, index });
  });

  scored.sort((a, b) => (b.score === a.score ? a.index - b.index : b.score - a.score));

  return scored.slice(0, options.topK).map(({ item, score }) => ({ item, score }));
}
