import Dexie, { type EntityTable } from "dexie";
import type { Chunk, ExtractedPage, StoredDocument } from "@/lib/documents/types";

export interface StoredPage extends ExtractedPage {
  id: string;
  documentId: string;
}

export interface ChatTurn {
  id: string;
  documentId: string;
  role: "user" | "assistant";
  content: string;
  createdAt: number;
  /** Citations attached to an assistant turn. */
  sources: {
    chunkId: string;
    page: number;
    section: string | null;
    chunkNumber: number;
    score: number;
    excerpt: string;
  }[];
  /** Genuine per-turn measurements; null when a phase was not measurable. */
  metrics: {
    questionEmbeddingMs: number | null;
    retrievalMs: number | null;
    timeToFirstTokenMs: number | null;
    generationMs: number | null;
    totalMs: number | null;
    tokensPerSecond: number | null;
    outputTokens: number | null;
  } | null;
}

export interface AppSettings {
  key: "settings";
  chatModel: string | null;
  embeddingModel: string | null;
  activeDocumentId: string | null;
  theme: "light" | "dark" | "system";
  updatedAt: number;
}

export interface BenchmarkRun {
  id: string;
  documentId: string;
  documentName: string;
  chatModel: string;
  embeddingModel: string;
  createdAt: number;
  serverVersion: string | null;
  osVersion: string | null;
  processor: string | null;
  results: BenchmarkQuestionResult[];
}

export interface BenchmarkQuestionResult {
  question: string;
  answerChars: number;
  questionEmbeddingMs: number | null;
  retrievalMs: number | null;
  timeToFirstTokenMs: number | null;
  generationMs: number | null;
  totalMs: number;
  tokensPerSecond: number | null;
  outputTokens: number | null;
  retrievedChunks: number;
  topScore: number | null;
  error: string | null;
}

export interface ExtractionRecord {
  id: string;
  documentId: string;
  kind: "deadlines" | "careers" | "summaries";
  payload: unknown;
  model: string;
  createdAt: number;
  durationMs: number;
}

export class CopilotDatabase extends Dexie {
  documents!: EntityTable<StoredDocument, "id">;
  pages!: EntityTable<StoredPage, "id">;
  chunks!: EntityTable<Chunk, "id">;
  chats!: EntityTable<ChatTurn, "id">;
  settings!: EntityTable<AppSettings, "key">;
  benchmarks!: EntityTable<BenchmarkRun, "id">;
  extractions!: EntityTable<ExtractionRecord, "id">;

  constructor() {
    super("ai-campus-copilot-local");
    this.version(1).stores({
      documents: "id, filename, createdAt, status",
      pages: "id, documentId, page",
      chunks: "id, documentId, page, chunkNumber",
      chats: "id, documentId, createdAt",
      settings: "key",
      benchmarks: "id, documentId, createdAt",
      extractions: "id, documentId, kind, [documentId+kind]",
    });
  }
}

let instance: CopilotDatabase | null = null;

/** Lazily constructed so importing this module never touches IndexedDB on the server. */
export function db(): CopilotDatabase {
  if (!instance) instance = new CopilotDatabase();
  return instance;
}

export function resetDatabaseInstance(): void {
  instance = null;
}
