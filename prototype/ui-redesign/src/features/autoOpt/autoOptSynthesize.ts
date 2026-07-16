import {
  AutoOptRecommendation,
  AutoOptResult,
  BenchPoint,
  FitEstimate,
  HardwareSnapshot,
  ModelFacts,
  SamplingDefaults,
  WizardAnswers,
} from './autoOptTypes';

const CTX_STEPS = [262144, 131072, 98304, 65536, 49152, 32768, 24576, 16384, 12288, 8192, 6144, 4096, 2048];

export function roundDownCtx(ctx: number): number {
  for (const s of CTX_STEPS) {
    if (ctx >= s) return s;
  }
  return ctx >= 1024 ? Math.floor(ctx) : 1024;
}

export function roundUpCtx(ctx: number): number {
  for (let i = CTX_STEPS.length - 1; i >= 0; i--) {
    if (CTX_STEPS[i] >= ctx) return CTX_STEPS[i];
  }
  return CTX_STEPS[0];
}

export function kvQuantFactor(q: string): number {
  if (q === 'q8_0') return 0.5;
  if (q === 'q5_1') return 0.375;
  if (q === 'q4_0') return 0.28125;
  return 1.0;
}

export function recommendedCtxSize(fittedCtx: number, nCtxTrain: number, kv: string): number {
  let ctx = nCtxTrain > 0 ? nCtxTrain : 32768;
  if (fittedCtx > 0) {
    let fitCtx = fittedCtx;
    if (kv !== 'none') fitCtx = Math.floor(fitCtx / kvQuantFactor(kv));
    ctx = Math.min(ctx, fitCtx);
  }
  let size = roundDownCtx(ctx);
  if (nCtxTrain > 0 && size >= nCtxTrain) size = nCtxTrain;
  return size;
}

const COMPUTE_BASE_MIB = 512;
const MIN_OFFLOAD_CTX = 4096;
const DEFAULT_KV_BYTES_PER_TOKEN = 131072;

const NON_GPU_BACKENDS = new Set(['cpu', 'system']);

export const NO_GPU_BACKEND_ERROR =
  'AutoOpt requires an installed GPU backend (Vulkan, ROCm, or CUDA); '
  + 'this system reports only CPU inference.';

export function selectCandidates(hw: HardwareSnapshot, ans: WizardAnswers): string[] {
  const out: string[] = [];
  for (const b of hw.installed_backends) {
    if (NON_GPU_BACKENDS.has(b)) continue;
    if (ans.backends_to_consider.length > 0 && !ans.backends_to_consider.includes(b)) continue;
    out.push(b);
  }
  return out;
}

export function availableMib(hw: HardwareSnapshot): number {
  if (hw.ram_is_vram) return Math.max(1024, hw.host_ram_gb * 0.9 * 1024);
  const gpuGb = hw.gpus.reduce((sum, g) => sum + (g.vram_gb || 0), 0);
  if (gpuGb > 0) return gpuGb * 0.92 * 1024;
  return Math.max(1024, hw.host_ram_gb * 0.7 * 1024);
}

function backendMemoryVendor(hw: HardwareSnapshot, backend: string): 'nvidia' | 'amd' | 'apple' | null {
  if (backend === 'cuda') return 'nvidia';
  if (backend.startsWith('rocm')) return 'amd';
  if (backend === 'metal') return 'apple';
  if (backend === 'vulkan') {
    if (hw.gpus.some(g => g.vendor === 'amd')) return 'amd';
    if (hw.gpus.some(g => g.vendor === 'nvidia')) return 'nvidia';
    return null;
  }
  return null;
}

export function availableMibForBackend(hw: HardwareSnapshot, backend: string): number {
  const vendor = backendMemoryVendor(hw, backend);
  if (vendor === 'apple') return Math.max(1024, hw.host_ram_gb * 0.9 * 1024);
  if (vendor === 'amd' && hw.ram_is_vram) return Math.max(1024, hw.host_ram_gb * 0.9 * 1024);
  if (vendor) {
    const groupGb = hw.gpus
      .filter(g => g.vendor === vendor)
      .reduce((sum, g) => sum + (g.vram_gb || 0), 0);
    if (groupGb > 0) return groupGb * 0.92 * 1024;
  }
  return Math.max(1024, hw.host_ram_gb * 0.7 * 1024);
}

export interface FitInputs {
  backend: string;
  availableMib: number;
  weightsMib: number;
  kvBytesPerToken: number;
  blockCount: number;
  isMoe: boolean;
  nCtxTrain: number;
  degraded: boolean;
}

