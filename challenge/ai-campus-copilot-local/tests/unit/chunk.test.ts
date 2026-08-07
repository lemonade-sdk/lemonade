import { describe, expect, it } from "vitest";
import { chunkDocument, chunkPageText, citationLabel } from "@/lib/documents/chunk";
import type { ExtractedPage } from "@/lib/documents/types";

const OPTIONS = { targetWords: 10, overlapWords: 3 };

function words(count: number, prefix = "w"): string {
  return Array.from({ length: count }, (_, index) => `${prefix}${index}`).join(" ");
}

function page(overrides: Partial<ExtractedPage> & { page: number; text: string }): ExtractedPage {
  return {
    section: null,
    charCount: overrides.text.length,
    likelyScanned: false,
    ...overrides,
  };
}

describe("chunkPageText", () => {
  it("returns a single chunk when the page fits in one window", () => {
    expect(chunkPageText(words(10), OPTIONS)).toEqual([words(10)]);
    expect(chunkPageText(words(4), OPTIONS)).toEqual([words(4)]);
  });

  it("returns nothing for empty or whitespace-only text", () => {
    expect(chunkPageText("", OPTIONS)).toEqual([]);
    expect(chunkPageText("   \n\t ", OPTIONS)).toEqual([]);
  });

  it("splits with a deterministic stride of targetWords minus overlap", () => {
    const chunks = chunkPageText(words(24), OPTIONS);

    // Stride 7 over 24 words -> windows at 0, 7, 14. The window at 14 reaches
    // the final word, so no redundant tail chunk is emitted.
    expect(chunks).toHaveLength(3);
    expect(chunks[0]).toBe(words(10));
    expect(chunks[1]?.split(" ")[0]).toBe("w7");
    expect(chunks[2]?.split(" ")[0]).toBe("w14");
    expect(chunks.at(-1)?.split(" ").at(-1)).toBe("w23");
  });

  it("overlaps consecutive chunks by exactly overlapWords", () => {
    const chunks = chunkPageText(words(24), OPTIONS);

    for (let i = 0; i < chunks.length - 1; i += 1) {
      const previous = (chunks[i] as string).split(" ");
      const next = (chunks[i + 1] as string).split(" ");
      const tail = previous.slice(-OPTIONS.overlapWords);
      const head = next.slice(0, OPTIONS.overlapWords);
      expect(head).toEqual(tail);
    }
  });

  it("covers every word of the source with no gaps", () => {
    const source = words(37);
    const covered = new Set(chunkPageText(source, OPTIONS).flatMap((chunk) => chunk.split(" ")));
    expect(covered.size).toBe(37);
  });

  it("terminates when overlap is greater than or equal to chunk size", () => {
    const chunks = chunkPageText(words(30), { targetWords: 5, overlapWords: 9 });
    expect(chunks.length).toBeGreaterThan(0);
    expect(chunks.length).toBeLessThanOrEqual(30);
  });

  it("is deterministic across repeated calls", () => {
    const source = words(50);
    expect(chunkPageText(source, OPTIONS)).toEqual(chunkPageText(source, OPTIONS));
  });
});

describe("chunkDocument", () => {
  const pages: ExtractedPage[] = [
    page({ page: 1, text: words(24, "a") }),
    page({ page: 2, text: words(5, "b") }),
    page({ page: 3, text: "" }),
    page({ page: 4, section: "Eligibility", text: words(12, "c") }),
  ];

  const chunks = chunkDocument(pages, { documentId: "doc1", filename: "notice.pdf" }, OPTIONS, () => 1234);

  it("never merges text across page boundaries", () => {
    for (const chunk of chunks) {
      const prefixes = new Set(chunk.text.split(" ").map((word) => word[0]));
      expect(prefixes.size).toBe(1);
    }
  });

  it("numbers chunks sequentially across the whole document", () => {
    expect(chunks.map((chunk) => chunk.chunkNumber)).toEqual(
      Array.from({ length: chunks.length }, (_, index) => index + 1),
    );
  });

  it("skips pages with no text", () => {
    expect(chunks.some((chunk) => chunk.page === 3)).toBe(false);
  });

  it("carries every required field on each chunk", () => {
    const first = chunks[0];
    expect(first).toMatchObject({
      documentId: "doc1",
      filename: "notice.pdf",
      page: 1,
      chunkNumber: 1,
      embedding: null,
      createdAt: 1234,
    });
    expect(first?.id).toBe("doc1:1");
    expect(first?.wordCount).toBe(10);
  });

  it("preserves section labels for the pages that have them", () => {
    const sectioned = chunks.filter((chunk) => chunk.page === 4);
    expect(sectioned.length).toBeGreaterThan(0);
    expect(sectioned.every((chunk) => chunk.section === "Eligibility")).toBe(true);
  });
});

describe("citationLabel", () => {
  it("prefers the section name when one exists", () => {
    expect(citationLabel({ page: 4, section: "Eligibility" })).toBe("Eligibility (page 4)");
    expect(citationLabel({ page: 2, section: null })).toBe("page 2");
  });
});
