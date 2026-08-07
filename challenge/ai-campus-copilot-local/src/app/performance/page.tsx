"use client";

import { Download, Gauge, Loader2, Play } from "lucide-react";
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
  Stat,
} from "@/components/ui/Primitives";
import { getChunks, listBenchmarks, saveBenchmark } from "@/lib/db/repo";
import type { BenchmarkRun } from "@/lib/db/schema";
import type { Chunk } from "@/lib/documents/types";
import { useDocuments } from "@/lib/hooks/useDocuments";
import { isLemonadeError } from "@/lib/lemonade/errors";
import { averageOf, BENCHMARK_QUESTIONS, runBenchmark } from "@/lib/benchmark/runner";
import { benchmarkToCsv, benchmarkToJson, downloadFile } from "@/lib/benchmark/export";
import { formatDuration, formatNumber, formatTimestamp } from "@/lib/utils/cn";

export default function PerformancePage() {
  const { client, chatModel, embeddingModel, isReady, systemInfo, health, status, beginRequest } =
    useLemonade();
  const { documents, activeDocument, activeDocumentId, setActiveDocumentId, loading } =
    useDocuments();
  const toast = useToast();

  const [runs, setRuns] = useState<BenchmarkRun[]>([]);
  const [chunks, setChunks] = useState<Chunk[]>([]);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<{ done: number; total: number; question: string } | null>(
    null,
  );

  useEffect(() => {
    void listBenchmarks().then(setRuns);
  }, []);

  useEffect(() => {
    if (!activeDocumentId) return;
    void getChunks(activeDocumentId).then(setChunks);
  }, [activeDocumentId]);

  const run = useCallback(async () => {
    if (!activeDocument || !chatModel || !embeddingModel) return;
    setBusy(true);
    const endRequest = beginRequest();

    try {
      const result = await runBenchmark({
        client,
        chatModel,
        embeddingModel,
        document: activeDocument,
        chunks,
        onProgress: (done, total, question) => setProgress({ done, total, question }),
      });
      await saveBenchmark(result);
      setRuns(await listBenchmarks());

      const failures = result.results.filter((r) => r.error !== null).length;
      if (failures > 0) {
        toast.info(`Benchmark finished with ${failures} of ${result.results.length} questions failing.`);
      } else {
        toast.success("Benchmark complete. All values below are measured, not estimated.");
      }
    } catch (error) {
      toast.error(isLemonadeError(error) ? error.displayMessage : (error as Error).message);
    } finally {
      endRequest();
      setBusy(false);
      setProgress(null);
    }
  }, [activeDocument, beginRequest, chatModel, chunks, client, embeddingModel, toast]);

  if (loading) return <SkeletonList rows={3} />;

  const latest = runs[0];

  return (
    <>
      <PageHeader
        title="Performance"
        description="Real measurements from this machine only. Where Lemonade does not report a value, the field stays blank — nothing here is estimated or synthetic."
        actions={
          runs.length > 0 ? (
            <>
              <button
                type="button"
                className="btn-ghost"
                onClick={() =>
                  downloadFile(
                    benchmarkToJson(runs),
                    `campus-copilot-benchmarks-${Date.now()}.json`,
                    "application/json",
                  )
                }
              >
                <Download className="h-4 w-4" aria-hidden />
                JSON
              </button>
              <button
                type="button"
                className="btn-ghost"
                onClick={() =>
                  downloadFile(
                    benchmarkToCsv(runs),
                    `campus-copilot-benchmarks-${Date.now()}.csv`,
                    "text/csv",
                  )
                }
              >
                <Download className="h-4 w-4" aria-hidden />
                CSV
              </button>
            </>
          ) : null
        }
      />

      <div className="flex flex-col gap-6">
        <section>
          <h2 className="mb-3 text-sm font-semibold text-ink">Environment</h2>
          <dl className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
            <Stat label="Lemonade status" value={status === "connected" ? "connected" : status.replace(/_/g, " ")} />
            <Stat label="Server version" value={health?.version ?? "—"} />
            <Stat label="Chat model" value={<span className="text-sm">{chatModel ?? "—"}</span>} />
            <Stat
              label="Embedding model"
              value={<span className="text-sm">{embeddingModel ?? "—"}</span>}
            />
          </dl>
          <dl className="mt-3 grid gap-3 sm:grid-cols-2">
            <Stat
              label="Operating system"
              value={<span className="text-sm">{systemInfo?.["OS Version"] ?? "not reported"}</span>}
              hint="From GET /api/v1/system-info"
            />
            <Stat
              label="Processor"
              value={<span className="text-sm">{systemInfo?.["Processor"] ?? "not reported"}</span>}
              hint={
                systemInfo?.["Physical Memory"]
                  ? `Physical memory: ${systemInfo["Physical Memory"]}`
                  : "From GET /api/v1/system-info"
              }
            />
          </dl>
        </section>

        {activeDocument ? (
          <section>
            <h2 className="mb-3 text-sm font-semibold text-ink">
              Processing measurements — {activeDocument.filename}
            </h2>
            <dl className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
              <Stat label="Extraction" value={formatDuration(activeDocument.timings.extractionMs)} />
              <Stat label="Chunking" value={formatDuration(activeDocument.timings.chunkingMs)} />
              <Stat label="Embedding" value={formatDuration(activeDocument.timings.embeddingMs)} />
              <Stat
                label="Chunks"
                value={activeDocument.chunkCount}
                hint={
                  activeDocument.embeddingDimensions
                    ? `${activeDocument.embeddingDimensions}-dimensional vectors`
                    : undefined
                }
              />
            </dl>
          </section>
        ) : null}

        <Card>
          <h2 className="mb-3 text-sm font-semibold text-ink">Benchmark runner</h2>
          <p className="mb-3 text-sm text-ink-muted">
            Runs {BENCHMARK_QUESTIONS.length} fixed questions through the full pipeline — question
            embedding, cosine retrieval and grounded generation — against the selected document.
          </p>

          {documents.length === 0 ? (
            <Callout tone="warning" title="Upload a document first">
              The <code className="font-mono text-xs">samples/</code> folder of this project ships
              three fictional notices you can use. <Link href="/documents">Go to documents</Link>.
            </Callout>
          ) : (
            <>
              <DocumentSelect
                documents={documents}
                value={activeDocumentId}
                onChange={(id) => void setActiveDocumentId(id)}
              />
              <button
                type="button"
                className="btn-primary mt-3"
                disabled={!isReady || busy || chunks.length === 0}
                onClick={() => void run()}
              >
                {busy ? (
                  <Loader2 className="h-4 w-4 animate-spin" aria-hidden />
                ) : (
                  <Play className="h-4 w-4" aria-hidden />
                )}
                Run benchmark
              </button>
            </>
          )}

          {busy && progress ? (
            <div className="mt-4">
              <ProgressBar
                value={(progress.done / progress.total) * 100}
                label={`Question ${Math.min(progress.done + 1, progress.total)} of ${progress.total}`}
              />
              <p className="mt-1.5 text-xs text-ink-muted">{progress.question}</p>
            </div>
          ) : null}
        </Card>

        {latest ? (
          <section>
            <h2 className="mb-3 text-sm font-semibold text-ink">
              Latest run — {formatTimestamp(latest.createdAt)}
            </h2>
            <dl className="mb-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
              <Stat
                label="Avg total"
                value={formatDuration(averageOf(latest.results, "totalMs"))}
              />
              <Stat
                label="Avg time to first token"
                value={formatDuration(averageOf(latest.results, "timeToFirstTokenMs"))}
              />
              <Stat
                label="Avg tokens/sec"
                value={formatNumber(averageOf(latest.results, "tokensPerSecond"), 1)}
                hint="Reported by GET /api/v1/stats"
              />
              <Stat
                label="Avg retrieval"
                value={formatDuration(averageOf(latest.results, "retrievalMs"))}
              />
            </dl>

            <div className="overflow-x-auto">
              <table className="w-full min-w-[52rem] border-collapse text-sm">
                <caption className="sr-only">Per-question benchmark measurements</caption>
                <thead>
                  <tr className="border-b border-line text-left text-xs uppercase tracking-wide text-ink-faint">
                    <th scope="col" className="py-2 pr-3 font-medium">Question</th>
                    <th scope="col" className="py-2 pr-3 font-medium">Embed</th>
                    <th scope="col" className="py-2 pr-3 font-medium">Retrieve</th>
                    <th scope="col" className="py-2 pr-3 font-medium">TTFT</th>
                    <th scope="col" className="py-2 pr-3 font-medium">Generate</th>
                    <th scope="col" className="py-2 pr-3 font-medium">Total</th>
                    <th scope="col" className="py-2 pr-3 font-medium">tok/s</th>
                    <th scope="col" className="py-2 font-medium">Chunks</th>
                  </tr>
                </thead>
                <tbody>
                  {latest.results.map((result) => (
                    <tr key={result.question} className="border-b border-line align-top">
                      <td className="max-w-xs py-2 pr-3 text-ink-muted">
                        {result.question}
                        {result.error ? (
                          <span className="mt-1 block text-xs text-danger">{result.error}</span>
                        ) : null}
                      </td>
                      <td className="py-2 pr-3 tabular-nums">{formatDuration(result.questionEmbeddingMs)}</td>
                      <td className="py-2 pr-3 tabular-nums">{formatDuration(result.retrievalMs)}</td>
                      <td className="py-2 pr-3 tabular-nums">{formatDuration(result.timeToFirstTokenMs)}</td>
                      <td className="py-2 pr-3 tabular-nums">{formatDuration(result.generationMs)}</td>
                      <td className="py-2 pr-3 tabular-nums">{formatDuration(result.totalMs)}</td>
                      <td className="py-2 pr-3 tabular-nums">{formatNumber(result.tokensPerSecond, 1)}</td>
                      <td className="py-2 tabular-nums">{result.retrievedChunks}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            <p className="mt-3 flex flex-wrap gap-2 text-xs text-ink-faint">
              <Chip>{latest.chatModel}</Chip>
              <Chip>{latest.embeddingModel}</Chip>
              {latest.osVersion ? <Chip>{latest.osVersion}</Chip> : null}
              {latest.processor ? <Chip>{latest.processor}</Chip> : null}
            </p>
          </section>
        ) : (
          <EmptyState
            icon={<Gauge className="h-8 w-8" aria-hidden />}
            title="No benchmark runs yet"
            description="Run the benchmark above to record genuine timings from your own hardware."
          />
        )}

        {runs.length > 1 ? (
          <section>
            <h2 className="mb-3 text-sm font-semibold text-ink">
              Earlier runs ({runs.length - 1})
            </h2>
            <ul className="flex flex-col gap-2">
              {runs.slice(1).map((previous) => (
                <Card key={previous.id} as="li">
                  <p className="text-sm text-ink">
                    {formatTimestamp(previous.createdAt)} — {previous.documentName}
                  </p>
                  <p className="mt-1 text-xs text-ink-muted">
                    {previous.chatModel} · avg total{" "}
                    {formatDuration(averageOf(previous.results, "totalMs"))} · avg tok/s{" "}
                    {formatNumber(averageOf(previous.results, "tokensPerSecond"), 1)}
                  </p>
                </Card>
              ))}
            </ul>
          </section>
        ) : null}
      </div>
    </>
  );
}
