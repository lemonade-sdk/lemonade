"use client";

import { CalendarClock, List, Loader2, Sparkles, Timer } from "lucide-react";
import Link from "next/link";
import { useCallback, useEffect, useMemo, useState } from "react";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { useToast } from "@/components/providers/ToastProvider";
import { DocumentSelect } from "@/components/documents/DocumentSelect";
import {
  Callout,
  Card,
  Chip,
  EmptyState,
  PageHeader,
  ProgressBar,
  SkeletonList,
} from "@/components/ui/Primitives";
import { getChunks, getExtraction, saveExtraction } from "@/lib/db/repo";
import { DEADLINE_CATEGORIES, deadlineListSchema, type Deadline } from "@/lib/extraction/schemas";
import { extractDeadlines, sortDeadlines } from "@/lib/extraction/structured";
import type { Chunk } from "@/lib/documents/types";
import { useDocuments } from "@/lib/hooks/useDocuments";
import { isLemonadeError } from "@/lib/lemonade/errors";
import { cn, createId, formatDuration } from "@/lib/utils/cn";

const CATEGORY_TONE: Record<Deadline["category"], "brand" | "caution" | "danger" | "positive" | "neutral"> = {
  exam: "danger",
  assignment: "caution",
  application: "brand",
  event: "positive",
  fee: "caution",
  scholarship: "brand",
  interview: "danger",
  other: "neutral",
};

