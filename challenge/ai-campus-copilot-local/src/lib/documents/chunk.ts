import { countWords } from "./clean";
import type { Chunk, ExtractedPage } from "./types";

export interface ChunkOptions {
  targetWords: number;
  overlapWords: number;
}

export const DEFAULT_CHUNK_OPTIONS: ChunkOptions = {
  targetWords: 500,
  overlapWords: 80,
};

/**
 * Splits one page into word windows of `targetWords` that share `overlapWords`
 * with the previous window. Pages are never merged, so every chunk keeps an
 * exact page (or section) citation.
 */
export function chunkPageText(text: string, options: ChunkOptions): string[] {
  const words = text.trim().split(/\s+/).filter(Boolean);
  if (words.length === 0) return [];
  if (words.length <= options.targetWords) return [words.join(" ")];

  const stride = Math.max(1, options.targetWords - options.overlapWords);
  const chunks: string[] = [];

  for (let start = 0; start < words.length; start += stride) {
    const window = words.slice(start, start + options.targetWords);
    chunks.push(window.join(" "));
    if (start + options.targetWords >= words.length) break;
  }

  return chunks;
}

/**
 * Page-aware chunking across a whole document. `chunkNumber` is sequential over
 * the document so citations read naturally ("page 3, chunk 7").
 */
export function chunkDocument(
  pages: ExtractedPage[],
  meta: { documentId: string; filename: string },
  options: ChunkOptions = DEFAULT_CHUNK_OPTIONS,
  now: () => number = Date.now,
): Chunk[] {
  const chunks: Chunk[] = [];
  let chunkNumber = 0;

  for (const page of pages) {
    for (const text of chunkPageText(page.text, options)) {
      chunkNumber += 1;
      chunks.push({
        id: `${meta.documentId}:${chunkNumber}`,
        documentId: meta.documentId,
        filename: meta.filename,
        page: page.page,
        section: page.section,
        chunkNumber,
        text,
        wordCount: countWords(text),
        embedding: null,
        createdAt: now(),
      });
    }
  }

  return chunks;
}

export function citationLabel(chunk: Pick<Chunk, "page" | "section">): string {
  return chunk.section ? `${chunk.section} (page ${chunk.page})` : `page ${chunk.page}`;
}