export function computeFitEstimate(input: FitInputs): FitEstimate {
  const kvPerTokenMib = (input.kvBytesPerToken || DEFAULT_KV_BYTES_PER_TOKEN) / (1024 * 1024);
  const compute = COMPUTE_BASE_MIB;
  const weights = Math.max(0, input.weightsMib);
  const available = Math.max(1, input.availableMib);
  const trained = input.nCtxTrain > 0 ? input.nCtxTrain : 32768;

  const base: FitEstimate = {
    backend: input.backend,
    fits_fully: false,
    fitted_ctx: 0,
    fitted_ngl: -1,
    fitted_ncmoe: 0,
    weights_mib: Math.round(weights),
    kv_mib: 0,
    compute_mib: compute,
    total_mib: 0,
    available_mib: Math.round(available),
    degraded: input.degraded,
    ok: true,
  };

  const minKvMib = kvPerTokenMib * MIN_OFFLOAD_CTX;
  if (weights + compute + minKvMib > available) {
    base.fits_fully = false;
    base.fitted_ctx = MIN_OFFLOAD_CTX;
    const perLayer = weights / Math.max(1, input.blockCount);
    const weightsBudget = available - compute - minKvMib;
    const layersOnGpu = Math.max(0, Math.min(input.blockCount, Math.floor(weightsBudget / Math.max(1, perLayer))));
    if (input.isMoe) {
      base.fitted_ngl = -1;
      base.fitted_ncmoe = Math.max(1, Math.min(input.blockCount, input.blockCount - layersOnGpu));
    } else {
      base.fitted_ngl = layersOnGpu;
      base.fitted_ncmoe = 0;
    }
    base.kv_mib = Math.round(minKvMib);
    base.total_mib = Math.round(available);
    return base;
  }

  base.fits_fully = true;
  const kvFullMib = kvPerTokenMib * trained;
  if (weights + compute + kvFullMib <= available) {
    base.fitted_ctx = 0;
    base.kv_mib = Math.round(kvFullMib);
    base.total_mib = Math.round(weights + compute + kvFullMib);
    return base;
  }
  const kvBudget = available - weights - compute;
  base.fitted_ctx = Math.max(MIN_OFFLOAD_CTX, Math.floor(kvBudget / kvPerTokenMib));
  base.kv_mib = Math.round(kvBudget);
  base.total_mib = Math.round(available);
  return base;
}

export function fitTotalGb(fit: FitEstimate | null | undefined): number {
  return fit ? fit.total_mib / 1024 : 0;
}

function dualIdenticalNvidia(hw: HardwareSnapshot): boolean {
  for (let i = 0; i < hw.gpus.length; i++) {
    for (let j = i + 1; j < hw.gpus.length; j++) {
      if (hw.gpus[i].vendor === 'nvidia' && hw.gpus[i].family === hw.gpus[j].family) return true;
    }
  }
  return false;
}

export function backendLoadArgs(
  hw: HardwareSnapshot,
  backend: string,
  fit: FitEstimate | null,
  mf: ModelFacts,
  kv: string,
): string[] {
  const args: string[] = [];
  if (backend.startsWith('rocm')) args.push('--direct-io');
  if (fit && !fit.fits_fully) {
    if (mf.is_moe && fit.fitted_ncmoe > 0) args.push(`--n-cpu-moe ${fit.fitted_ncmoe}`);
    else if (mf.is_moe) args.push('--cpu-moe');
    else if (fit.fitted_ngl >= 0) args.push(`-ngl ${fit.fitted_ngl}`);
  }
  if (kv !== 'none') args.push(`-ctk ${kv} -ctv ${kv}`);
  if (backend === 'cuda' && dualIdenticalNvidia(hw)) args.push('--split-mode tensor');
  return args;
}

function isDeepPoint(p: BenchPoint): boolean {
  return (p.params?.d ?? 0) > 0 && p.params?.ladder !== true && p.params?.spec_n === undefined;
}

function deepRunFailed(bench: BenchPoint[], backend: string): boolean {
  const deep = bench.filter(p => p.backend === backend && isDeepPoint(p));
  return deep.length > 0 && !deep.some(p => p.ok);
}

function representative(bench: BenchPoint[], backend: string): BenchPoint | null {
  let d0: BenchPoint | null = null;
  let deep: BenchPoint | null = null;
  for (const p of bench) {
    if (!p.ok || p.backend !== backend) continue;
    if (p.params?.ladder === true || p.params?.spec_n !== undefined) continue;
    if ((p.params?.d ?? 0) > 0) deep = p;
    else d0 = p;
  }
  return deep || d0;
}

