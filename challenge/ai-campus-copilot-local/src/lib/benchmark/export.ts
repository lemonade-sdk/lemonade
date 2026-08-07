import type { BenchmarkRun } from "@/lib/db/schema";

const CSV_COLUMNS = [
  "run_id",
  "created_at",
  "document",
  "chat_model",
  "embedding_model",
  "server_version",
  "os_version",
  "processor",
  "question",
  "answer_chars",
  "question_embedding_ms",
  "retrieval_ms",
  "time_to_first_token_ms",
  "generation_ms",
  "total_ms",
  "tokens_per_second",
  "output_tokens",
  "retrieved_chunks",
  "top_score",
  "error",
] as const;

export function benchmarkToJson(runs: BenchmarkRun[]): string {
  return JSON.stringify(
    {
      exportedAt: new Date().toISOString(),
      note: "All values are measured. Empty fields were not reported by Lemonade Server and are not estimated.",
      runs,
    },
    null,
    2,
  );
}

export function benchmarkToCsv(runs: BenchmarkRun[]): string {
  const rows: string[] = [CSV_COLUMNS.join(",")];

  for (const run of runs) {
    for (const result of run.results) {
      rows.push(
        [
          run.id,
          new Date(run.createdAt).toISOString(),
          run.documentName,
          run.chatModel,
          run.embeddingModel,
          run.serverVersion ?? "",
          run.osVersion ?? "",
          run.processor ?? "",
          result.question,
          result.answerChars,
          numeric(result.questionEmbeddingMs),
          numeric(result.retrievalMs),
          numeric(result.timeToFirstTokenMs),
          numeric(result.generationMs),
          numeric(result.totalMs),
          numeric(result.tokensPerSecond),
          numeric(result.outputTokens),
          result.retrievedChunks,
          numeric(result.topScore),
          result.error ?? "",
        ]
          .map(csvCell)
          .join(","),
      );
    }
  }

  return rows.join("\n");
}

function numeric(value: number | null): string {
  return value === null ? "" : String(Math.round(value * 1000) / 1000);
}

/**
 * Quotes every cell and neutralises leading =, +, - and @ so a spreadsheet
 * never interprets exported text as a formula.
 */
function csvCell(value: string | number): string {
  const text = String(value);
  const guarded = /^[=+\-@\t\r]/.test(text) ? `'${text}` : text;
  return `"${guarded.replace(/"/g, '""')}"`;
}

export function downloadFile(content: string, filename: string, mimeType: string): void {
  const blob = new Blob([content], { type: mimeType });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  URL.revokeObjectURL(url);
}
