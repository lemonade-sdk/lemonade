"use client";

import { Check } from "lucide-react";
import { formatModelSize, type ClassifiedModel } from "@/lib/lemonade/models";
import { Chip } from "@/components/ui/Primitives";
import { cn } from "@/lib/utils/cn";

export function ModelPicker({
  id,
  label,
  description,
  models,
  selected,
  onSelect,
  emptyHint,
}: {
  id: string;
  label: string;
  description: string;
  models: ClassifiedModel[];
  selected: string | null;
  onSelect: (modelId: string) => void;
  emptyHint: string;
}) {
  return (
    <fieldset className="card p-4">
      <legend className="sr-only">{label}</legend>
      <div className="mb-3">
        <h3 className="text-sm font-semibold text-ink">{label}</h3>
        <p className="mt-0.5 text-sm text-ink-muted">{description}</p>
      </div>

      {models.length === 0 ? (
        <p className="rounded-lg bg-surface-sunken p-3 text-sm text-ink-muted">{emptyHint}</p>
      ) : (
        <div className="flex max-h-72 flex-col gap-1.5 overflow-y-auto" role="radiogroup" aria-labelledby={`${id}-label`}>
          <span id={`${id}-label`} className="sr-only">
            {label}
          </span>
          {models.map((model) => {
            const active = model.id === selected;
            return (
              <button
                key={model.id}
                type="button"
                role="radio"
                aria-checked={active}
                onClick={() => onSelect(model.id)}
                className={cn(
                  "flex items-center gap-3 rounded-lg border px-3 py-2.5 text-left transition-colors",
                  active
                    ? "border-brand bg-brand-soft"
                    : "border-line hover:bg-surface-sunken",
                )}
              >
                <span
                  className={cn(
                    "flex h-4 w-4 shrink-0 items-center justify-center rounded-full border",
                    active ? "border-brand bg-brand text-white" : "border-line",
                  )}
                  aria-hidden
                >
                  {active ? <Check className="h-3 w-3" /> : null}
                </span>
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-sm font-medium text-ink">{model.id}</span>
                  <span className="mt-0.5 flex flex-wrap items-center gap-1.5 text-xs text-ink-muted">
                    <span>{formatModelSize(model)}</span>
                    {model.recipe ? <span>· {model.recipe}</span> : null}
                    {model.maxContextWindow ? (
                      <span>· {model.maxContextWindow.toLocaleString()} ctx</span>
                    ) : null}
                  </span>
                </span>
                {model.labels.slice(0, 2).map((label) => (
                  <Chip key={label} className="hidden shrink-0 sm:inline-flex">
                    {label}
                  </Chip>
                ))}
              </button>
            );
          })}
        </div>
      )}
    </fieldset>
  );
}
