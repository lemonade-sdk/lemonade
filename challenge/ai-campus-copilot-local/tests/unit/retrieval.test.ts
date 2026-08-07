import { describe, expect, it } from "vitest";
import { cosineSimilarity, topKBySimilarity } from "@/lib/rag/similarity";
import { formatContext, retrieveRelevantChunks, truncate } from "@/lib/rag/retrieve";
import type { Chunk } from "@/lib/documents/types";

describe("cosineSimilarity", () => {
  it("returns 1 for identical vectors", () => {
    expect(cosineSimilarity([1, 2, 3], [1, 2, 3])).toBeCloseTo(1, 10);
  });

  it("is scale invariant", () => {
    expect(cosineSimilarity([1, 2, 3], [2, 4, 6])).toBeCloseTo(1, 10);
  });

  it("returns 0 for orthogonal vectors", () => {
    expect(cosineSimilarity([1, 0], [0, 1])).toBeCloseTo(0, 10);
  });

  it("returns -1 for opposite vectors", () => {
    expect(cosineSimilarity([1, 0], [-1, 0])).toBeCloseTo(-1, 10);
  });

  it("returns 0 rather than NaN when a vector has zero magnitude", () => {
    expect(cosineSimilarity([0, 0], [1, 2])).toBe(0);
    expect(cosineSimilarity([0, 0], [0, 0])).toBe(0);
  });

  it("is symmetric", () => {
    const a = [0.2, -0.5, 0.9];
    const b = [0.7, 0.1, -0.3];
    expect(cosineSimilarity(a, b)).toBeCloseTo(cosineSimilarity(b, a), 12);
  });

  it("throws on length mismatch instead of silently comparing", () => {
    expect(() => cosineSimilarity([1, 2], [1, 2, 3])).toThrow(/length mismatch/i);
  });
});

describe("topKBySimilarity", () => {
  const items = [
    { id: "a", vector: [1, 0, 0] },
    { id: "b", vector: [0.9, 0.1, 0] },
    { id: "c", vector: [0, 1, 0] },
    { id: "d", vector: [-1, 0, 0] },
    { id: "e", vector: null },
  ];

  const rank = (topK: number, minScore: number) =>
    topKBySimilarity([1, 0, 0], items, (item) => item.vector, { topK, minScore });

  it("sorts by descending score", () => {
    const result = rank(5, -1);
    expect(result.map((entry) => entry.item.id)).toEqual(["a", "b", "c", "d"]);
    for (let i = 0; i < result.length - 1; i += 1) {
      expect((result[i] as { score: number }).score).toBeGreaterThanOrEqual(
        (result[i + 1] as { score: number }).score,
      );
    }
  });

  it("respects topK", () => {
    expect(rank(2, -1)).toHaveLength(2);
  });

  it("drops items below the minimum score", () => {
    expect(rank(5, 0.5).map((entry) => entry.item.id)).toEqual(["a", "b"]);
  });

  it("skips items with no vector", () => {
    expect(rank(5, -1).some((entry) => entry.item.id === "e")).toBe(false);
  });

  it("skips items whose vector length does not match the query", () => {
    const mismatched = [{ id: "x", vector: [1, 0] }];
    expect(
      topKBySimilarity([1, 0, 0], mismatched, (item) => item.vector, { topK: 5, minScore: -1 }),
    ).toEqual([]);
  });

  it("breaks ties on original order for determinism", () => {
    const tied = [
      { id: "first", vector: [1, 0] },
      { id: "second", vector: [2, 0] },
    ];
    const result = topKBySimilarity([1, 0], tied, (item) => item.vector, {
      topK: 2,
      minScore: -1,
    });
    expect(result.map((entry) => entry.item.id)).toEqual(["first", "second"]);
  });

  it("returns an empty list when nothing clears the threshold", () => {
    expect(rank(5, 0.999999)).toHaveLength(1);
    expect(rank(5, 1.5)).toHaveLength(0);
  });
});

function chunk(id: string, page: number, embedding: number[] | null, text = `text ${id}`): Chunk {
  return {
    id,
    documentId: "doc1",
    filename: "notice.pdf",
    page,
    section: null,
    chunkNumber: Number(id.replace(/\D/g, "")) || 1,
    text,
    wordCount: text.split(" ").length,
    embedding,
    createdAt: 0,
  };
}

describe("retrieveRelevantChunks", () => {
  const chunks = [
    chunk("c1", 1, [1, 0, 0], "Semester examinations begin on 12 May 2027."),
    chunk("c2", 2, [0.8, 0.2, 0], "Hall tickets are issued one week before."),
    chunk("c3", 3, [0, 1, 0], "The canteen menu changes on Fridays."),
    chunk("c4", 4, null, "This chunk was never embedded."),
  ];

  it("returns citation-ready sources ordered by relevance", () => {
    const result = retrieveRelevantChunks([1, 0, 0], chunks, { topK: 2, minScore: 0 });

    expect(result.sources.map((source) => source.chunkId)).toEqual(["c1", "c2"]);
    expect(result.sources[0]).toMatchObject({ page: 1, chunkNumber: 1 });
    expect(result.sources[0]?.score).toBeCloseTo(1, 10);
    expect(result.durationMs).toBeGreaterThanOrEqual(0);
  });

  it("reports how many chunks were actually searchable", () => {
    expect(retrieveRelevantChunks([1, 0, 0], chunks, { minScore: 0 }).searchedChunks).toBe(3);
  });

  it("returns no sources when nothing is relevant enough", () => {
    const result = retrieveRelevantChunks([0, 0, 1], chunks, { minScore: 0.5 });
    expect(result.sources).toEqual([]);
  });
});

describe("truncate", () => {
  it("leaves short text untouched", () => {
    expect(truncate("hello", 10)).toBe("hello");
  });

  it("adds an ellipsis when cutting", () => {
    expect(truncate("hello world", 5)).toBe("hello…");
  });
});

describe("formatContext", () => {
  it("numbers sources and labels each with its page and chunk", () => {
    const result = retrieveRelevantChunks([1, 0, 0], [chunk("c1", 7, [1, 0, 0], "Exam text")], {
      minScore: 0,
    });
    const context = formatContext(result.sources);

    expect(context).toContain("[1] page 7, chunk 1");
    expect(context).toContain("Exam text");
  });
});
