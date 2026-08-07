"use client";

import { Check, Copy, MessagesSquare, Send, Square, Trash2 } from "lucide-react";
import Link from "next/link";
import { useCallback, useEffect, useRef, useState } from "react";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { useToast } from "@/components/providers/ToastProvider";
import { DocumentSelect } from "@/components/documents/DocumentSelect";
import { SourcePanel } from "@/components/chat/SourcePanel";
import { Callout, Card, EmptyState, PageHeader, SkeletonList } from "@/components/ui/Primitives";
import { addChatTurn, clearChats, getChunks, listChats } from "@/lib/db/repo";
import type { ChatTurn } from "@/lib/db/schema";
import type { Chunk } from "@/lib/documents/types";
import { useDocuments } from "@/lib/hooks/useDocuments";
import { isLemonadeError } from "@/lib/lemonade/errors";
import { askQuestion, toChatTurnSources } from "@/lib/rag/ask";
import { recentHistory } from "@/lib/rag/prompt";
import { cn, createId, formatDuration } from "@/lib/utils/cn";

const SUGGESTIONS = [
  "What are the important dates in this notice?",
  "Who is eligible to apply?",
  "What documents do I need to submit?",
  "இந்த அறிவிப்பில் உள்ள முக்கியமான தேதிகள் என்ன?",
];