export default function DeadlinesPage() {
  const { client, chatModel, isReady, beginRequest } = useLemonade();
  const { documents, activeDocument, activeDocumentId, setActiveDocumentId, loading } =
    useDocuments();
  const toast = useToast();

  const [deadlines, setDeadlines] = useState<Deadline[]>([]);
  const [chunks, setChunks] = useState<Chunk[]>([]);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [view, setView] = useState<"list" | "timeline">("list");
  const [category, setCategory] = useState<Deadline["category"] | "all">("all");
  const [lastRun, setLastRun] = useState<{ durationMs: number; failedGroups: number } | null>(null);

  useEffect(() => {
    if (!activeDocumentId) return;
    let cancelled = false;
    void (async () => {
      const [record, docChunks] = await Promise.all([
        getExtraction(activeDocumentId, "deadlines"),
        getChunks(activeDocumentId),
      ]);
      if (cancelled) return;
      const parsed = deadlineListSchema.safeParse(record?.payload);
      setDeadlines(parsed.success ? parsed.data : []);
      setChunks(docChunks);
      setLastRun(null);
    })();
    return () => {
      cancelled = true;
    };
  }, [activeDocumentId]);

  const run = useCallback(async () => {
    if (!activeDocument || !chatModel) return;
    setBusy(true);
    setProgress({ done: 0, total: 1 });
    const endRequest = beginRequest();

    try {
      const result = await extractDeadlines(client, chatModel, chunks, {
        onProgress: (done, total) => setProgress({ done, total }),
      });
      const sorted = sortDeadlines(result.items);
      setDeadlines(sorted);
      setLastRun({ durationMs: result.durationMs, failedGroups: result.failedGroups });

      await saveExtraction({
        id: createId("dl"),
        documentId: activeDocument.id,
        kind: "deadlines",
        payload: sorted,
        model: chatModel,
        createdAt: Date.now(),
        durationMs: result.durationMs,
      });

      if (result.failedGroups > 0) {
        toast.info(
          `Found ${sorted.length} deadlines. ${result.failedGroups} of ${result.totalGroups} sections returned output that failed schema validation and were skipped.`,
        );
      } else {
        toast.success(`Found ${sorted.length} deadlines in ${formatDuration(result.durationMs)}.`);
      }
    } catch (error) {
      toast.error(isLemonadeError(error) ? error.displayMessage : (error as Error).message);
    } finally {
      endRequest();
      setBusy(false);
      setProgress(null);
    }
  }, [activeDocument, beginRequest, chatModel, chunks, client, toast]);

  const filtered = useMemo(
    () => (category === "all" ? deadlines : deadlines.filter((d) => d.category === category)),
    [category, deadlines],
  );

  if (loading) return <SkeletonList rows={3} />;

  if (documents.length === 0) {
    return (
      <EmptyState
        icon={<CalendarClock className="h-8 w-8" aria-hidden />}
        title="No documents yet"
        description="Upload an examination notice or circular, and this page will extract the dates it contains."
        action={
          <Link href="/documents" className="btn-primary">
            Go to documents
          </Link>
        }
      />
    );
  }

  return (
    <>
      <PageHeader
        title="Deadlines"
        description="Dates extracted from the selected document as validated JSON. A date is only normalised when the document states it unambiguously."
        actions={
          <button
            type="button"
            className="btn-primary"
            disabled={!isReady || busy || chunks.length === 0}
            onClick={() => void run()}
          >
            {busy ? (
              <Loader2 className="h-4 w-4 animate-spin" aria-hidden />
            ) : (
              <Sparkles className="h-4 w-4" aria-hidden />
            )}
            {deadlines.length > 0 ? "Re-extract" : "Extract deadlines"}
          </button>
        }
      />

      <div className="flex flex-col gap-4">
        <Card>
          <DocumentSelect
            documents={documents}
            value={activeDocumentId}
            onChange={(id) => void setActiveDocumentId(id)}
          />
        </Card>

        {!isReady ? (
          <Callout tone="warning" title="Finish setup first">
            Connect Lemonade and pick a chat model on the <Link href="/setup">Setup page</Link>.
          </Callout>
        ) : null}

        {busy && progress ? (
          <Card>
            <ProgressBar
              value={(progress.done / Math.max(1, progress.total)) * 100}
              label={`Scanning section ${progress.done} of ${progress.total}`}
            />
          </Card>
        ) : null}

        {lastRun ? (
          <p className="text-xs text-ink-faint">
            Last extraction took {formatDuration(lastRun.durationMs)}.
            {lastRun.failedGroups > 0
              ? ` ${lastRun.failedGroups} section(s) produced invalid JSON and were skipped rather than guessed at.`
              : ""}
          </p>
        ) : null}

        {deadlines.length === 0 ? (
          <EmptyState
            icon={<CalendarClock className="h-8 w-8" aria-hidden />}
            title="No deadlines extracted yet"
            description="Press “Extract deadlines” to have your local model scan the document for exam dates, fee deadlines, application windows and interviews."
          />
        ) : (
          <>
            <div className="flex flex-wrap items-center gap-2">
              <div className="flex rounded-lg border border-line p-0.5" role="tablist">
                <ViewTab active={view === "list"} onClick={() => setView("list")} icon={List}>
                  List
                </ViewTab>
                <ViewTab active={view === "timeline"} onClick={() => setView("timeline")} icon={Timer}>
                  Timeline
                </ViewTab>
              </div>

              <label htmlFor="category" className="sr-only">
                Filter by category
              </label>
              <select
                id="category"
                className="field w-auto"
                value={category}
                onChange={(event) => setCategory(event.target.value as Deadline["category"] | "all")}
              >
                <option value="all">All categories ({deadlines.length})</option>
                {DEADLINE_CATEGORIES.map((value) => {
                  const count = deadlines.filter((d) => d.category === value).length;
                  return count === 0 ? null : (
                    <option key={value} value={value}>
                      {value} ({count})
                    </option>
                  );
                })}
              </select>
            </div>

            {view === "list" ? (
              <ul className="flex flex-col gap-3">
                {filtered.map((deadline, index) => (
                  <DeadlineCard key={`${deadline.title}-${index}`} deadline={deadline} />
                ))}
              </ul>
            ) : (
              <Timeline deadlines={filtered} />
            )}
          </>
        )}
      </div>
    </>
  );
}