export function benchScore(tps: number, ttftMs: number, tpsMax: number, ttftMin: number): number {
  const normTps = tpsMax > 0 ? Math.max(0, tps) / tpsMax : 0;
  const normTtft = (ttftMs > 0 && ttftMin > 0) ? ttftMin / ttftMs : 0;
  return 0.7 * normTps + 0.3 * normTtft;
}

function pickBackend(bench: BenchPoint[], candidates: string[]): { backend: string; rep: BenchPoint | null } {
  const eligible = candidates.filter(c => !deepRunFailed(bench, c));
  const pool = eligible.length > 0 ? eligible : candidates;
  const reps: Array<{ backend: string; rep: BenchPoint }> = [];
  for (const c of pool) {
    const rep = representative(bench, c);
    if (rep) reps.push({ backend: c, rep });
  }
  if (reps.length === 0) return { backend: '', rep: null };
  const tpsMax = Math.max(...reps.map(r => r.rep.tps), 0);
  const ttfts = reps.map(r => r.rep.ttft_ms).filter(v => v > 0);
  const ttftMin = ttfts.length ? Math.min(...ttfts) : 0;
  let best = reps[0];
  let bestScore = -1;
  for (const r of reps) {
    const score = benchScore(r.rep.tps, r.rep.ttft_ms, tpsMax, ttftMin);
    if (score > bestScore) {
      bestScore = score;
      best = r;
    }
  }
  return { backend: best.backend, rep: best.rep };
}

export function pickBenchWinner(bench: BenchPoint[], candidates: string[]): string {
  return pickBackend(bench, candidates).backend;
}

function fmt1(v: number): string {
  return v.toFixed(1);
}

export function checkpointRepoId(checkpoint: string): string {
  const marker = 'huggingface.co/';
  const pos = checkpoint.indexOf(marker);
  let id = pos === -1 ? checkpoint : checkpoint.slice(pos + marker.length);
  while (id.endsWith('/')) id = id.slice(0, -1);
  const colon = id.indexOf(':');
  if (colon !== -1) id = id.slice(0, colon);
  return id.includes('/') ? id : '';
}

