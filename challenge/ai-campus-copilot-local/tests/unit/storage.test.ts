/** Local IndexedDB storage, exercised against fake-indexeddb. */
import { beforeEach, describe, expect, it } from "vitest";
import {
  addChatTurn,
  clearChats,
  countQuestionsAsked,
  deleteAllData,
  deleteDocument,
  getChunks,
  getExtraction,
  getSettings,
  listChats,
  listDocuments,
  saveDocument,
  saveExtraction,
  saveSettings,
} from "@/lib/db/repo";
import type { StoredPage } from "@/lib/db/schema";
import type { Chunk, StoredDocument } from "@/lib/documents/types";

function document(id: string): StoredDocument {
  return {
    id,
    filename: `${id}.pdf`,
    kind: "pdf",
    sizeBytes: 1234,
    pageCount: 1,
    chunkCount: 2,
    totalChars: 100,
    embeddingModel: null,
    embeddingDimensions: null,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    status: "extracted",
    warnings: [],
    timings: { extractionMs: 10, chunkingMs: 2, embeddingMs: null },
  };
}

function chunk(documentId: string, number: number): Chunk {
  return {
    id: `${documentId}:${number}`,
    documentId,
    filename: `${documentId}.pdf`,
    page: 1,
    section: null,
    chunkNumber: number,
    text: `chunk ${number}`,
    wordCount: 2,
    embedding: [0.1, 0.2],
    createdAt: Date.now(),
  };
}

function page(documentId: string): StoredPage {
  return {
    id: `${documentId}:p1`,
    documentId,
    page: 1,
    section: null,
    text: "page text",
    charCount: 9,
    likelyScanned: false,
  };
}

beforeEach(async () => {
  await deleteAllData();
});

describe("document storage", () => {
  it("round-trips a document with its pages and chunks", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1), chunk("d1", 2)]);

    const documents = await listDocuments();
    expect(documents).toHaveLength(1);
    expect(documents[0]?.filename).toBe("d1.pdf");

    const chunks = await getChunks("d1");
    expect(chunks.map((entry) => entry.chunkNumber)).toEqual([1, 2]);
    expect(chunks[0]?.embedding).toEqual([0.1, 0.2]);
  });

  it("replaces chunks on re-save rather than accumulating them", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1), chunk("d1", 2)]);
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1)]);

    expect(await getChunks("d1")).toHaveLength(1);
    expect(await listDocuments()).toHaveLength(1);
  });

  it("cascades a delete to chunks, chats and extractions", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1)]);
    await addChatTurn({
      id: "t1",
      documentId: "d1",
      role: "user",
      content: "hi",
      createdAt: Date.now(),
      sources: [],
      metrics: null,
    });
    await saveExtraction({
      id: "e1",
      documentId: "d1",
      kind: "deadlines",
      payload: [],
      model: "m",
      createdAt: Date.now(),
      durationMs: 1,
    });

    await deleteDocument("d1");

    expect(await listDocuments()).toHaveLength(0);
    expect(await getChunks("d1")).toHaveLength(0);
    expect(await listChats("d1")).toHaveLength(0);
    expect(await getExtraction("d1", "deadlines")).toBeUndefined();
  });

  it("leaves other documents untouched when one is deleted", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1)]);
    await saveDocument(document("d2"), [page("d2")], [chunk("d2", 1)]);

    await deleteDocument("d1");

    expect((await listDocuments()).map((entry) => entry.id)).toEqual(["d2"]);
    expect(await getChunks("d2")).toHaveLength(1);
  });
});

describe("settings", () => {
  it("defaults to no model selected", async () => {
    const settings = await getSettings();
    expect(settings.chatModel).toBeNull();
    expect(settings.embeddingModel).toBeNull();
  });

  it("persists and merges partial updates", async () => {
    await saveSettings({ chatModel: "Qwen3-0.6B-GGUF" });
    await saveSettings({ embeddingModel: "nomic-embed-text-v1-GGUF" });

    const settings = await getSettings();
    expect(settings.chatModel).toBe("Qwen3-0.6B-GGUF");
    expect(settings.embeddingModel).toBe("nomic-embed-text-v1-GGUF");
  });

  it("clears the active document when that document is deleted", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1)]);
    await saveSettings({ activeDocumentId: "d1" });

    await deleteDocument("d1");

    expect((await getSettings()).activeDocumentId).toBeNull();
  });
});

describe("chat history", () => {
  it("returns turns in chronological order and counts questions", async () => {
    await addChatTurn({
      id: "t2",
      documentId: "d1",
      role: "assistant",
      content: "answer",
      createdAt: 200,
      sources: [],
      metrics: null,
    });
    await addChatTurn({
      id: "t1",
      documentId: "d1",
      role: "user",
      content: "question",
      createdAt: 100,
      sources: [],
      metrics: null,
    });

    expect((await listChats("d1")).map((turn) => turn.id)).toEqual(["t1", "t2"]);
    expect(await countQuestionsAsked()).toBe(1);
  });

  it("clears history for one document only", async () => {
    for (const documentId of ["d1", "d2"]) {
      await addChatTurn({
        id: `t-${documentId}`,
        documentId,
        role: "user",
        content: "q",
        createdAt: Date.now(),
        sources: [],
        metrics: null,
      });
    }

    await clearChats("d1");

    expect(await listChats("d1")).toHaveLength(0);
    expect(await listChats("d2")).toHaveLength(1);
  });
});

describe("deleteAllData", () => {
  it("wipes every table including settings", async () => {
    await saveDocument(document("d1"), [page("d1")], [chunk("d1", 1)]);
    await saveSettings({ chatModel: "m" });

    await deleteAllData();

    expect(await listDocuments()).toHaveLength(0);
    expect((await getSettings()).chatModel).toBeNull();
  });
});

describe("extractions", () => {
  it("keeps one record per document and kind", async () => {
    for (const payload of [["first"], ["second"]]) {
      await saveExtraction({
        id: `e-${payload[0]}`,
        documentId: "d1",
        kind: "deadlines",
        payload,
        model: "m",
        createdAt: Date.now(),
        durationMs: 1,
      });
    }

    const record = await getExtraction("d1", "deadlines");
    expect(record?.payload).toEqual(["second"]);
  });

  it("stores different kinds side by side", async () => {
    for (const kind of ["deadlines", "careers"] as const) {
      await saveExtraction({
        id: `e-${kind}`,
        documentId: "d1",
        kind,
        payload: [kind],
        model: "m",
        createdAt: Date.now(),
        durationMs: 1,
      });
    }

    expect((await getExtraction("d1", "deadlines"))?.payload).toEqual(["deadlines"]);
    expect((await getExtraction("d1", "careers"))?.payload).toEqual(["careers"]);
  });
});
