"use client";

import { ArrowLeft, FileWarning } from "lucide-react";
import Link from "next/link";
import { useParams } from "next/navigation";
import { useEffect, useState } from "react";
import { Card, Chip, EmptyState, PageHeader, SkeletonList } from "@/components/ui/Primitives";
import { getChunks, getDocument, getPages } from "@/lib/db/repo";
import type { StoredPage } from "@/lib/db/schema";
import { formatBytes } from "@/lib/documents/limits";
import type { Chunk, StoredDocument } from "@/lib/documents/types";
import { formatDuration, formatTimestamp } from "@/lib/utils/cn";

export default function DocumentDetailPage() {
  const params = useParams<{ id: string }>();
  const documentId = params.id;

  const [document, setDocument] = useState<StoredDocument | null>(null);
  const [pages, setPages] = useState<StoredPage[]>([]);
  const [chunks, setChunks] = useState<Chunk[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      const [doc, docPages, docChunks] = await Promise.all([
        getDocument(documentId),
        getPages(documentId),
        getChunks(documentId),
      ]);
      if (cancelled) return;
      setDocument(doc ?? null);
      setPages(docPages);
      setChunks(docChunks);
      setLoading(false);
    })();
    return () => {
      cancelled = true;
    };
  }, [documentId]);

  if (loading) return <SkeletonList rows={3} />;

  if (!document) {
    return (
      <EmptyState
        icon={<FileWarning className="h-8 w-8" aria-hidden />}
        title="Document not found"
        description="It may have been deleted from this browser's local storage."
        action={
          <Link href="/documents" className="btn-primary">
            Back to documents
          </Link>
        }
      />
    );
  }

  const embeddedCount = chunks.filter((chunk) => chunk.embedding !== null).length;

  return (
    <>
      <PageHeader
        title={document.filename}
        description={`${document.kind.toUpperCase()} · ${document.pageCount} pages · ${document.chunkCount} chunks · ${formatBytes(document.sizeBytes)}`}
        actions={
          <Link href="/documents" className="btn-ghost">
            <ArrowLeft className="h-4 w-4" aria-hidden />
            All documents
          </Link>
        }
      />

      <div className="flex flex-col gap-6">
        <Card>
          <h2 className="mb-3 text-sm font-semibold text-ink">Processing record</h2>
          <dl className="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-2">
            <Row label="Added">{formatTimestamp(document.createdAt)}</Row>
            <Row label="Status">
              <Chip tone={document.status === "embedded" ? "positive" : "caution"}>
                {document.status}
              </Chip>
            </Row>
            <Row label="Embedding model">{document.embeddingModel ?? "—"}</Row>
            <Row label="Vector dimensions">{document.embeddingDimensions ?? "—"}</Row>
            <Row label="Extraction time">{formatDuration(document.timings.extractionMs)}</Row>
            <Row label="Chunking time">{formatDuration(document.timings.chunkingMs)}</Row>
            <Row label="Embedding time">{formatDuration(document.timings.embeddingMs)}</Row>
            <Row label="Chunks with vectors">
              {embeddedCount} of {chunks.length}
            </Row>
          </dl>

          {document.warnings.length > 0 ? (
            <ul className="mt-4 flex flex-col gap-1.5">
              {document.warnings.map((warning) => (
                <li key={warning} className="text-xs text-caution">
                  {warning}
                </li>
              ))}
            </ul>
          ) : null}
        </Card>

        <section>
          <h2 className="mb-3 text-sm font-semibold text-ink">
            Extracted pages ({pages.length})
          </h2>
          <ul className="flex flex-col gap-2">
            {pages.map((page) => (
              <Card key={page.id} as="li">
                <details>
                  <summary className="cursor-pointer text-sm font-medium text-ink">
                    <span className="mr-2">
                      {page.section ? page.section : `Page ${page.page}`}
                    </span>
                    <span className="text-xs font-normal text-ink-faint">
                      {page.charCount.toLocaleString()} chars
                      {page.likelyScanned ? " · likely scanned, needs OCR" : ""}
                    </span>
                  </summary>
                  <p className="mt-3 max-h-64 overflow-y-auto whitespace-pre-wrap rounded-lg bg-surface-sunken p-3 text-xs leading-relaxed text-ink-muted">
                    {page.text || "(no text extracted from this page)"}
                  </p>
                </details>
              </Card>
            ))}
          </ul>
        </section>

        <section>
          <h2 className="mb-3 text-sm font-semibold text-ink">Chunks ({chunks.length})</h2>
          <ul className="flex flex-col gap-2">
            {chunks.map((chunk) => (
              <Card key={chunk.id} as="li">
                <details>
                  <summary className="cursor-pointer text-sm font-medium text-ink">
                    Chunk {chunk.chunkNumber}
                    <span className="ml-2 text-xs font-normal text-ink-faint">
                      {chunk.section ?? `page ${chunk.page}`} · {chunk.wordCount} words ·{" "}
                      {chunk.embedding ? `${chunk.embedding.length}d vector` : "no vector"}
                    </span>
                  </summary>
                  <p className="mt-3 max-h-56 overflow-y-auto whitespace-pre-wrap rounded-lg bg-surface-sunken p-3 text-xs leading-relaxed text-ink-muted">
                    {chunk.text}
                  </p>
                </details>
              </Card>
            ))}
          </ul>
        </section>
      </div>
    </>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-wrap gap-x-2">
      <dt className="text-ink-faint">{label}:</dt>
      <dd className="min-w-0 break-words text-ink-muted">{children}</dd>
    </div>
  );
}
