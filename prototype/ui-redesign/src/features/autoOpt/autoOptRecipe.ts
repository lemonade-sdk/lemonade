import { backendLoadArgs, recommendedCtxSize } from './autoOptSynthesize';
import {
  BenchParams,
  BenchPlanEntry,
  FitEstimate,
  HardwareSnapshot,
  ModelFacts,
  WizardAnswers,
} from './autoOptTypes';

export interface BenchRecipe {
  steps: Record<string, unknown>[];
  inputs: Record<string, unknown>;
  plan: BenchPlanEntry[];
  step_labels: Record<string, string>;
}

const DEPTH_SENTENCE = 'The quick brown fox jumps over the lazy dog. ';
const DEPTH_TOKENS_PER_SENTENCE = 11;
const FALLBACK_CTX = 2048;
const MAX_COMPLETION_TOKENS = 64;
const DEEP_GEN_MARGIN = 1024;
const MIN_DEEP_DEPTH = 2048;

function depthPrompt(depthTokens: number): string {
  if (depthTokens <= 0) return 'Reply with a one-word greeting.';
  const repeats = Math.max(1, Math.ceil(depthTokens / DEPTH_TOKENS_PER_SENTENCE));
  return DEPTH_SENTENCE.repeat(repeats) + '\nReply with a one-word summary of the text above.';
}

interface Measurement {
  key: string;
  label: string;
  backend: string;
  ctxPrimary: number;
  ctxFallback: number | null;
  ctxProbe: boolean;
  args: string;
  params: BenchParams;
  depthTokens: number;
}

