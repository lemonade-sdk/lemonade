"use client";

import { useId } from "react";
import type { StoredDocument } from "@/lib/documents/types";

export function DocumentSelect({
  documents,
  value,
  onChange,
  label = "Selected document",
}: {
  documents: StoredDocument[];
  value: string | null;
  onChange: (id: string) => void;
  label?: string;
}) {
  const id = useId();

  return (
    <div className="min-w-0">
      <label htmlFor={id} className="label">
        {label}
      </label>
      <select
        id={id}
        className="field"
        value={value ?? ""}
        onChange={(event) => onChange(event.target.value)}
      >
        {documents.map((document) => (
          <option key={document.id} value={document.id}>
            {document.filename} — {document.pageCount} pages, {document.chunkCount} chunks
          </option>
        ))}
      </select>
    </div>
  );
}
