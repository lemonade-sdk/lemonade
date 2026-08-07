"use client";

import { FileUp, Upload } from "lucide-react";
import { useCallback, useId, useRef, useState, type DragEvent } from "react";
import { ACCEPTED_EXTENSIONS, formatBytes, LIMITS } from "@/lib/documents/limits";
import { cn } from "@/lib/utils/cn";

export function UploadZone({
  onFile,
  disabled,
  disabledReason,
}: {
  onFile: (file: File) => void;
  disabled?: boolean;
  disabledReason?: string;
}) {
  const inputId = useId();
  const inputRef = useRef<HTMLInputElement>(null);
  const [dragging, setDragging] = useState(false);

  const handleDrop = useCallback(
    (event: DragEvent<HTMLDivElement>) => {
      event.preventDefault();
      setDragging(false);
      if (disabled) return;
      const file = event.dataTransfer.files?.[0];
      if (file) onFile(file);
    },
    [disabled, onFile],
  );

  return (
    <div>
      <div
        onDragOver={(event) => {
          event.preventDefault();
          if (!disabled) setDragging(true);
        }}
        onDragLeave={() => setDragging(false)}
        onDrop={handleDrop}
        className={cn(
          "flex flex-col items-center gap-3 rounded-xl border-2 border-dashed px-6 py-10 text-center transition-colors",
          dragging ? "border-brand bg-brand-soft" : "border-line bg-surface-raised",
          disabled && "opacity-60",
        )}
      >
        <FileUp className="h-8 w-8 text-ink-faint" aria-hidden />
        <div>
          <p className="text-sm font-medium text-ink">
            Drag a college notice here, or choose a file
          </p>
          <p className="mt-1 text-xs text-ink-muted">
            {ACCEPTED_EXTENSIONS.join(", ")} · up to {formatBytes(LIMITS.maxFileBytes)} ·{" "}
            {LIMITS.maxPages} pages max
          </p>
        </div>

        <label htmlFor={inputId} className="sr-only">
          Choose a document to upload
        </label>
        <input
          ref={inputRef}
          id={inputId}
          type="file"
          className="sr-only"
          accept={ACCEPTED_EXTENSIONS.join(",")}
          disabled={disabled}
          onChange={(event) => {
            const file = event.target.files?.[0];
            if (file) onFile(file);
            // Allow re-selecting the same filename after a delete.
            event.target.value = "";
          }}
        />
        <button
          type="button"
          className="btn-primary"
          disabled={disabled}
          onClick={() => inputRef.current?.click()}
        >
          <Upload className="h-4 w-4" aria-hidden />
          Choose file
        </button>

        <p className="max-w-md text-xs text-ink-faint">
          The file is read in your browser. Only extracted text chunks are sent to your local
          Lemonade Server for embedding — the file itself never leaves this machine.
        </p>
      </div>

      {disabled && disabledReason ? (
        <p className="mt-2 text-sm text-caution" role="status">
          {disabledReason}
        </p>
      ) : null}
    </div>
  );
}
