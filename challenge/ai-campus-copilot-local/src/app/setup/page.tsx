"use client";

import { CircleAlert, CircleCheck, CircleSlash, Loader2, RefreshCw } from "lucide-react";
import { useLemonade, type ConnectionStatus } from "@/components/providers/LemonadeProvider";
import { useToast } from "@/components/providers/ToastProvider";
import { ModelPicker } from "@/components/setup/ModelPicker";
import { ServerUrlField } from "@/components/setup/ServerUrlField";
import { Callout, Card, Chip, PageHeader, SkeletonList } from "@/components/ui/Primitives";

const STATUS_COPY: Record<
  ConnectionStatus,
  { title: string; tone: "info" | "warning" | "danger"; icon: typeof CircleCheck }
> = {
  checking: { title: "Checking Lemonade Server…", tone: "info", icon: Loader2 },
  connected: { title: "Connected to Lemonade Server", tone: "info", icon: CircleCheck },
  server_unavailable: {
    title: "Lemonade Server is not reachable",
    tone: "danger",
    icon: CircleSlash,
  },
  no_models_installed: {
    title: "Connected, but no models are downloaded",
    tone: "warning",
    icon: CircleAlert,
  },
  request_failed: { title: "The request to Lemonade failed", tone: "danger", icon: CircleAlert },
};

