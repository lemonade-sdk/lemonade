import type { Chunk, StoredDocument } from "@/lib/documents/types";
import {
  db,
  type AppSettings,
  type BenchmarkRun,
  type ChatTurn,
  type ExtractionRecord,
  type StoredPage,
} from "./schema";

const SETTINGS_KEY = "settings" as const;

export const DEFAULT_SETTINGS: AppSettings = {
  key: SETTINGS_KEY,
  chatModel: null,
  embeddingModel: null,
  activeDocumentId: null,
  theme: "system",
  updatedAt: 0,
};

export async function getSettings(): Promise<AppSettings> {
  const stored = await db().settings.get(SETTINGS_KEY);
  return stored ?? DEFAULT_SETTINGS;
}

export async function saveSettings(patch: Partial<Omit<AppSettings, "key">>): Promise<AppSettings> {
  const current = await getSettings();
  const next: AppSettings = { ...current, ...patch, key: SETTINGS_KEY, updatedAt: Date.now() };
  await db().settings.put(next);
  return next;
}

export async function listDocuments(): Promise<StoredDocument[]> {
  const docs = await db().documents.toArray();
  return docs.sort((a, b) => b.createdAt - a.createdAt);
}

export async function getDocument(id: string): Promise<StoredDocument | undefined> {
  return db().documents.get(id);
}

export async function saveDocument(
  document: StoredDocument,
  pages: StoredPage[],
  chunks: Chunk[],
): Promise<void> {
  const database = db();
  await database.transaction("rw", database.documents, database.pages, database.chunks, async () => {
    await database.pages.where("documentId").equals(document.id).delete();
    await database.chunks.where("documentId").equals(document.id).delete();
    await database.documents.put(document);
    await database.pages.bulkPut(pages);
    await database.chunks.bulkPut(chunks);
  });
}

export async function updateDocument(
  id: string,
  patch: Partial<StoredDocument>,
): Promise<void> {
  await db().documents.update(id, { ...patch, updatedAt: Date.now() });
}

export async function getChunks(documentId: string): Promise<Chunk[]> {
  const chunks = await db().chunks.where("documentId").equals(documentId).toArray();
  return chunks.sort((a, b) => a.chunkNumber - b.chunkNumber);
}

export async function getPages(documentId: string): Promise<StoredPage[]> {
  const pages = await db().pages.where("documentId").equals(documentId).toArray();
  return pages.sort((a, b) => a.page - b.page);
}

export async function putChunkEmbeddings(chunks: Chunk[]): Promise<void> {
  await db().chunks.bulkPut(chunks);
}

export async function deleteDocument(id: string): Promise<void> {
  const database = db();
  await database.transaction(
    "rw",
    [
      database.documents,
      database.pages,
      database.chunks,
      database.chats,
      database.extractions,
      database.benchmarks,
    ],
    async () => {
      await database.documents.delete(id);
      await database.pages.where("documentId").equals(id).delete();
      await database.chunks.where("documentId").equals(id).delete();
      await database.chats.where("documentId").equals(id).delete();
      await database.extractions.where("documentId").equals(id).delete();
      await database.benchmarks.where("documentId").equals(id).delete();
    },
  );

  const settings = await getSettings();
  if (settings.activeDocumentId === id) await saveSettings({ activeDocumentId: null });
}

/** Wipes every table. Model choices and theme go too — this is "delete all local data". */
export async function deleteAllData(): Promise<void> {
  const database = db();
  await database.transaction(
    "rw",
    [
      database.documents,
      database.pages,
      database.chunks,
      database.chats,
      database.extractions,
      database.benchmarks,
      database.settings,
    ],
    async () => {
      await Promise.all([
        database.documents.clear(),
        database.pages.clear(),
        database.chunks.clear(),
        database.chats.clear(),
        database.extractions.clear(),
        database.benchmarks.clear(),
        database.settings.clear(),
      ]);
    },
  );
}

export async function listChats(documentId: string): Promise<ChatTurn[]> {
  const turns = await db().chats.where("documentId").equals(documentId).toArray();
  return turns.sort((a, b) => a.createdAt - b.createdAt);
}

export async function addChatTurn(turn: ChatTurn): Promise<void> {
  await db().chats.put(turn);
}

export async function clearChats(documentId: string): Promise<void> {
  await db().chats.where("documentId").equals(documentId).delete();
}

/** Number of questions the student has asked, across all documents. */
export async function countQuestionsAsked(): Promise<number> {
  return db().chats.filter((turn) => turn.role === "user").count();
}

export async function saveExtraction(record: ExtractionRecord): Promise<void> {
  const database = db();
  await database.transaction("rw", database.extractions, async () => {
    await database.extractions
      .where("[documentId+kind]")
      .equals([record.documentId, record.kind])
      .delete();
    await database.extractions.put(record);
  });
}

export async function getExtraction(
  documentId: string,
  kind: ExtractionRecord["kind"],
): Promise<ExtractionRecord | undefined> {
  return db()
    .extractions.where("[documentId+kind]")
    .equals([documentId, kind])
    .first();
}

export async function listExtractions(
  kind: ExtractionRecord["kind"],
): Promise<ExtractionRecord[]> {
  const records = await db().extractions.where("kind").equals(kind).toArray();
  return records.sort((a, b) => b.createdAt - a.createdAt);
}

export async function saveBenchmark(run: BenchmarkRun): Promise<void> {
  await db().benchmarks.put(run);
}

export async function listBenchmarks(): Promise<BenchmarkRun[]> {
  const runs = await db().benchmarks.toArray();
  return runs.sort((a, b) => b.createdAt - a.createdAt);
}
