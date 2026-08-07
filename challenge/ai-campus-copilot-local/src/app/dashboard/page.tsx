"use client";

import { ArrowRight, FileText } from "lucide-react";
import Link from "next/link";
import { useEffect, useState } from "react";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { Card, Chip, EmptyState, PageHeader, SkeletonList, Stat } from "@/components/ui/Primitives";
import {
  countQuestionsAsked,
  listBenchmarks,
  listDocuments,
  listExtractions,
} from "@/lib/db/repo";
import type { ChatTurn } from "@/lib/db/schema";
import { db } from "@/lib/db/schema";
import { deadlineListSchema, opportunityListSchema } from "@/lib/extraction/schemas";
import type { StoredDocument } from "@/lib/documents/types";
import { formatDuration, formatTimestamp } from "@/lib/utils/cn";

interface DashboardData {
  documents: StoredDocument[];
  totalChunks: number;
  questionsAsked: number;
  deadlines: number;
  opportunities: number;
  lastResponseMs: number | null;
  averageResponseMs: number | null;
  recentActivity: { id: string; label: string; at: number }[];
}

export default function DashboardPage() {
  const { status, chatModel, embeddingModel, health } = useLemonade();
  const [data, setData] = useState<DashboardData | null>(null);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      const [documents, questionsAsked, deadlineRecords, careerRecords, benchmarks, turns] =
        await Promise.all([
          listDocuments(),
          countQuestionsAsked(),
          listExtractions("deadlines"),
          listExtractions("careers"),
          listBenchmarks(),
          db().chats.toArray(),
        ]);
      if (cancelled) return;

      const assistantTurns = turns
        .filter((turn): turn is ChatTurn => turn.role === "assistant" && turn.metrics !== null)
        .sort((a, b) => b.createdAt - a.createdAt);

      const durations = assistantTurns
        .map((turn) => turn.metrics?.totalMs)
        .filter((value): value is number => typeof value === "number");

      const deadlines = deadlineRecords.reduce((sum, record) => {
        const parsed = deadlineListSchema.safeParse(record.payload);
        return sum + (parsed.success ? parsed.data.length : 0);
      }, 0);

      const opportunities = careerRecords.reduce((sum, record) => {
        const parsed = opportunityListSchema.safeParse(record.payload);
        return sum + (parsed.success ? parsed.data.length : 0);
      }, 0);

      const recentActivity = [
        ...documents.map((doc) => ({
          id: `doc-${doc.id}`,
          label: `Processed ${doc.filename} (${doc.chunkCount} chunks)`,
          at: doc.createdAt,
        })),
        ...assistantTurns.slice(0, 5).map((turn) => ({
          id: `turn-${turn.id}`,
          label: `Answered a question in ${formatDuration(turn.metrics?.totalMs ?? null)}`,
          at: turn.createdAt,
        })),
        ...benchmarks.map((run) => ({
          id: `bench-${run.id}`,
          label: `Benchmarked ${run.documentName} with ${run.chatModel}`,
          at: run.createdAt,
        })),
      ]
        .sort((a, b) => b.at - a.at)
        .slice(0, 8);

      setData({
        documents,
        totalChunks: documents.reduce((sum, doc) => sum + doc.chunkCount, 0),
        questionsAsked,
        deadlines,
        opportunities,
        lastResponseMs: durations[0] ?? null,
        averageResponseMs:
          durations.length > 0
            ? durations.reduce((sum, value) => sum + value, 0) / durations.length
            : null,
        recentActivity,
      });
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  if (!data) return <SkeletonList rows={4} />;

  return (
    <>
      <PageHeader
        title="Dashboard"
        description="Everything on this page is computed from your own local storage and your own Lemonade Server."
      />

      <div className="flex flex-col gap-6">
        <Card>
          <div className="flex flex-wrap items-center gap-3">
            <Chip tone={status === "connected" ? "positive" : "danger"}>
              Lemonade: {status.replace(/_/g, " ")}
            </Chip>
            {health?.version ? <Chip>v{health.version}</Chip> : null}
            <Chip tone={chatModel ? "brand" : "caution"}>
              Chat: {chatModel ?? "not selected"}
            </Chip>
            <Chip tone={embeddingModel ? "brand" : "caution"}>
              Embedding: {embeddingModel ?? "not selected"}
            </Chip>
            <Link href="/setup" className="ml-auto text-sm text-brand hover:underline">
              Manage connection <ArrowRight className="inline h-3.5 w-3.5" aria-hidden />
            </Link>
          </div>
        </Card>

        <dl className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          <Stat label="Documents" value={data.documents.length} />
          <Stat label="Chunks stored" value={data.totalChunks.toLocaleString()} />
          <Stat label="Questions asked" value={data.questionsAsked} />
          <Stat label="Deadlines detected" value={data.deadlines} />
          <Stat label="Opportunities detected" value={data.opportunities} />
          <Stat label="Last response" value={formatDuration(data.lastResponseMs)} />
          <Stat
            label="Average response"
            value={formatDuration(data.averageResponseMs)}
            hint="Across all answered questions"
          />
          <Stat
            label="Embedded documents"
            value={data.documents.filter((doc) => doc.status === "embedded").length}
          />
        </dl>

        <section>
          <h2 className="mb-3 text-sm font-semibold text-ink">Recent activity</h2>
          {data.recentActivity.length === 0 ? (
            <EmptyState
              icon={<FileText className="h-8 w-8" aria-hidden />}
              title="Nothing here yet"
              description="Upload a document and ask a question — your activity log builds up locally as you work."
              action={
                <Link href="/documents" className="btn-primary">
                  Upload a document
                </Link>
              }
            />
          ) : (
            <ul className="flex flex-col gap-2">
              {data.recentActivity.map((entry) => (
                <Card key={entry.id} as="li">
                  <div className="flex flex-wrap items-center justify-between gap-2">
                    <p className="text-sm text-ink">{entry.label}</p>
                    <p className="text-xs text-ink-faint">{formatTimestamp(entry.at)}</p>
                  </div>
                </Card>
              ))}
            </ul>
          )}
        </section>
      </div>
    </>
  );
}
