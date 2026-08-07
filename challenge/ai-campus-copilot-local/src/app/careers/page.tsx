"use client";

import { Briefcase, ExternalLink, Loader2, Sparkles } from "lucide-react";
import Link from "next/link";
import { useCallback, useEffect, useState } from "react";
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
import {
  opportunityListSchema,
  safeHttpUrl,
  type Opportunity,
} from "@/lib/extraction/schemas";
import { extractOpportunities } from "@/lib/extraction/structured";
import type { Chunk } from "@/lib/documents/types";
import { useDocuments } from "@/lib/hooks/useDocuments";
import { isLemonadeError } from "@/lib/lemonade/errors";
import { createId, formatDuration } from "@/lib/utils/cn";

export default function CareersPage() {
  const { client, chatModel, isReady, beginRequest } = useLemonade();
  const { documents, activeDocument, activeDocumentId, setActiveDocumentId, loading } =
    useDocuments();
  const toast = useToast();

  const [opportunities, setOpportunities] = useState<Opportunity[]>([]);
  const [chunks, setChunks] = useState<Chunk[]>([]);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [lastRun, setLastRun] = useState<{ durationMs: number; failedGroups: number } | null>(null);

  useEffect(() => {
    if (!activeDocumentId) return;
    let cancelled = false;
    void (async () => {
      const [record, docChunks] = await Promise.all([
        getExtraction(activeDocumentId, "careers"),
        getChunks(activeDocumentId),
      ]);
      if (cancelled) return;
      const parsed = opportunityListSchema.safeParse(record?.payload);
      setOpportunities(parsed.success ? parsed.data : []);
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
      const result = await extractOpportunities(client, chatModel, chunks, {
        onProgress: (done, total) => setProgress({ done, total }),
      });
      setOpportunities(result.items);
      setLastRun({ durationMs: result.durationMs, failedGroups: result.failedGroups });

      await saveExtraction({
        id: createId("opp"),
        documentId: activeDocument.id,
        kind: "careers",
        payload: result.items,
        model: chatModel,
        createdAt: Date.now(),
        durationMs: result.durationMs,
      });

      toast.success(
        `Found ${result.items.length} opportunit${result.items.length === 1 ? "y" : "ies"} in ${formatDuration(result.durationMs)}.`,
      );
    } catch (error) {
      toast.error(isLemonadeError(error) ? error.displayMessage : (error as Error).message);
    } finally {
      endRequest();
      setBusy(false);
      setProgress(null);
    }
  }, [activeDocument, beginRequest, chatModel, chunks, client, toast]);

  if (loading) return <SkeletonList rows={3} />;

  if (documents.length === 0) {
    return (
      <EmptyState
        icon={<Briefcase className="h-8 w-8" aria-hidden />}
        title="No documents yet"
        description="Upload a placement announcement or internship circular to pull out roles, eligibility and deadlines."
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
        title="Internships & placements"
        description="Opportunities extracted from the selected document as schema-validated JSON. Fields the document does not state are left blank rather than guessed."
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
            {opportunities.length > 0 ? "Re-extract" : "Extract opportunities"}
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
              ? ` ${lastRun.failedGroups} section(s) produced invalid JSON and were skipped.`
              : ""}
          </p>
        ) : null}

        {opportunities.length === 0 ? (
          <EmptyState
            icon={<Briefcase className="h-8 w-8" aria-hidden />}
            title="No opportunities extracted yet"
            description="Press “Extract opportunities” to scan this document for internships, placements and job roles."
          />
        ) : (
          <ul className="grid gap-4 md:grid-cols-2">
            {opportunities.map((opportunity, index) => (
              <OpportunityCard key={`${opportunity.organisation}-${index}`} opportunity={opportunity} />
            ))}
          </ul>
        )}
      </div>
    </>
  );
}

function OpportunityCard({ opportunity }: { opportunity: Opportunity }) {
  const link = safeHttpUrl(opportunity.applicationLink);

  return (
    <Card as="li" className="flex flex-col">
      <div className="flex items-start justify-between gap-2">
        <div className="min-w-0">
          <h3 className="truncate text-sm font-semibold text-ink">{opportunity.role}</h3>
          <p className="truncate text-sm text-ink-muted">{opportunity.organisation}</p>
        </div>
        <Chip tone="brand">{opportunity.employmentType}</Chip>
      </div>

      <dl className="mt-3 flex flex-col gap-1.5 text-xs">
        <Field label="Eligibility">{opportunity.eligibility}</Field>
        <Field label="Location">{opportunity.location}</Field>
        <Field label="Stipend / salary">{opportunity.compensation}</Field>
        <Field label="Apply by">{opportunity.applicationDeadline}</Field>
      </dl>

      {opportunity.requiredSkills.length > 0 ? (
        <div className="mt-3 flex flex-wrap gap-1.5">
          {opportunity.requiredSkills.map((skill) => (
            <Chip key={skill}>{skill}</Chip>
          ))}
        </div>
      ) : null}

      <p className="mt-3 flex-1 border-l-2 border-line pl-3 text-xs italic text-ink-muted">
        “{opportunity.evidence}”
      </p>

      <div className="mt-3 flex flex-wrap items-center gap-2 text-xs text-ink-faint">
        {opportunity.page ? <span>page {opportunity.page}</span> : null}
        <Chip
          tone={
            opportunity.confidence === "high"
              ? "positive"
              : opportunity.confidence === "medium"
                ? "caution"
                : "neutral"
          }
        >
          {opportunity.confidence}
        </Chip>
        {link ? (
          <a
            href={link}
            target="_blank"
            rel="noreferrer noopener"
            className="ml-auto inline-flex items-center gap-1 text-brand hover:underline"
          >
            Apply <ExternalLink className="h-3 w-3" aria-hidden />
          </a>
        ) : opportunity.applicationLink ? (
          <span className="ml-auto" title="Not a valid http(s) URL, shown as text">
            {opportunity.applicationLink}
          </span>
        ) : null}
      </div>
    </Card>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex gap-2">
      <dt className="shrink-0 text-ink-faint">{label}:</dt>
      <dd className="min-w-0 break-words text-ink-muted">{children ?? "not stated"}</dd>
    </div>
  );
}
