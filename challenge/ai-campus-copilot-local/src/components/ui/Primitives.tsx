import type { ReactNode } from "react";
import { cn } from "@/lib/utils/cn";

export function PageHeader({
  title,
  description,
  actions,
}: {
  title: string;
  description?: string;
  actions?: ReactNode;
}) {
  return (
    <header className="mb-6 flex flex-wrap items-start justify-between gap-4">
      <div>
        <h1 className="text-2xl font-semibold tracking-tight text-ink">{title}</h1>
        {description ? (
          <p className="mt-1 max-w-2xl text-sm text-ink-muted">{description}</p>
        ) : null}
      </div>
      {actions ? <div className="flex flex-wrap items-center gap-2">{actions}</div> : null}
    </header>
  );
}

export function Card({
  children,
  className,
  as: Component = "div",
}: {
  children: ReactNode;
  className?: string;
  as?: "div" | "section" | "article" | "li";
}) {
  return <Component className={cn("card p-4", className)}>{children}</Component>;
}

export function EmptyState({
  icon,
  title,
  description,
  action,
}: {
  icon?: ReactNode;
  title: string;
  description: string;
  action?: ReactNode;
}) {
  return (
    <div className="card flex flex-col items-center gap-3 px-6 py-12 text-center">
      {icon ? <div className="text-ink-faint">{icon}</div> : null}
      <h2 className="text-base font-semibold text-ink">{title}</h2>
      <p className="max-w-md text-sm text-ink-muted">{description}</p>
      {action}
    </div>
  );
}

export function Skeleton({ className }: { className?: string }) {
  return <div className={cn("skeleton", className)} aria-hidden />;
}

export function SkeletonList({ rows = 3 }: { rows?: number }) {
  return (
    <div className="flex flex-col gap-3" role="status" aria-label="Loading">
      {Array.from({ length: rows }, (_, index) => (
        <div key={index} className="card flex flex-col gap-2 p-4">
          <Skeleton className="h-4 w-1/3" />
          <Skeleton className="h-3 w-2/3" />
        </div>
      ))}
    </div>
  );
}

export function Stat({
  label,
  value,
  hint,
}: {
  label: string;
  value: ReactNode;
  hint?: string;
}) {
  return (
    <div className="card p-4">
      <dt className="text-xs font-medium uppercase tracking-wide text-ink-faint">{label}</dt>
      <dd className="mt-1.5 text-xl font-semibold tabular-nums text-ink">{value}</dd>
      {hint ? <p className="mt-1 text-xs text-ink-muted">{hint}</p> : null}
    </div>
  );
}

const CHIP_TONES = {
  neutral: "border-line text-ink-muted",
  brand: "border-transparent bg-brand-soft text-brand",
  positive: "border-transparent bg-positive/10 text-positive",
  caution: "border-transparent bg-caution/10 text-caution",
  danger: "border-transparent bg-danger/10 text-danger",
} as const;

export function Chip({
  children,
  tone = "neutral",
  className,
}: {
  children: ReactNode;
  tone?: keyof typeof CHIP_TONES;
  className?: string;
}) {
  return <span className={cn("chip", CHIP_TONES[tone], className)}>{children}</span>;
}

export function Callout({
  tone = "info",
  title,
  children,
}: {
  tone?: "info" | "warning" | "danger";
  title?: string;
  children: ReactNode;
}) {
  const tones = {
    info: "border-brand/30 bg-brand-soft/60",
    warning: "border-caution/40 bg-caution/10",
    danger: "border-danger/40 bg-danger/10",
  } as const;

  return (
    <div className={cn("rounded-xl border p-4 text-sm", tones[tone])} role="note">
      {title ? <p className="mb-1 font-semibold text-ink">{title}</p> : null}
      <div className="text-ink-muted [&_a]:underline">{children}</div>
    </div>
  );
}

export function ProgressBar({ value, label }: { value: number; label: string }) {
  const clamped = Math.max(0, Math.min(100, value));
  return (
    <div>
      <div className="mb-1 flex items-center justify-between text-xs text-ink-muted">
        <span>{label}</span>
        <span className="tabular-nums">{Math.round(clamped)}%</span>
      </div>
      <div
        className="h-2 overflow-hidden rounded-full bg-surface-sunken"
        role="progressbar"
        aria-valuenow={Math.round(clamped)}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-label={label}
      >
        <div
          className="h-full rounded-full bg-brand transition-[width] duration-200"
          style={{ width: `${clamped}%` }}
        />
      </div>
    </div>
  );
}
