export const LIMITS = {
  maxFileBytes: 20 * 1024 * 1024,
  maxPages: 300,
  maxExtractedChars: 1_500_000,
  /** Below this, a PDF page is treated as scanned rather than text-based. */
  minCharsForTextPage: 24,
} as const;

export const ACCEPTED_EXTENSIONS = [".pdf", ".txt", ".md", ".markdown"] as const;

export type DocumentKind = "pdf" | "text" | "markdown";

const MIME_BY_KIND: Record<DocumentKind, readonly string[]> = {
  pdf: ["application/pdf"],
  text: ["text/plain", ""],
  markdown: ["text/markdown", "text/x-markdown", "text/plain", ""],
};

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export function extensionOf(filename: string): string {
  const index = filename.lastIndexOf(".");
  return index === -1 ? "" : filename.slice(index).toLowerCase();
}

export function kindForExtension(extension: string): DocumentKind | null {
  switch (extension) {
    case ".pdf":
      return "pdf";
    case ".txt":
      return "text";
    case ".md":
    case ".markdown":
      return "markdown";
    default:
      return null;
  }
}

/**
 * Strips directories and control characters from a user-supplied filename so it
 * is safe to render and to use as a storage key.
 */
export function safeFilename(filename: string): string {
  const base = filename.split(/[\\/]/).pop() ?? "document";
  const cleaned = base.replace(/[\u0000-\u001f\u007f]/g, "").trim();
  const trimmed = cleaned.slice(0, 180).replace(/^\.+/, "");
  return trimmed || "document";
}

export interface FileValidationSuccess {
  ok: true;
  kind: DocumentKind;
  filename: string;
}

export interface FileValidationFailure {
  ok: false;
  reason: string;
}

export type FileValidationResult = FileValidationSuccess | FileValidationFailure;

export interface ValidatableFile {
  name: string;
  size: number;
  type: string;
}

export function validateFile(file: ValidatableFile): FileValidationResult {
  const filename = safeFilename(file.name);
  const extension = extensionOf(filename);
  const kind = kindForExtension(extension);

  if (!kind) {
    return {
      ok: false,
      reason: `Unsupported file type "${extension || "unknown"}". Upload a PDF, TXT or Markdown file.`,
    };
  }

  if (file.size <= 0) {
    return { ok: false, reason: "That file is empty." };
  }

  if (file.size > LIMITS.maxFileBytes) {
    return {
      ok: false,
      reason: `File is ${formatBytes(file.size)}. The limit is ${formatBytes(LIMITS.maxFileBytes)}.`,
    };
  }

  const declaredType = file.type.toLowerCase();
  if (declaredType && !MIME_BY_KIND[kind].includes(declaredType)) {
    return {
      ok: false,
      reason: `File extension says ${extension} but the browser reports "${declaredType}". Refusing to process a mismatched file.`,
    };
  }

  return { ok: true, kind, filename };
}

/** PDFs must start with the %PDF- magic bytes; extensions alone are not trusted. */
export function hasPdfMagic(bytes: Uint8Array): boolean {
  return (
    bytes.length >= 5 &&
    bytes[0] === 0x25 &&
    bytes[1] === 0x50 &&
    bytes[2] === 0x44 &&
    bytes[3] === 0x46 &&
    bytes[4] === 0x2d
  );
}
