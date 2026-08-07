export interface ExtractedPage {
  /** 1-based page number for PDFs, or 1-based section index for text files. */
  page: number;
  /** Heading text for Markdown sections; null for PDF pages. */
  section: string | null;
  text: string;
  charCount: number;
  /** True when a PDF page produced almost no text and likely needs OCR. */
  likelyScanned: boolean;
}

export interface ExtractionResult {
  pages: ExtractedPage[];
  totalChars: number;
  pageCount: number;
  scannedPageCount: number;
  truncated: boolean;
  /** User-facing warnings, e.g. "OCR required". */
  warnings: string[];
  durationMs: number;
}

export interface Chunk {
  id: string;
  documentId: string;
  filename: string;
  page: number;
  section: string | null;
  chunkNumber: number;
  text: string;
  wordCount: number;
  embedding: number[] | null;
  createdAt: number;
}

export interface StoredDocument {
  id: string;
  filename: string;
  kind: "pdf" | "text" | "markdown";
  sizeBytes: number;
  pageCount: number;
  chunkCount: number;
  totalChars: number;
  embeddingModel: string | null;
  embeddingDimensions: number | null;
  createdAt: number;
  updatedAt: number;
  status: "extracted" | "embedded" | "failed";
  warnings: string[];
  /** Genuine measurements captured while processing this document. */
  timings: {
    extractionMs: number;
    chunkingMs: number;
    embeddingMs: number | null;
  };
}
