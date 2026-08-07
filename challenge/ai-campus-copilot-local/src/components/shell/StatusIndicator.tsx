"use client";

import { Activity, CircleAlert, CircleCheck, CircleSlash, Loader2, ShieldCheck } from "lucide-react";
import Link from "next/link";
import { useLemonade, type ConnectionStatus } from "@/components/providers/LemonadeProvider";
import { cn } from "@/lib/utils/cn";

const PRESENTATION: Record<
  ConnectionStatus,
  { label: string; icon: typeof CircleCheck; tone: string }
> = {
  checking: { label: "Checking Lemonade", icon: Loader2, tone: "text-ink-muted" },
  connected: { label: "Lemonade connected", icon: CircleCheck, tone: "text-positive" },
  server_unavailable: {
    label: "Lemonade not reachable",
    icon: CircleSlash,
    tone: "text-danger",
  },
  no_models_installed: { label: "No models installed", icon: CircleAlert, tone: "text-caution" },
  request_failed: { label: "Lemonade request failed", icon: CircleAlert, tone: "text-danger" },
};

export function StatusIndicator({ className }: { className?: string }) {
  const { status, isRefreshing } = useLemonade();
  const presentation = PRESENTATION[status];
  const Icon = presentation.icon;
  const spinning = status === "checking" || isRefreshing;

  return (
    <Link
      href="/setup"
      className={cn(
        "chip border-line transition-colors hover:bg-surface-sunken",
        presentation.tone,
        className,
      )}
      aria-label={`${presentation.label}. Open connection setup.`}
    >
      <Icon className={cn("h-3.5 w-3.5", spinning && "animate-spin")} aria-hidden />
      <span className="hidden sm:inline">{presentation.label}</span>
    </Link>
  );
}

/** Lights up whenever a request to the local Lemonade Server is in flight. */
export function NetworkActivityIndicator() {
  const { activeRequests } = useLemonade();
  const active = activeRequests > 0;

  return (
    <span
      className={cn(
        "chip border-line transition-colors",
        active ? "text-brand" : "text-ink-faint",
      )}
      role="status"
      aria-live="polite"
    >
      <Activity className={cn("h-3.5 w-3.5", active && "animate-pulse")} aria-hidden />
      <span className="hidden md:inline">
        {active
          ? `Talking to Lemonade (${activeRequests})`
          : "Idle — no traffic"}
      </span>
    </span>
  );
}

export function LocalAiBadge() {
  return (
    <span className="chip border-transparent bg-brand-soft text-brand" title="All inference runs on your own machine">
      <ShieldCheck className="h-3.5 w-3.5" aria-hidden />
      <span className="hidden sm:inline">100% local AI</span>
    </span>
  );
}
