import { chunkDocument, DEFAULT_CHUNK_OPTIONS } from "./chunk";
import { extractFromPdf, extractFromText, ExtractionError } from "./extract";
import { validateFile, type DocumentKind, type ValidatableFile } from "./limits";
import type { Chunk, ExtractionResult, StoredDocument } from "./types";
import { embedChunks } from "@/lib/rag/embed";
import { saveDocument, updateDocument } from "@/lib/db/repo";
import type { StoredPage } from "@/lib/db/schema";
import type { LemonadeClient } from "@/lib/lemonade/client";
import { createId } from "@/lib/utils/cn";

export type ProcessingPhase =
  | "validating"
  | "extracting"
  | "chunking"
  | "embedding"
  | "saving"
  | "done";

export interface ProcessingProgress {
  phase: ProcessingPhase;
  message: string;
  /** 0-100, or null when the phase has no measurable progress. */
  percent: number | null;
}

export interface ProcessFileOptions {
  client: LemonadeClient;
  embeddingModel: string;
  signal?: AbortSignal | undefined;
  onProgress?: ((progress: ProcessingProgress) => void) | undefined;
  /** Reuse an existing document id when reprocessing. */
  documentId?: string;
}

export interface ProcessFileResult {
  document: StoredDocument;
  chunks: Chunk[];
  extraction: ExtractionResult;
}

/**
 * Full local pipeline: validate, extract page by page, chunk, embed via
 * Lemonade, and persist to IndexedDB. Every stage is timed with real
 * measurements that the performance page later reports.
 */
export async function processFile(
  file: File,
  options: ProcessFileOptions,
): Promise<ProcessFileResult> {
  const { client, embeddingModel, signal, onProgress } = options;

  onProgress?.({ phase: "validating", message: "Checking the file", percent: null });
  const validation = validateFile(file as ValidatableFile);
  if (!validation.ok) throw new ExtractionError(validation.reason);

  const documentId = options.documentId ?? createId("doc");
  const { filename, kind } = validation;

  onProgress?.({ phase: "extracting", message: "Extracting text", percent: 0 });
  const extraction = await extractContent(file, kind, (done, total) => {
    onProgress?.({
      phase: "extracting",
      message: `Extracting page ${done} of ${total}`,
      percent: (done / total) * 100,
    });
  });

  if (extraction.pages.length === 0 || extraction.totalChars === 0) {
    throw new ExtractionError(
      "No readable text was found in this file. If it is a scanned PDF it needs OCR, which this version does not include.",
    );
  }

  onProgress?.({ phase: "chunking", message: "Splitting into page-aware chunks", percent: null });
  const chunkingStartedAt = Date.now();
  const chunks = chunkDocument(
    extraction.pages,
    { documentId, filename },
    DEFAULT_CHUNK_OPTIONS,
  );
  const chunkingMs = Date.now() - chunkingStartedAt;

  const pages: StoredPage[] = extraction.pages.map((page) => ({
    ...page,
    id: `${documentId}:p${page.page}`,
    documentId,
  }));

  const baseDocument: StoredDocument = {
    id: documentId,
    filename,
    kind,
    sizeBytes: file.size,
    pageCount: extraction.pageCount,
    chunkCount: chunks.length,
    totalChars: extraction.totalChars,
    embeddingModel: null,
    embeddingDimensions: null,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    status: "extracted",
    warnings: extraction.warnings,
    timings: {
      extractionMs: extraction.durationMs,
      chunkingMs,
      embeddingMs: null,
    },
  };

  onProgress?.({ phase: "saving", message: "Saving locally", percent: null });
  await saveDocument(baseDocument, pages, chunks);

  onProgress?.({
    phase: "embedding",
    message: `Embedding 0 of ${chunks.length} chunks with ${embeddingModel}`,
    percent: 0,
  });

  const embedded = await embedChunks(client, embeddingModel, chunks, {
    signal,
    onProgress: ({ completed, total }) =>
      onProgress?.({
        phase: "embedding",
        message: `Embedding ${completed} of ${total} chunks with ${embeddingModel}`,
        percent: (completed / total) * 100,
      }),
  });

  const document: StoredDocument = {
    ...baseDocument,
    embeddingModel,
    embeddingDimensions: embedded.dimensions,
    status: "embedded",
    updatedAt: Date.now(),
    timings: { ...baseDocument.timings, embeddingMs: embedded.durationMs },
  };

  onProgress?.({ phase: "saving", message: "Storing embeddings locally", percent: null });
  await saveDocument(document, pages, embedded.chunks);

  onProgress?.({ phase: "done", message: "Ready", percent: 100 });
  return { document, chunks: embedded.chunks, extraction };
}

/**
 * Re-embeds already-extracted chunks without re-reading the source file, which
 * is what "rebuild embeddings" needs after switching embedding model.
 */
export async function rebuildEmbeddings(
  document: StoredDocument,
  chunks: Chunk[],
  options: Omit<ProcessFileOptions, "documentId">,
): Promise<Chunk[]> {
  const { client, embeddingModel, signal, onProgress } = options;

  const embedded = await embedChunks(
    client,
    embeddingModel,
    chunks.map((chunk) => ({ ...chunk, embedding: null })),
    {
      signal,
      onProgress: ({ completed, total }) =>
        onProgress?.({
          phase: "embedding",
          message: `Embedding ${completed} of ${total} chunks with ${embeddingModel}`,
          percent: (completed / total) * 100,
        }),
    },
  );

  const { putChunkEmbeddings } = await import("@/lib/db/repo");
  await putChunkEmbeddings(embedded.chunks);
  await updateDocument(document.id, {
    embeddingModel,
    embeddingDimensions: embedded.dimensions,
    status: "embedded",
    timings: { ...document.timings, embeddingMs: embedded.durationMs },
  });

  return embedded.chunks;
}

async function extractContent(
  file: File,
  kind: DocumentKind,
  onPageProgress: (done: number, total: number) => void,
): Promise<ExtractionResult> {
  if (kind === "pdf") {
    return extractFromPdf(await file.arrayBuffer(), onPageProgress);
  }
  return extractFromText(await file.text(), kind);
}
