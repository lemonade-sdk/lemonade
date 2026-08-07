"use client";

import { Quote } from "lucide-react";
import type { ChatTurn } from "@/lib/db/schema";

export function SourcePanel({ sources }: { sources: ChatTurn["sources"] }) {
  if (sources.length === 0) {
    return (
      <p className="mt-2 text-xs text-caution">
        No passage scored above the relevance threshold, so the answer was generated with no
        supporting context.
      </p>
    );
  }

  return (
    <details className="mt-3 rounded-lg border border-line bg-surface-sunken">
      <summary className="cursor-pointer px-3 py-2 text-xs font-medium text-ink-muted">
        <Quote className="mr-1.5 inline h-3.5 w-3.5" aria-hidden />
        {sources.length} retrieved source{sources.length === 1 ? "" : "s"}
      </summary>
      <ul className="flex flex-col gap-2 px-3 pb-3">
        {sources.map((source, index) => (
          <li key={source.chunkId} className="rounded-lg border border-line bg-surface-raised p-2.5">
            <p className="flex flex-wrap items-center gap-2 text-xs font-medium text-ink">
              <span className="chip border-transparent bg-brand-soft text-brand">[{index + 1}]</span>
              <span>{source.section ? source.section : `Page ${source.page}`}</span>
              <span className="text-ink-faint">chunk {source.chunkNumber}</span>
              <span className="ml-auto tabular-nums text-ink-faint">
                similarity {source.score.toFixed(3)}
              </span>
            </p>
            <p className="mt-1.5 text-xs leading-relaxed text-ink-muted">{source.excerpt}</p>
          </li>
        ))}
      </ul>
    </details>
  );
}
