import type { Metadata } from "next";
import { Card, PageHeader } from "@/components/ui/Primitives";

export const metadata: Metadata = { title: "About" };

const ENDPOINTS = [
  ["GET /api/v1/health", "Connection check and loaded-model state"],
  ["GET /api/v1/models", "Dynamic model discovery — nothing is hardcoded"],
  ["GET /api/v1/system-info", "OS, processor and memory shown on the performance page"],
  ["GET /api/v1/system-stats", "Host resource usage"],
  ["GET /api/v1/stats", "Tokens per second and time to first token after a generation"],
  ["POST /api/v1/embeddings", "Chunk and question embeddings"],
  ["POST /api/v1/chat/completions", "Grounded answers, summaries and structured extraction"],
];

export default function AboutPage() {
  return (
    <>
      <PageHeader
        title="About"
        description="AI Campus Copilot Local — a privacy-first student assistant powered by Lemonade local AI."
      />

      <div className="flex flex-col gap-6">
        <Card>
          <h2 className="text-sm font-semibold text-ink">Attribution</h2>
          <ul className="mt-2 flex list-disc flex-col gap-1.5 pl-5 text-sm text-ink-muted">
            <li>
              This application was created by <strong className="text-ink">Jerome Prakash L</strong>{" "}
              for the <strong className="text-ink">AMD Lemonade Developer Challenge</strong>.
            </li>
            <li>
              It uses <strong className="text-ink">Lemonade Server</strong> for all local inference.
            </li>
            <li>
              Lemonade Server itself is maintained by the Lemonade community and AMD contributors —
              it is not my work.
            </li>
            <li>
              The repository this app lives in is a fork of the official{" "}
              <a
                href="https://github.com/lemonade-sdk/lemonade"
                rel="noreferrer noopener"
                target="_blank"
                className="text-brand hover:underline"
              >
                lemonade-sdk/lemonade
              </a>{" "}
              repository.
            </li>
            <li>
              My original contribution is only the application inside{" "}
              <code className="font-mono text-xs">challenge/ai-campus-copilot-local/</code>. The
              upstream Lemonade source, licence, copyright notices and history are unmodified.
            </li>
          </ul>
        </Card>

        <Card>
          <h2 className="text-sm font-semibold text-ink">Lemonade endpoints used</h2>
          <p className="mt-1.5 text-sm text-ink-muted">
            Endpoint shapes were taken from the parent repository&apos;s own API documentation
            (<code className="font-mono text-xs">docs/api/openai.md</code> and{" "}
            <code className="font-mono text-xs">docs/api/lemonade.md</code>), not guessed.
          </p>
          <dl className="mt-3 flex flex-col gap-2 text-sm">
            {ENDPOINTS.map(([endpoint, purpose]) => (
              <div key={endpoint} className="flex flex-wrap gap-x-3">
                <dt className="font-mono text-xs text-brand">{endpoint}</dt>
                <dd className="text-ink-muted">{purpose}</dd>
              </div>
            ))}
          </dl>
        </Card>

        <Card>
          <h2 className="text-sm font-semibold text-ink">Technology</h2>
          <p className="mt-1.5 text-sm text-ink-muted">
            Next.js App Router, React 19, TypeScript in strict mode, Tailwind CSS, Lucide icons, Zod
            for request and model-output validation, PDF.js for page-aware extraction, Dexie over
            IndexedDB for local storage, Vitest for unit tests and Playwright for the end-to-end
            test.
          </p>
        </Card>

        <Card>
          <h2 className="text-sm font-semibold text-ink">Licence</h2>
          <p className="mt-1.5 text-sm text-ink-muted">
            Apache-2.0, matching the parent repository. See the{" "}
            <code className="font-mono text-xs">LICENSE</code> file in the project folder.
          </p>
        </Card>
      </div>
    </>
  );
}
