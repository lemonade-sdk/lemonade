"use client";

import { Trash2 } from "lucide-react";
import { useCallback } from "react";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { useToast } from "@/components/providers/ToastProvider";
import { Callout, Card, PageHeader } from "@/components/ui/Primitives";
import { deleteAllData } from "@/lib/db/repo";

const POINTS = [
  {
    title: "Documents are processed locally",
    body: "PDF, TXT and Markdown files are read and parsed by your browser. The file itself is never uploaded anywhere — not to this app's server, not to any third party.",
  },
  {
    title: "AI requests go to your own Lemonade Server",
    body: "Extracted text chunks and your questions are sent to the Lemonade Server running on your machine at the address in LEMONADE_SERVER_URL. Requests travel from your browser to this app's local Next.js process, which forwards them to Lemonade on the same machine.",
  },
  {
    title: "No cloud AI provider is required",
    body: "There is no OpenAI, Anthropic, Google or Voyage AI key in this project, and no code path that calls one. All inference is local.",
  },
  {
    title: "Models may need an initial internet download",
    body: "Lemonade downloads model weights from Hugging Face or ModelScope the first time you pull them. That download is handled by Lemonade, not by this app, and happens before you use it.",
  },
  {
    title: "Your documents stay on your machine",
    body: "Document metadata, extracted pages, chunks, embeddings, chat history, model choices and benchmark results live in this browser's IndexedDB database, named ai-campus-copilot-local.",
  },
  {
    title: "You can delete everything",
    body: "Use the button below, or the per-document delete on the Documents page. Clearing site data in your browser settings removes it too.",
  },
  {
    title: "Only upload documents you may process",
    body: "Make sure you have permission to process any document you upload, and avoid uploading other people's personal information.",
  },
  {
    title: "No telemetry",
    body: "This app collects no analytics, sends no crash reports and loads no remote fonts, scripts or images. It works with no internet connection once Lemonade has its models.",
  },
];

export default function PrivacyPage() {
  const { activeRequests } = useLemonade();
  const toast = useToast();

  const handleDeleteAll = useCallback(async () => {
    if (!window.confirm("Delete all local data stored by AI Campus Copilot Local?")) return;
    await deleteAllData();
    toast.success("All local data deleted from this browser.");
  }, [toast]);

  return (
    <>
      <PageHeader
        title="Privacy"
        description="What this app does with your documents, in plain terms."
      />

      <div className="flex flex-col gap-6">
        <Callout title="Live network activity">
          <p>
            {activeRequests > 0
              ? `${activeRequests} request${activeRequests === 1 ? "" : "s"} to your local Lemonade Server in flight right now.`
              : "No requests to Lemonade in flight right now."}{" "}
            The indicator in the top bar shows this on every page, so you can always see when the
            app is talking to the model.
          </p>
        </Callout>

        <ul className="grid gap-4 md:grid-cols-2">
          {POINTS.map((point) => (
            <Card key={point.title} as="li">
              <h2 className="text-sm font-semibold text-ink">{point.title}</h2>
              <p className="mt-1.5 text-sm text-ink-muted">{point.body}</p>
            </Card>
          ))}
        </ul>

        <Card>
          <h2 className="text-sm font-semibold text-ink">Where data actually travels</h2>
          <pre className="mt-3 overflow-x-auto rounded-lg bg-surface-sunken p-3 font-mono text-xs text-ink-muted">
            {`Your file        →  browser memory only (never transmitted)
Extracted text   →  browser  →  localhost Next.js  →  localhost Lemonade
Embeddings       →  stored in browser IndexedDB
Your questions   →  browser  →  localhost Next.js  →  localhost Lemonade
Answers          →  stored in browser IndexedDB

Every hop above is on your own machine.`}
          </pre>
          <p className="mt-3 text-xs text-ink-muted">
            The Next.js hop exists so the Lemonade URL and any API key stay server-side and never
            reach the browser bundle. Both processes run locally.
          </p>
        </Card>

        <Card>
          <h2 className="text-sm font-semibold text-ink">Delete all local data</h2>
          <p className="mt-1.5 text-sm text-ink-muted">
            Removes every document, page, chunk, embedding, chat turn, extraction and benchmark from
            this browser, along with your model selections. This cannot be undone.
          </p>
          <button type="button" className="btn-danger mt-3" onClick={() => void handleDeleteAll()}>
            <Trash2 className="h-4 w-4" aria-hidden />
            Delete all local data
          </button>
        </Card>
      </div>
    </>
  );
}
