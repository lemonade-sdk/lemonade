"use client";

import {
  ArrowRight,
  Briefcase,
  CalendarClock,
  Gauge,
  MessagesSquare,
  ScrollText,
  ShieldCheck,
  Upload,
} from "lucide-react";
import Link from "next/link";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { Card, Chip } from "@/components/ui/Primitives";

const FEATURES = [
  {
    icon: MessagesSquare,
    title: "Grounded answers",
    body: "Ask in English or Tamil. Every answer comes from passages retrieved out of your document, with page citations you can expand and check.",
  },
  {
    icon: ScrollText,
    title: "Six kinds of summary",
    body: "Short, detailed, key points, student-friendly, English and Tamil. Long documents are summarised in stages, never in one oversized request.",
  },
  {
    icon: CalendarClock,
    title: "Deadline extraction",
    body: "Exam dates, fee deadlines, application windows and interviews as schema-validated JSON. Ambiguous dates are left un-normalised rather than guessed.",
  },
  {
    icon: Briefcase,
    title: "Placement details",
    body: "Organisation, role, eligibility, skills, stipend and how to apply — pulled out of placement circulars into readable cards.",
  },
  {
    icon: Gauge,
    title: "Honest benchmarks",
    body: "Time to first token, tokens per second and retrieval latency measured on your own hardware. Nothing is estimated or invented.",
  },
  {
    icon: ShieldCheck,
    title: "Private by construction",
    body: "Documents are parsed in your browser and stored in IndexedDB. Inference goes to your own Lemonade Server. No cloud AI provider is involved.",
  },
];

export default function HomePage() {
  const { status, isReady } = useLemonade();

  return (
    <div className="flex flex-col gap-10">
      <section className="text-center">
        <Chip tone="brand" className="mx-auto">
          Powered by Lemonade local AI
        </Chip>
        <h1 className="mt-4 text-3xl font-semibold tracking-tight text-ink sm:text-4xl">
          Understand any college notice, privately
        </h1>
        <p className="mx-auto mt-3 max-w-2xl text-base text-ink-muted">
          AI Campus Copilot Local turns examination schedules, circulars, scholarship notices and
          placement announcements into answers you can trust — running entirely on your own machine
          through Lemonade Server.
        </p>

        <div className="mt-6 flex flex-wrap justify-center gap-3">
          <Link href={isReady ? "/documents" : "/setup"} className="btn-primary">
            {isReady ? (
              <>
                <Upload className="h-4 w-4" aria-hidden />
                Upload a document
              </>
            ) : (
              <>
                Connect Lemonade
                <ArrowRight className="h-4 w-4" aria-hidden />
              </>
            )}
          </Link>
          <Link href="/about" className="btn-ghost">
            About this project
          </Link>
        </div>

        {status !== "connected" ? (
          <p className="mt-4 text-sm text-caution">
            Lemonade Server is not connected yet. The{" "}
            <Link href="/setup" className="underline">
              Setup page
            </Link>{" "}
            walks you through starting it and choosing models.
          </p>
        ) : null}
      </section>

      <section>
        <h2 className="sr-only">Features</h2>
        <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-3">
          {FEATURES.map((feature) => {
            const Icon = feature.icon;
            return (
              <Card key={feature.title}>
                <Icon className="h-5 w-5 text-brand" aria-hidden />
                <h3 className="mt-3 text-sm font-semibold text-ink">{feature.title}</h3>
                <p className="mt-1.5 text-sm text-ink-muted">{feature.body}</p>
              </Card>
            );
          })}
        </div>
      </section>

      <section>
        <h2 className="mb-4 text-lg font-semibold text-ink">How it works</h2>
        <ol className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          {[
            { step: "1", title: "Upload", body: "A PDF, TXT or Markdown notice, read in your browser." },
            { step: "2", title: "Process", body: "Page-aware extraction and chunking, then embeddings from Lemonade." },
            { step: "3", title: "Retrieve", body: "Your question is embedded and matched by cosine similarity." },
            { step: "4", title: "Answer", body: "Lemonade generates a grounded answer with page citations." },
          ].map((item) => (
            <Card key={item.step} as="li">
              <span className="flex h-7 w-7 items-center justify-center rounded-full bg-brand-soft text-sm font-semibold text-brand">
                {item.step}
              </span>
              <h3 className="mt-2.5 text-sm font-semibold text-ink">{item.title}</h3>
              <p className="mt-1 text-sm text-ink-muted">{item.body}</p>
            </Card>
          ))}
        </ol>
      </section>

      <Card className="text-center">
        <p className="text-sm text-ink-muted">
          Created by <strong className="text-ink">Jerome Prakash L</strong> for the AMD Lemonade
          Developer Challenge. Lemonade Server itself is a community project maintained by the
          Lemonade community and AMD contributors — this app is a separate client that consumes its
          public APIs.
        </p>
      </Card>
    </div>
  );
}