export default function ChatPage() {
  const { client, chatModel, embeddingModel, isReady, beginRequest } = useLemonade();
  const { documents, activeDocument, activeDocumentId, setActiveDocumentId, loading } =
    useDocuments();
  const toast = useToast();

  const [turns, setTurns] = useState<ChatTurn[]>([]);
  const [chunks, setChunks] = useState<Chunk[]>([]);
  const [question, setQuestion] = useState("");
  const [streamingText, setStreamingText] = useState("");
  const [generating, setGenerating] = useState(false);
  const [copiedId, setCopiedId] = useState<string | null>(null);

  const abortRef = useRef<AbortController | null>(null);
  const endRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!activeDocumentId) {
      setTurns([]);
      setChunks([]);
      return;
    }
    let cancelled = false;
    void (async () => {
      const [history, docChunks] = await Promise.all([
        listChats(activeDocumentId),
        getChunks(activeDocumentId),
      ]);
      if (cancelled) return;
      setTurns(history);
      setChunks(docChunks);
    })();
    return () => {
      cancelled = true;
    };
  }, [activeDocumentId]);

  useEffect(() => {
    endRef.current?.scrollIntoView({ behavior: "smooth", block: "end" });
  }, [turns.length, streamingText]);

  const submit = useCallback(
    async (text: string) => {
      const trimmed = text.trim();
      if (!trimmed || !activeDocument || !chatModel || !embeddingModel || generating) return;

      const userTurn: ChatTurn = {
        id: createId("turn"),
        documentId: activeDocument.id,
        role: "user",
        content: trimmed,
        createdAt: Date.now(),
        sources: [],
        metrics: null,
      };

      setTurns((current) => [...current, userTurn]);
      setQuestion("");
      setStreamingText("");
      setGenerating(true);
      await addChatTurn(userTurn);

      const controller = new AbortController();
      abortRef.current = controller;
      const endRequest = beginRequest();

      try {
        const result = await askQuestion({
          client,
          chatModel,
          embeddingModel,
          question: trimmed,
          chunks,
          filename: activeDocument.filename,
          history: recentHistory(turns),
          signal: controller.signal,
          onDelta: (delta) => setStreamingText((current) => current + delta),
        });

        const assistantTurn: ChatTurn = {
          id: createId("turn"),
          documentId: activeDocument.id,
          role: "assistant",
          content: result.answer,
          createdAt: Date.now(),
          sources: toChatTurnSources(result.sources),
          metrics: result.metrics,
        };

        setTurns((current) => [...current, assistantTurn]);
        await addChatTurn(assistantTurn);
      } catch (error) {
        if (isLemonadeError(error) && error.kind === "cancelled") {
          toast.info("Generation stopped.");
        } else {
          toast.error(isLemonadeError(error) ? error.displayMessage : (error as Error).message);
        }
      } finally {
        endRequest();
        setGenerating(false);
        setStreamingText("");
        abortRef.current = null;
      }
    },
    [
      activeDocument,
      beginRequest,
      chatModel,
      chunks,
      client,
      embeddingModel,
      generating,
      toast,
      turns,
    ],
  );

  const handleClear = useCallback(async () => {
    if (!activeDocumentId) return;
    await clearChats(activeDocumentId);
    setTurns([]);
    toast.success("Chat history cleared.");
  }, [activeDocumentId, toast]);

  const handleCopy = useCallback(async (turn: ChatTurn) => {
    await navigator.clipboard.writeText(turn.content);
    setCopiedId(turn.id);
    setTimeout(() => setCopiedId(null), 1500);
  }, []);

  if (loading) return <SkeletonList rows={3} />;

  if (documents.length === 0) {
    return (
      <EmptyState
        icon={<MessagesSquare className="h-8 w-8" aria-hidden />}
        title="Upload a document first"
        description="Answers are grounded in a document you have processed locally. Upload a notice to start asking questions."
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
        title="Ask your document"
        description="Every answer is generated by your local Lemonade Server from passages retrieved out of the selected document, with page citations."
        actions={
          turns.length > 0 ? (
            <button type="button" className="btn-ghost text-danger" onClick={() => void handleClear()}>
              <Trash2 className="h-4 w-4" aria-hidden />
              Clear history
            </button>
          ) : null
        }
      />

      <div className="flex flex-col gap-4">
        <Card>
          <DocumentSelect
            documents={documents}
            value={activeDocumentId}
            onChange={(id) => void setActiveDocumentId(id)}
          />
          {activeDocument && activeDocument.status !== "embedded" ? (
            <p className="mt-2 text-xs text-caution">
              This document has no embeddings yet. Rebuild them on the Documents page before asking
              questions.
            </p>
          ) : null}
        </Card>

        {!isReady ? (
          <Callout tone="warning" title="Finish setup before asking questions">
            Connect to Lemonade Server and choose a chat model and an embedding model on the{" "}
            <Link href="/setup">Setup page</Link>.
          </Callout>
        ) : null}

        <div className="flex flex-col gap-4">
          {turns.length === 0 && !generating ? (
            <Card>
              <p className="text-sm text-ink-muted">
                Try one of these, or ask anything about {activeDocument?.filename}:
              </p>
              <div className="mt-3 flex flex-wrap gap-2">
                {SUGGESTIONS.map((suggestion) => (
                  <button
                    key={suggestion}
                    type="button"
                    className="btn-ghost text-xs"
                    onClick={() => void submit(suggestion)}
                    disabled={!isReady}
                  >
                    {suggestion}
                  </button>
                ))}
              </div>
            </Card>
          ) : null}

          {turns.map((turn) => (
            <div
              key={turn.id}
              className={cn(
                "rounded-xl border p-4",
                turn.role === "user"
                  ? "border-transparent bg-brand-soft"
                  : "border-line bg-surface-raised",
              )}
            >
              <div className="mb-2 flex items-center justify-between gap-2">
                <span className="text-xs font-semibold uppercase tracking-wide text-ink-faint">
                  {turn.role === "user" ? "You" : "Campus Copilot"}
                </span>
                {turn.role === "assistant" ? (
                  <button
                    type="button"
                    className="btn-ghost px-2 py-1 text-xs"
                    onClick={() => void handleCopy(turn)}
                    aria-label="Copy answer"
                  >
                    {copiedId === turn.id ? (
                      <Check className="h-3.5 w-3.5" aria-hidden />
                    ) : (
                      <Copy className="h-3.5 w-3.5" aria-hidden />
                    )}
                    {copiedId === turn.id ? "Copied" : "Copy"}
                  </button>
                ) : null}
              </div>

              {/* Model output is rendered as plain text — never as HTML. */}
              <p className="prose-answer">{turn.content}</p>

              {turn.role === "assistant" ? (
                <>
                  <SourcePanel sources={turn.sources} />
                  {turn.metrics ? (
                    <p className="mt-2 text-xs tabular-nums text-ink-faint">
                      embed {formatDuration(turn.metrics.questionEmbeddingMs)} · retrieve{" "}
                      {formatDuration(turn.metrics.retrievalMs)} · first token{" "}
                      {formatDuration(turn.metrics.timeToFirstTokenMs)} · total{" "}
                      {formatDuration(turn.metrics.totalMs)}
                      {turn.metrics.tokensPerSecond !== null
                        ? ` · ${turn.metrics.tokensPerSecond.toFixed(1)} tok/s`
                        : ""}
                    </p>
                  ) : null}
                </>
              ) : null}
            </div>
          ))}

          {generating ? (
            <div className="rounded-xl border border-line bg-surface-raised p-4">
              <span className="text-xs font-semibold uppercase tracking-wide text-ink-faint">
                Campus Copilot
              </span>
              <p className="prose-answer mt-2">
                {streamingText || "Retrieving relevant passages…"}
                <span className="ml-0.5 inline-block h-4 w-1.5 animate-pulse bg-brand align-middle" />
              </p>
            </div>
          ) : null}

          <div ref={endRef} />
        </div>

        <form
          className="sticky bottom-16 z-20 flex items-end gap-2 rounded-xl border border-line bg-surface-raised p-2 lg:bottom-4"
          onSubmit={(event) => {
            event.preventDefault();
            void submit(question);
          }}
        >
          <label htmlFor="question" className="sr-only">
            Your question
          </label>
          <textarea
            id="question"
            rows={1}
            value={question}
            onChange={(event) => setQuestion(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter" && !event.shiftKey) {
                event.preventDefault();
                void submit(question);
              }
            }}
            placeholder="Ask about deadlines, eligibility, fees… (English or தமிழ்)"
            disabled={!isReady || generating}
            className="field max-h-40 flex-1 resize-y border-0 focus:ring-0"
          />
          {generating ? (
            <button
              type="button"
              className="btn-danger shrink-0"
              onClick={() => abortRef.current?.abort()}
            >
              <Square className="h-4 w-4" aria-hidden />
              Stop
            </button>
          ) : (
            <button type="submit" className="btn-primary shrink-0" disabled={!isReady || !question.trim()}>
              <Send className="h-4 w-4" aria-hidden />
              Ask
            </button>
          )}
        </form>
      </div>
    </>
  );
}
