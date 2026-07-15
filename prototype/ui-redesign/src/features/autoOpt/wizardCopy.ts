import type { AutoOptBudget, AutoOptKvCacheQuant, AutoOptParallelMode, AutoOptRamHeadroom } from './autoOptTypes';

export const WIZARD_TITLE = 'Optimize this model';
export const WIZARD_INTRO = 'AutoOpt benchmarks this model on your hardware and recommends the fastest safe configuration. The benchmark runs as a job on the server — you can close this window or reload the page and it keeps going. Answer a few questions so it optimizes for how you actually use it.';

export const MODEL_STEP = {
  legend: 'Which model should be optimized?',
  help: 'AutoOpt tunes one model at a time. The result applies to this exact model on this machine.',
  note: 'AutoOpt currently supports downloaded llama.cpp models; it compares llama.cpp backend variants (Vulkan/ROCm/CUDA), not other engines.',
  empty: 'No downloaded llama.cpp chat models are available. Download a llama.cpp chat or omni model first.',
};

export const PARALLEL_STEP = {
  legend: 'How will you use this model?',
  options: [
    {
      value: 'single' as AutoOptParallelMode,
      label: 'Just me, one conversation at a time',
      description: 'One request at a time gets the whole context window and the fastest single-stream speed.',
    },
    {
      value: 'parallel' as AutoOptParallelMode,
      label: 'Several chats or clients at once',
      description: 'Multiple requests are served in parallel slots. Total throughput goes up; each request gets a slice of the context.',
    },
  ],
  slotsLabel: 'Parallel slots',
  dedicatedLabel: 'This machine is a dedicated server (allow more aggressive memory use)',
  footnote: 'With N parallel slots, a context window of C tokens gives each request about C / N tokens. Example: 32K context with 4 slots means roughly 8K tokens per conversation.',
};

export const KV_QUANT_STEP = {
  legend: 'Context size vs. answer quality',
  help: 'Compressing the KV cache frees memory for a larger context window at a small quality cost.',
  options: [
    {
      value: 'none' as AutoOptKvCacheQuant,
      label: 'Full quality (no compression)',
      description: 'Keeps the KV cache at full precision. Best answer quality, largest memory use per token of context.',
    },
    {
      value: 'q8_0' as AutoOptKvCacheQuant,
      label: 'Balanced (q8_0)',
      description: 'Halves KV cache memory with virtually no measurable quality loss. The safe default when you want more context.',
    },
    {
      value: 'q5_1' as AutoOptKvCacheQuant,
      label: 'More context (q5_1)',
      description: 'About one third of the full-precision memory. Slight quality loss on long, detail-heavy tasks.',
    },
    {
      value: 'q4_0' as AutoOptKvCacheQuant,
      label: 'Maximum context (q4_0)',
      description: 'Quarter of the full-precision memory for the biggest context windows. Noticeable quality loss is possible; prefer q8_0 unless you truly need the space.',
    },
  ],
};

export const RAM_STEP = {
  legend: 'How much system RAM should stay free for other applications?',
  help: "This sizes llama.cpp's prompt cache: checkpoints of already-processed conversations kept in "
    + 'system RAM (--cache-ram, -ctxcp) so that switching back to a swapped-out conversation is instant '
    + 'instead of re-processing its whole prompt. A bigger cache means faster conversation switching; '
    + 'a smaller one keeps more RAM free for everything else you run.',
  suggestionChip: (gb: number) => `Suggested for this machine (${gb} GB RAM)`,
  options: [
    {
      value: 'normal' as AutoOptRamHeadroom,
      label: 'Default cache',
      description: "Keeps llama.cpp's default prompt-cache size. Every recent conversation resumes "
        + 'instantly; pick this when RAM is plentiful.',
    },
    {
      value: 'reduced' as AutoOptRamHeadroom,
      label: 'Reduced cache (4 GB, 16 checkpoints)',
      description: 'Caps the prompt cache at 4 GB of system RAM. Recent conversations still resume '
        + 'instantly; more RAM stays free for heavier applications running next to the model.',
    },
    {
      value: 'minimal' as AutoOptRamHeadroom,
      label: 'Minimal cache (2 GB, 8 checkpoints)',
      description: 'Caps the prompt cache at 2 GB. Keeps most system RAM free; switching back to '
        + 'older conversations re-processes their prompt instead of resuming instantly.',
    },
    {
      value: 'disabled' as AutoOptRamHeadroom,
      label: 'No cache (0 GB)',
      description: 'No prompt cache at all: every conversation switch re-processes the full prompt '
        + 'from scratch. Warning: hybrid and recurrent models cannot shift their cache, so any cache '
        + 'miss already forces a full prompt recompute — AutoOpt bumps them back to the minimal cache.',
    },
  ],
};

export const BUDGET_STEP = {
  legend: 'How thorough should the optimization be?',
  options: [
    {
      value: 'quick' as AutoOptBudget,
      label: 'Fast Scan',
      description: 'Heuristic memory fit only — no loads, no benchmarks. The recommendation is not load-validated. (~seconds)',
    },
    {
      value: 'standard' as AutoOptBudget,
      label: 'Benchmark',
      description: 'A server job loads and times each candidate: a backend duel at depth 0 and at deep context (~30k tokens), a batch ladder on unified-memory machines, and an MTP sweep {2,3,4} where supported. A memory-heavy load that fails falls back to a smaller context. (~5–15 min)',
    },
    {
      value: 'thorough' as AutoOptBudget,
      label: 'Deep Benchmark',
      description: 'The same measurements with full batch ladders {512…8192} and a wider MTP sweep {1…6}. (~15–45 min)',
    },
  ],
  networkLabel: 'Allow fetching model metadata from Hugging Face',
  networkHelp: "Only the base model's metadata and generation_config.json are fetched from Hugging Face. Nothing else is downloaded.",
  consentLabel: 'AutoOpt may unload the models currently loaded on this server while it benchmarks',
  consentHelp: 'The benchmark job needs exclusive access to the hardware: it repeatedly unloads and reloads the model to clear the prompt cache between timed runs, and normal inference queues behind it until it finishes. Loaded models are evicted during the run and are not reloaded automatically. Fast Scan runs no benchmarks and never unloads anything.',
};

export const REVIEW_STEP = {
  legend: 'Review and start',
  help: 'AutoOpt will run with these answers. You can cancel the run at any time.',
};

export const RUNNING_STEP = {
  closeNote: 'You can close this window or reload the page — the benchmark keeps running on the server and this view re-attaches to it.',
};

export const KV_QUANT_LABELS: Record<AutoOptKvCacheQuant, string> = {
  none: 'Full quality',
  q8_0: 'Balanced (q8_0)',
  q5_1: 'More context (q5_1)',
  q4_0: 'Maximum context (q4_0)',
};

export const RAM_HEADROOM_LABELS: Record<AutoOptRamHeadroom, string> = {
  normal: 'Default cache',
  reduced: 'Reduced cache (4 GB)',
  minimal: 'Minimal cache (2 GB)',
  disabled: 'No cache',
};

export const BUDGET_LABELS: Record<AutoOptBudget, string> = {
  quick: 'Fast Scan',
  standard: 'Benchmark',
  thorough: 'Deep Benchmark',
};
