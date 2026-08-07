import { cleanText, looksScanned, splitIntoSections } from "./clean";
import { hasPdfMagic, LIMITS, type DocumentKind } from "./limits";
import type { ExtractedPage, ExtractionResult } from "./types";

export class ExtractionError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ExtractionError";
  }
}

/** Builds an ExtractionResult from already-split pages, applying char limits. */
export function assemblePages(
  rawPages: { page: number; section: string | null; text: string }[],
  startedAt: number,
): ExtractionResult {
  const pages: ExtractedPage[] = [];
  const warnings: string[] = [];
  let totalChars = 0;
  let truncated = false;
  let scannedPageCount = 0;

  for (const raw of rawPages) {
    if (pages.length >= LIMITS.maxPages) {
      truncated = true;
      break;
    }

    const text = cleanText(raw.text);
    const likelyScanned = looksScanned(text, LIMITS.minCharsForTextPage);
    if (likelyScanned) scannedPageCount += 1;

    let pageText = text;
    if (totalChars + pageText.length > LIMITS.maxExtractedChars) {
      pageText = pageText.slice(0, Math.max(0, LIMITS.maxExtractedChars - totalChars));
      truncated = true;
    }

    totalChars += pageText.length;
    pages.push({
      page: raw.page,
      section: raw.section,
      text: pageText,
      charCount: pageText.length,
      likelyScanned,
    });

    if (truncated) break;
  }

  if (truncated) {
    warnings.push(
      `Document was truncated at ${pages.length} pages / ${totalChars.toLocaleString()} characters to stay within local processing limits.`,
    );
  }

  const textPages = pages.length - scannedPageCount;
  if (pages.length > 0 && textPages === 0) {
    warnings.push(
      "No text layer was found in this document. It looks like a scanned PDF, which needs OCR before it can be searched. OCR is not included in this version.",
    );
  } else if (scannedPageCount > 0) {
    warnings.push(
      `${scannedPageCount} of ${pages.length} pages had almost no extractable text and were likely scanned images. Answers cannot cover those pages without OCR.`,
    );
  }

  return {
    pages,
    totalChars,
    pageCount: pages.length,
    scannedPageCount,
    truncated,
    warnings,
    durationMs: Date.now() - startedAt,
  };
}

/** Splits plain text or Markdown into heading-aware "pages". */
export function extractFromText(content: string, kind: DocumentKind): ExtractionResult {
  const startedAt = Date.now();

  if (kind === "markdown") {
    const sections = splitIntoSections(content).filter(
      (section) => section.text.trim().length > 0 || section.title,
    );
    return assemblePages(
      sections.map((section, index) => ({
        page: index + 1,
        section: section.title,
        text: section.title ? `${section.title}\n${section.text}` : section.text,
      })),
      startedAt,
    );
  }

  // Plain text: split on blank-line paragraph groups so citations stay useful
  // on long files, while short files remain a single page.
  const paragraphs = content.split(/\n{2,}/);
  const groups: string[] = [];
  let buffer: string[] = [];
  let bufferChars = 0;

  for (const paragraph of paragraphs) {
    buffer.push(paragraph);
    bufferChars += paragraph.length;
    if (bufferChars >= 3000) {
      groups.push(buffer.join("\n\n"));
      buffer = [];
      bufferChars = 0;
    }
  }
  if (buffer.length > 0) groups.push(buffer.join("\n\n"));

  return assemblePages(
    (groups.length > 0 ? groups : [content]).map((text, index) => ({
      page: index + 1,
      section: null,
      text,
    })),
    startedAt,
  );
}

/**
 * Extracts a PDF page by page in the browser using PDF.js. The worker is loaded
 * from the bundled copy of pdfjs-dist — nothing is fetched from a CDN.
 */
export async function extractFromPdf(
  data: ArrayBuffer,
  onProgress?: (done: number, total: number) => void,
): Promise<ExtractionResult> {
  const startedAt = Date.now();

  if (!hasPdfMagic(new Uint8Array(data.slice(0, 8)))) {
    throw new ExtractionError("That file is not a valid PDF (missing %PDF- header).");
  }

  const pdfjs = await import("pdfjs-dist");
  pdfjs.GlobalWorkerOptions.workerSrc = new URL(
    "pdfjs-dist/build/pdf.worker.min.mjs",
    import.meta.url,
  ).toString();

  const loadingTask = pdfjs.getDocument({
    data: new Uint8Array(data),
    // A campus notice never needs remote resources; refusing them keeps
    // processing genuinely offline.
    disableAutoFetch: true,
    isEvalSupported: false,
  });

  const pdf = await loadingTask.promise;
  const total = Math.min(pdf.numPages, LIMITS.maxPages);
  const rawPages: { page: number; section: string | null; text: string }[] = [];

  try {
    for (let pageNumber = 1; pageNumber <= total; pageNumber += 1) {
      const page = await pdf.getPage(pageNumber);
      const content = await page.getTextContent();
      const text = content.items
        .map((item) => ("str" in item ? item.str : ""))
        .join(" ");
      rawPages.push({ page: pageNumber, section: null, text });
      page.cleanup();
      onProgress?.(pageNumber, total);
    }
  } finally {
    await pdf.destroy();
  }

  const result = assemblePages(rawPages, startedAt);
  if (pdf.numPages > LIMITS.maxPages) {
    result.warnings.push(
      `Only the first ${LIMITS.maxPages} of ${pdf.numPages} pages were processed.`,
    );
  }
  return result;
}