export function synthesize(
  hw: HardwareSnapshot,
  mf: ModelFacts,
  ans: WizardAnswers,
  fits: FitEstimate[],
  bench: BenchPoint[],
  sampling?: SamplingDefaults,
): AutoOptResult {
  const p: AutoOptRecommendation = {
    label: 'Recommended',
    llamacpp_backend: '',
    ctx_size: -1,
    llamacpp_args: '',
    rationale: [],
  };
  const result: AutoOptResult = { primary: p, alternatives: [] };
  const args: string[] = [];

  const candidates = selectCandidates(hw, ans);
  if (candidates.length === 0) throw new Error(NO_GPU_BACKEND_ERROR);

  let backend = candidates[0];
  if (candidates.length > 1) {
    const { backend: measured, rep } = pickBackend(bench, candidates);
    if (measured) backend = measured;
    if (rep) {
      const others = candidates.filter(c => c !== backend).join(', ');
      p.rationale.push(`${backend} chosen over ${others}: best measured throughput/latency balance `
        + `on this model (${fmt1(rep.tps)} tok/s decode, ${fmt1(rep.ttft_ms)} ms to first token`
        + `${rep.params?.d ? ` at depth ${rep.params.d}` : ''}).`);
    } else {
      p.rationale.push(`${backend} chosen (first installed GPU backend; enable a benchmark `
        + 'budget to measure alternatives).');
    }
  }
  p.llamacpp_backend = backend;
  const isCuda = backend === 'cuda';
  const fit = fits.find(f => f.ok && f.backend === backend) || fits.find(f => f.ok) || null;
  const kv = ans.kv_cache_quant;

  args.push(...backendLoadArgs(hw, backend, fit, mf, kv));

  if (backend.startsWith('rocm')) {
    p.rationale.push('--direct-io: works around broken mmap on ROCm with faster cold loads '
      + 'than --no-mmap.');
  }

  let usedCpuMoe = false;
  if (fit && !fit.fits_fully) {
    if (mf.is_moe && fit.fitted_ncmoe > 0) {
      usedCpuMoe = true;
      p.rationale.push(`Model exceeds GPU memory: expert tensors of the first ${fit.fitted_ncmoe} `
        + 'layers stay on CPU (--n-cpu-moe) — attention and shared tensors keep GPU speed.');
    } else if (mf.is_moe) {
      usedCpuMoe = true;
      p.rationale.push('Model exceeds GPU memory: all expert tensors on CPU (--cpu-moe); '
        + 'the GPU keeps the non-expert layers.');
    } else if (fit.fitted_ngl >= 0) {
      p.rationale.push(`Model exceeds GPU memory: ${fit.fitted_ngl} of ${mf.block_count} layers `
        + 'offloaded to GPU (-ngl), the rest run on CPU (expect reduced speed).');
    }
  }

  if (kv !== 'none') {
    const note = kv === 'q8_0'
      ? 'roughly doubles usable context with negligible quality loss'
      : (kv === 'q5_1'
        ? 'about 2.7x context capacity with a slight quality cost'
        : 'about 3.5x context capacity; quality measurably degrades on very long contexts');
    p.rationale.push(`KV cache quantized to ${kv}: ${note}.`);
  }

  if (isCuda && dualIdenticalNvidia(hw)) {
    p.rationale.push('Two or more identical NVIDIA GPUs: tensor parallelism speeds up decode '
      + '(prefill gets slightly slower).');
  }
  let recCtx = recommendedCtxSize(fit ? fit.fitted_ctx : 0, mf.n_ctx_train, kv);
  let loadCeilingHit = false;
  for (const bp of bench) {
    if (!bp.ok || bp.backend !== backend) continue;
    if (typeof bp.max_loaded_ctx === 'number' && bp.max_loaded_ctx > 0 && bp.max_loaded_ctx < recCtx) {
      recCtx = bp.max_loaded_ctx;
      loadCeilingHit = true;
    }
  }
  p.ctx_size = recCtx;
  if (loadCeilingHit) {
    p.rationale.push(`Context ${p.ctx_size}: the heuristic estimate did not load on this `
      + 'hardware during benchmarking; capped to the largest context that actually loaded.');
  } else if (mf.n_ctx_train > 0 && p.ctx_size >= mf.n_ctx_train) {
    p.ctx_size = mf.n_ctx_train;
    p.rationale.push(`Context ${p.ctx_size}: the model's full trained window fits.`);
  } else {
    p.rationale.push(`Context ${p.ctx_size}: the largest standard size that fits in memory`
      + (kv !== 'none' ? ' with the quantized KV cache' : '')
      + (fit?.degraded ? ' (coarse estimate — model metadata was unavailable).' : '.'));
  }

  let headroom = ans.ram_headroom;
  if (mf.is_hybrid_or_recurrent && headroom === 'disabled') {
    headroom = 'minimal';
    p.rationale.push('Prompt-cache checkpoints kept at minimal instead of disabled: this '
      + 'architecture (hybrid/recurrent) must re-process the whole prompt on any cache miss.');
  }
  if (headroom === 'reduced') args.push('--cache-ram 4096 -ctxcp 16');
  else if (headroom === 'minimal') args.push('--cache-ram 2048 -ctxcp 8');
  else if (headroom === 'disabled') args.push('--cache-ram 0 -ctxcp 0');
  if (headroom !== 'normal') {
    p.rationale.push(`Prompt-cache RAM capped (${headroom})`
      + (hw.ram_is_vram
        ? ' — on this machine system RAM and GPU memory share one pool.'
        : ' to keep system RAM free.'));
  }

  if (ans.parallel && ans.slots > 1) {
    const np = Math.min(Math.max(ans.slots, 2), 8);
    if (ans.dedicated_slots) {
      args.push(`-np ${np} -no-kvu`);
      p.rationale.push(`${np} parallel slots with dedicated context: each request is `
        + `guaranteed ${Math.floor(p.ctx_size / np)} tokens (${p.ctx_size} / ${np}).`);
    } else {
      args.push(`-np ${np} -kvu`);
      p.rationale.push(`${np} parallel slots sharing one context pool: long requests can use `
        + 'most of the window, but concurrent long requests race for it.');
    }
  }

  args.push('--spec-default');
  p.rationale.push('--spec-default: n-gram speculative decoding is effectively free.');
  if (mf.has_mtp) {
    let bestN = 3;
    let bestTps = -1;
    for (const bp of bench) {
      if (!bp.ok || bp.params?.spec_n === undefined) continue;
      if (bp.tps > bestTps) {
        bestTps = bp.tps;
        bestN = bp.params.spec_n;
      }
    }
    args.push(`--spec-type draft-mtp --spec-draft-n-max ${bestN}`);
    p.rationale.push(bestTps > 0
      ? `MTP draft length ${bestN} measured fastest on this machine (${fmt1(bestTps)} tok/s).`
      : 'Model has MTP heads: draft-based speculative decoding enabled (default draft length 3).');
  }

  const bigIgpu = hw.ram_is_vram && hw.host_ram_gb >= 32;
  {
    let bestB = 0;
    let bestTtft = Infinity;
    let baseTtft = -1;
    for (const bp of bench) {
      if (!bp.ok || bp.params?.ladder !== true || bp.backend !== backend) continue;
      const b = bp.params.b ?? 0;
      if (b === 512) baseTtft = bp.ttft_ms;
      if (bp.ttft_ms > 0 && bp.ttft_ms < bestTtft) {
        bestTtft = bp.ttft_ms;
        bestB = b;
      }
    }
    if (bestB > 512 && baseTtft > 0 && bestTtft > 0 && baseTtft / bestTtft > 1.05) {
      args.push(`-b ${bestB} -ub ${bestB}`);
      p.rationale.push(`Batch size ${bestB} measured ${fmt1((baseTtft / bestTtft - 1) * 100)}% `
        + 'faster prefill than the default 512 (costs some memory for compute buffers).');
    } else if (bench.length === 0 && bigIgpu) {
      args.push('-b 2048 -ub 2048');
      p.rationale.push('Batch size 2048: unified-memory machines with ample RAM prefill much '
        + 'faster with larger batches (heuristic — run a benchmark pass to measure).');
    }
  }

  result.sampling_defaults = sampling;
  if (sampling) {
    p.rationale.push('Sampling defaults (temperature/top-p/top-k) taken from the base model\'s '
      + `generation_config (${sampling.source}).`);
  }

  const join = (v: string[]) => v.join(' ');
  p.llamacpp_args = join(args);
  {
    const rep = pickBackend(bench, [backend]).rep;
    const expected: AutoOptRecommendation['expected'] = {};
    if (fit) expected.vram_gb = Number(fitTotalGb(fit).toFixed(2));
    if (rep) {
      if (rep.tps > 0) expected.tps = rep.tps;
      if (rep.ttft_ms > 0) expected.ttft_ms = rep.ttft_ms;
    }
    if (Object.keys(expected).length) p.expected = expected;
  }

  if (kv !== 'none') {
    const alt: AutoOptRecommendation = {
      ...p,
      label: 'Maximum quality',
      tradeoff: 'smaller context window',
      rationale: ['Unquantized f16 KV cache: no quality risk; context shrinks to what fits.'],
      llamacpp_args: join(args.filter(a => !a.startsWith('-ctk'))),
      ctx_size: roundDownCtx(Math.max(Math.floor(p.ctx_size * kvQuantFactor(kv)), 4096)),
      expected: undefined,
    };
    result.alternatives.push(alt);
  }
  if (kv !== 'q4_0') {
    const a2: string[] = [];
    let replaced = false;
    for (const a of args) {
      if (a.startsWith('-ctk')) {
        a2.push('-ctk q4_0 -ctv q4_0');
        replaced = true;
      } else {
        a2.push(a);
      }
    }
    if (!replaced) a2.unshift('-ctk q4_0 -ctv q4_0');
    const altCtx = Math.floor(p.ctx_size * kvQuantFactor(kv) / kvQuantFactor('q4_0'));
    const alt: AutoOptRecommendation = {
      ...p,
      label: 'Maximum context',
      tradeoff: 'quality degrades on very long contexts',
      rationale: ['q4_0 KV cache: about 3.5x the context capacity of f16; noticeable quality '
        + 'loss past ~32k tokens of active context.'],
      llamacpp_args: join(a2),
      ctx_size: roundDownCtx(Math.min(altCtx, mf.n_ctx_train > 0 ? mf.n_ctx_train : altCtx)),
      expected: undefined,
    };
    result.alternatives.push(alt);
  }
  if (usedCpuMoe) {
    const alt: AutoOptRecommendation = {
      ...p,
      label: 'Conservative offload',
      tradeoff: 'slower, but immune to memory pressure',
      rationale: ['All expert tensors on CPU (--cpu-moe): the safest fit when other '
        + 'applications compete for GPU memory.'],
      llamacpp_args: join(args.map(a => a.startsWith('--n-cpu-moe') ? '--cpu-moe' : a)),
      expected: undefined,
    };
    result.alternatives.push(alt);
  }

  return result;
}