function ViewTab({
  active,
  onClick,
  icon: Icon,
  children,
}: {
  active: boolean;
  onClick: () => void;
  icon: typeof List;
  children: React.ReactNode;
}) {
  return (
    <button
      type="button"
      role="tab"
      aria-selected={active}
      onClick={onClick}
      className={cn(
        "flex items-center gap-1.5 rounded-md px-3 py-1.5 text-sm transition-colors",
        active ? "bg-brand-soft font-medium text-brand" : "text-ink-muted hover:text-ink",
      )}
    >
      <Icon className="h-4 w-4" aria-hidden />
      {children}
    </button>
  );
}

function DeadlineCard({ deadline }: { deadline: Deadline }) {
  return (
    <Card as="li">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div className="min-w-0 flex-1">
          <h3 className="text-sm font-semibold text-ink">{deadline.title}</h3>
          <p className="mt-1 text-sm text-ink-muted">
            {deadline.dateText}
            {deadline.normalizedDate ? (
              <span className="ml-2 font-mono text-xs text-ink-faint">
                {deadline.normalizedDate}
              </span>
            ) : (
              <span className="ml-2 text-xs text-caution">not normalised — ambiguous in source</span>
            )}
          </p>
          <p className="mt-2 border-l-2 border-line pl-3 text-xs italic text-ink-muted">
            “{deadline.evidence}”
          </p>
        </div>
        <div className="flex shrink-0 flex-col items-end gap-1.5">
          <Chip tone={CATEGORY_TONE[deadline.category]}>{deadline.category}</Chip>
          <Chip
            tone={
              deadline.confidence === "high"
                ? "positive"
                : deadline.confidence === "medium"
                  ? "caution"
                  : "neutral"
            }
          >
            {deadline.confidence} confidence
          </Chip>
          {deadline.page ? <span className="text-xs text-ink-faint">page {deadline.page}</span> : null}
        </div>
      </div>
    </Card>
  );
}

function Timeline({ deadlines }: { deadlines: Deadline[] }) {
  const dated = deadlines.filter((d) => d.normalizedDate !== null);
  const undated = deadlines.filter((d) => d.normalizedDate === null);

  return (
    <div className="flex flex-col gap-6">
      {dated.length > 0 ? (
        <ol className="relative ml-3 border-l-2 border-line pl-6">
          {dated.map((deadline, index) => (
            <li key={`${deadline.title}-${index}`} className="relative pb-6 last:pb-0">
              <span
                className="absolute -left-[1.9rem] top-1 h-3 w-3 rounded-full border-2 border-surface bg-brand"
                aria-hidden
              />
              <p className="font-mono text-xs text-brand">{deadline.normalizedDate}</p>
              <h3 className="mt-0.5 text-sm font-semibold text-ink">{deadline.title}</h3>
              <p className="text-xs text-ink-muted">
                {deadline.dateText} · {deadline.category}
                {deadline.page ? ` · page ${deadline.page}` : ""}
              </p>
            </li>
          ))}
        </ol>
      ) : (
        <p className="text-sm text-ink-muted">
          No deadline in this document had an unambiguous calendar date, so the timeline is empty.
        </p>
      )}

      {undated.length > 0 ? (
        <Card>
          <h3 className="text-sm font-semibold text-ink">
            Undated entries ({undated.length})
          </h3>
          <p className="mt-1 text-xs text-ink-muted">
            These were mentioned without a date the model could resolve unambiguously, so they are
            listed rather than placed on the timeline.
          </p>
          <ul className="mt-2 flex flex-col gap-1.5">
            {undated.map((deadline, index) => (
              <li key={`${deadline.title}-${index}`} className="text-sm text-ink-muted">
                <span className="text-ink">{deadline.title}</span> — {deadline.dateText}
              </li>
            ))}
          </ul>
        </Card>
      ) : null}
    </div>
  );
}