export default function SetupPage() {
  const {
    status,
    statusDetail,
    health,
    systemInfo,
    allModels,
    availableChatModels,
    availableEmbeddingModels,
    chatModel,
    embeddingModel,
    setChatModel,
    setEmbeddingModel,
    refresh,
    isRefreshing,
    isReady,
    isDirectMode,
    serverUrl,
  } = useLemonade();
  const toast = useToast();

  const copy = STATUS_COPY[status];
  const Icon = copy.icon;

  return (
    <>
      <PageHeader
        title="Lemonade connection"
        description="This app talks only to the Lemonade Server running on your own machine. Nothing here reaches a cloud AI provider."
        actions={
          <button
            type="button"
            className="btn-ghost"
            onClick={() => void refresh()}
            disabled={isRefreshing}
          >
            <RefreshCw className={isRefreshing ? "h-4 w-4 animate-spin" : "h-4 w-4"} aria-hidden />
            {isRefreshing ? "Checking…" : "Retry / refresh"}
          </button>
        }
      />

      <div className="flex flex-col gap-6">
        {isDirectMode ? (
          <Callout tone="warning" title="You are using the hosted demo">
            <p>
              This page is served from the internet, but the AI still runs on{" "}
              <strong>your</strong> machine — your browser calls your own Lemonade Server
              directly. Nothing is sent to a cloud model.
            </p>
            <p className="mt-2">
              For that to work Lemonade must be running locally, and your browser must allow this
              page to reach it. Some browsers block or prompt before a hosted page contacts a local
              address. If the connection keeps failing, run the app locally instead — see the
              README. That path is the supported one.
            </p>
          </Callout>
        ) : null}

        <ServerUrlField />

        <Card>
          <div className="flex items-start gap-3">
            <Icon
              className={`mt-0.5 h-5 w-5 shrink-0 ${
                status === "connected"
                  ? "text-positive"
                  : status === "checking"
                    ? "animate-spin text-ink-muted"
                    : status === "no_models_installed"
                      ? "text-caution"
                      : "text-danger"
              }`}
              aria-hidden
            />
            <div className="min-w-0 flex-1">
              <h2 className="text-sm font-semibold text-ink">{copy.title}</h2>
              <p className="mt-1 text-sm text-ink-muted">
                {statusDetail ??
                  (status === "connected"
                    ? `Lemonade Server ${health?.version ? `v${health.version}` : ""} responded to /api/v1/health.`
                    : "Waiting for a response…")}
              </p>

              <dl className="mt-3 grid gap-x-6 gap-y-1.5 text-sm sm:grid-cols-2">
                <Row label="Server URL">
                  <code className="font-mono text-xs">
                    {isDirectMode
                      ? (serverUrl ?? "not set")
                      : "proxied via this app to LEMONADE_SERVER_URL"}
                  </code>
                </Row>
                <Row label="Server version">{health?.version ?? "—"}</Row>
                <Row label="Models available">{allModels.length}</Row>
                <Row label="Currently loaded">
                  {health?.all_models_loaded?.length
                    ? health.all_models_loaded.map((m) => m.model_name).join(", ")
                    : "none"}
                </Row>
                {systemInfo?.["OS Version"] ? (
                  <Row label="Operating system">{systemInfo["OS Version"]}</Row>
                ) : null}
                {systemInfo?.["Processor"] ? (
                  <Row label="Processor">{systemInfo["Processor"]}</Row>
                ) : null}
              </dl>
            </div>
            <Chip tone={isReady ? "positive" : "caution"}>
              {isReady ? "Ready to use" : "Setup incomplete"}
            </Chip>
          </div>
        </Card>

        {status === "server_unavailable" || status === "request_failed" ? (
          <Callout tone="danger" title="Start Lemonade Server, then press retry">
            <ol className="ml-4 list-decimal space-y-1">
              <li>
                Install Lemonade Server from{" "}
                <a href="https://lemonade-server.ai/docs/guide/install/" rel="noreferrer">
                  lemonade-server.ai
                </a>
                .
              </li>
              <li>
                Launch it. On Windows 11, <code className="font-mono text-xs">LemonadeServer.exe</code>{" "}
                starts automatically and shows a tray icon.
              </li>
              <li>
                Confirm it answers at{" "}
                <code className="font-mono text-xs">http://127.0.0.1:13305/api/v1/health</code>.
              </li>
              <li>
                If you changed the port, set{" "}
                <code className="font-mono text-xs">LEMONADE_SERVER_URL</code> in{" "}
                <code className="font-mono text-xs">.env.local</code> and restart this app.
              </li>
            </ol>
          </Callout>
        ) : null}

        {status === "no_models_installed" ? (
          <Callout tone="warning" title="Download at least one chat model and one embedding model">
            <p className="mb-2">
              This app never assumes a particular model. Pull whatever your machine can run, then
              press refresh — the lists below fill in from{" "}
              <code className="font-mono text-xs">GET /api/v1/models</code>.
            </p>
            <pre className="overflow-x-auto rounded-lg bg-surface-sunken p-3 font-mono text-xs">
              {`lemonade list                 # see what is available\nlemonade pull <chat-model>    # any text-generation model\nlemonade pull <embed-model>   # a model labelled "embeddings"`}
            </pre>
          </Callout>
        ) : null}

        {status === "checking" ? (
          <SkeletonList rows={2} />
        ) : (
          <div className="grid gap-4 lg:grid-cols-2">
            <ModelPicker
              id="chat-model"
              label="Chat model"
              description="Generates grounded answers, summaries and structured extractions."
              models={availableChatModels}
              selected={chatModel}
              onSelect={(id) => {
                void setChatModel(id);
                toast.success(`Chat model set to ${id}`);
              }}
              emptyHint="No text-generation models are downloaded. Pull one with `lemonade pull <model>`, then refresh."
            />
            <ModelPicker
              id="embedding-model"
              label="Embedding model"
              description="Turns document chunks and your questions into vectors for retrieval."
              models={availableEmbeddingModels}
              selected={embeddingModel}
              onSelect={(id) => {
                void setEmbeddingModel(id);
                toast.success(`Embedding model set to ${id}`);
              }}
              emptyHint={
                'No embedding models are downloaded. Lemonade supports embeddings on the llamacpp and flm recipes — pull a model labelled "embeddings", then refresh.'
              }
            />
          </div>
        )}

        <Callout title="Where your selections are stored">
          Your model choices live in this browser&apos;s IndexedDB, on this machine. They are never
          written to Lemonade Server, so other Lemonade clients are unaffected.
        </Callout>
      </div>
    </>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-wrap gap-x-2">
      <dt className="text-ink-faint">{label}:</dt>
      <dd className="min-w-0 break-words text-ink-muted">{children}</dd>
    </div>
  );
}
