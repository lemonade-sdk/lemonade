import type { AutoOptBudget, AutoOptKvCacheQuant, AutoOptParallelMode, AutoOptRamHeadroom } from './autoOptTypes';

export const WIZARD_TITLE = 'Optimize this model';
export const WIZARD_INTRO = 'AutoOpt benchmarks this model on your hardware and recommends the fastest safe configuration. Answer a few questions so it optimizes for how you actually use it.';

export const MODEL_STEP = {
  legend: 'Which model should be optimized?',
  help: 'AutoOpt tunes one chat or omni model at a time. The result applies to this exact model on this machine.',
  empty: 'No chat-capable models are available. Download a chat or omni model first.',
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
  legend: 'How much system memory should stay free?',
  help: 'AutoOpt leaves headroom so the rest of your system stays responsive while the model is loaded.',
  suggestionChip: (gb: number) => `Suggested for this machine (${gb} GB RAM)`,
  options: [
    {
      value: 'normal' as AutoOptRamHeadroom,
      label: 'Normal headroom',
      description: 'Keeps a comfortable reserve for your browser, IDE, and other apps. Recommended on machines with plenty of RAM.',
    },
    {
      value: 'reduced' as AutoOptRamHeadroom,
      label: 'Reduced headroom',
      description: 'Gives the model more memory and keeps a smaller reserve. Fine when the model is your main workload.',
    },
    {
      value: 'minimal' as AutoOptRamHeadroom,
      label: 'Minimal headroom',
      description: 'Nearly everything goes to the model. Other apps may swap or stutter while it is loaded.',
    },
    {
      value: 'disabled' as AutoOptRamHeadroom,
      label: 'Disabled (no reserve)',
      description: 'No memory reserve at all. Warning: on hybrid or recurrent models the KV cache cannot be shifted, so an out-of-memory context overflow forces a full prompt recompute instead of a cheap truncation.',
    },
  ],
};

export const VISION_STEP = {
  legend: 'Will you send images to this model?',
  help: 'The vision projector (mmproj) keeps image input working but permanently occupies memory that could otherwise hold context.',
  options: [
    {
      value: true,
      label: 'Yes, I use image input',
      description: 'Keep the vision projector loaded. Some memory is reserved for it at all times.',
    },
    {
      value: false,
      label: 'No, text only',
      description: "Skip the projector and give its memory to the context window. You can turn it back on later in Model Tuning.",
    },
  ],
};

export const BUDGET_STEP = {
  legend: 'How thorough should the optimization be?',
  options: [
    {
      value: 'quick' as AutoOptBudget,
      label: 'Quick',
      description: 'Roughly 2–5 minutes. Probes memory fit and a small benchmark grid. Good first pass.',
    },
    {
      value: 'standard' as AutoOptBudget,
      label: 'Standard',
      description: 'Roughly 10–20 minutes. Benchmarks the candidate backends and batch sizes that matter. Recommended.',
    },
    {
      value: 'thorough' as AutoOptBudget,
      label: 'Thorough',
      description: 'Roughly 30–60 minutes. Full sweep across backends, batch ladders, and long-context depths for the most precise result.',
    },
  ],
  networkLabel: 'Allow downloading backends during optimization',
  networkHelp: 'When enabled, AutoOpt may fetch additional inference backends to compare. Disable to only benchmark what is already installed.',
  consentLabel: 'AutoOpt may unload the models currently loaded on this server while it benchmarks',
  consentHelp: 'Benchmarking needs exclusive access to the hardware. Loaded models are evicted during the run and are not reloaded automatically.',
};

export const REVIEW_STEP = {
  legend: 'Review and start',
  help: 'AutoOpt will benchmark with these answers. You can cancel the run at any time.',
};

export const RUNNING_STEP = {
  closeNote: 'You can close this — the run continues on the server and stays in the AutoOpt rail.',
};

export const KV_QUANT_LABELS: Record<AutoOptKvCacheQuant, string> = {
  none: 'Full quality',
  q8_0: 'Balanced (q8_0)',
  q5_1: 'More context (q5_1)',
  q4_0: 'Maximum context (q4_0)',
};

export const RAM_HEADROOM_LABELS: Record<AutoOptRamHeadroom, string> = {
  normal: 'Normal',
  reduced: 'Reduced',
  minimal: 'Minimal',
  disabled: 'Disabled',
};

export const BUDGET_LABELS: Record<AutoOptBudget, string> = {
  quick: 'Quick',
  standard: 'Standard',
  thorough: 'Thorough',
};