export function buildBenchRecipe(
  fits: FitEstimate[],
  answers: WizardAnswers,
  hardware: HardwareSnapshot,
  facts: ModelFacts,
  model: string,
  budget: 'quick' | 'standard' | 'thorough',
  candidates: string[],
): BenchRecipe {
  const kv = answers.kv_cache_quant;
  const fitFor = (backend: string): FitEstimate | null =>
    fits.find(f => f.backend === backend) ?? null;
  const loadArgsFor = (backend: string): string =>
    backendLoadArgs(hardware, backend, fitFor(backend), facts, kv).join(' ').trim();
  const sanitize = (s: string): string => s.replace(/[^a-zA-Z0-9_]/g, '_');

  const measurements: Measurement[] = [];
  const ladderRungs = budget === 'thorough' ? [512, 1024, 2048, 4096, 8192] : [512, 2048, 8192];
  const mtpNs = budget === 'thorough' ? [1, 2, 3, 4, 5, 6] : [2, 3, 4];
  const doLadder = hardware.ram_is_vram && hardware.host_ram_gb >= 32;

  for (const backend of candidates) {
    const bkey = sanitize(backend);
    const recCtx = recommendedCtxSize(fitFor(backend)?.fitted_ctx ?? 0, facts.n_ctx_train, kv);
    const loadArgs = loadArgsFor(backend);

    measurements.push({
      key: `${bkey}_d0`,
      label: `Benchmarking ${backend} at depth 0`,
      backend,
      ctxPrimary: recCtx,
      ctxFallback: recCtx > FALLBACK_CTX ? FALLBACK_CTX : null,
      ctxProbe: true,
      args: loadArgs,
      params: { d: 0 },
      depthTokens: 0,
    });

    if (recCtx >= 8192) {
      const target = recCtx >= 32768 ? 30000 : Math.floor(0.8 * recCtx);
      const deepDepth = Math.min(target, recCtx - DEEP_GEN_MARGIN);
      if (deepDepth >= MIN_DEEP_DEPTH) {
        measurements.push({
          key: `${bkey}_d${deepDepth}`,
          label: `Benchmarking ${backend} at depth ${deepDepth}`,
          backend,
          ctxPrimary: recCtx,
          ctxFallback: null,
          ctxProbe: false,
          args: loadArgs,
          params: { d: deepDepth },
          depthTokens: deepDepth,
        });
      }
    }

    if (doLadder) {
      for (const r of ladderRungs) {
        measurements.push({
          key: `ladder_${bkey}_b${r}`,
          label: `Batch ladder -b ${r} on ${backend}`,
          backend,
          ctxPrimary: FALLBACK_CTX,
          ctxFallback: null,
          ctxProbe: false,
          args: `${loadArgs} -b ${r} -ub ${r}`.trim(),
          params: { ladder: true, b: r, ub: r, d: 0 },
          depthTokens: 0,
        });
      }
    }

    if (facts.has_mtp) {
      for (const n of mtpNs) {
        measurements.push({
          key: `mtp_${bkey}_n${n}`,
          label: `MTP draft sweep n=${n} on ${backend}`,
          backend,
          ctxPrimary: FALLBACK_CTX,
          ctxFallback: null,
          ctxProbe: false,
          args: `${loadArgs} --spec-type draft-mtp --spec-draft-n-max ${n} --spec-draft-p-min 0.75`.trim(),
          params: { spec_n: n, d: 0 },
          depthTokens: 0,
        });
      }
    }
  }

  const inputs: Record<string, unknown> = { model };
  const distinctDepths = new Set(measurements.map(m => m.depthTokens));
  for (const depth of distinctDepths) inputs[`primer_${depth}`] = depthPrompt(depth);

  const steps: Record<string, unknown>[] = [];
  const plan: BenchPlanEntry[] = [];
  const stepLabels: Record<string, string> = {};

  const firstStepOf = (index: number): string =>
    index < measurements.length ? `u_${measurements[index].key}` : 'done';

  const chatParams = (depth: number): Record<string, unknown> => ({
    model: '${inputs.model}',
    messages: [{ role: 'user', content: `\${inputs.primer_${depth}}` }],
    temperature: 0,
    max_completion_tokens: MAX_COMPLETION_TOKENS,
  });

  const loadParams = (backend: string, ctx: number, args: string): Record<string, unknown> => ({
    model: '${inputs.model}',
    llamacpp_backend: backend,
    ctx_size: ctx,
    llamacpp_args: args,
    merge_args: false,
    save_options: false,
  });

  measurements.forEach((m, index) => {
    const next = firstStepOf(index + 1);
    const ttftKey = `${m.key}_ttft`;
    const tpsKey = `${m.key}_tps`;
    const vramKey = `${m.key}_vram`;
    stepLabels[`u_${m.key}`] = m.label;
    stepLabels[`load_${m.key}`] = m.label;
    stepLabels[`chat_${m.key}`] = m.label;
    stepLabels[`stats_${m.key}`] = m.label;

    steps.push({ id: `u_${m.key}`, op: 'unload', on_fail: 'continue' });
    steps.push({
      id: `load_${m.key}`,
      op: 'load',
      params: loadParams(m.backend, m.ctxPrimary, m.args),
      on_fail: m.ctxFallback !== null ? `loadlo_${m.key}` : next,
    });
    steps.push({
      id: `chat_${m.key}`,
      op: 'chat',
      params: chatParams(m.depthTokens),
      extract: { [ttftKey]: 'timings.prompt_ms', [tpsKey]: 'timings.predicted_per_second' },
      on_fail: next,
    });
    steps.push({
      id: `stats_${m.key}`,
      op: 'system_stats',
      extract: { [vramKey]: 'vram_gb' },
      on_fail: next,
      on_done: next,
    });

    const loTtftKey = `${m.key}_lo_ttft`;
    const loTpsKey = `${m.key}_lo_tps`;
    const loVramKey = `${m.key}_lo_vram`;

    if (m.ctxFallback !== null) {
      stepLabels[`loadlo_${m.key}`] = `${m.label} (fallback ctx ${m.ctxFallback})`;
      steps.push({
        id: `loadlo_${m.key}`,
        op: 'load',
        params: loadParams(m.backend, m.ctxFallback, m.args),
        on_fail: next,
      });
      steps.push({
        id: `chatlo_${m.key}`,
        op: 'chat',
        params: chatParams(m.depthTokens),
        extract: { [loTtftKey]: 'timings.prompt_ms', [loTpsKey]: 'timings.predicted_per_second' },
        on_fail: next,
      });
      steps.push({
        id: `statslo_${m.key}`,
        op: 'system_stats',
        extract: { [loVramKey]: 'vram_gb' },
        on_fail: next,
        on_done: next,
      });
    }

    plan.push({
      label: m.label,
      backend: m.backend,
      ctx_size: m.ctxPrimary,
      llamacpp_args: m.args,
      params: m.params,
      ctx_probe: m.ctxProbe,
      ttft_key: ttftKey,
      tps_key: tpsKey,
      vram_key: vramKey,
      ...(m.ctxFallback !== null ? {
        fallback_ctx_size: m.ctxFallback,
        fallback_ttft_key: loTtftKey,
        fallback_tps_key: loTpsKey,
        fallback_vram_key: loVramKey,
      } : {}),
    });
  });

  steps.push({ id: 'done', op: 'unload' });
  stepLabels.done = 'Finishing up';

  return { steps, inputs, plan, step_labels: stepLabels };
}
