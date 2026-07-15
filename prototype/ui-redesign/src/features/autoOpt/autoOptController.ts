import api, { JobRecord } from '../../api';
import { buildBenchRecipe } from './autoOptRecipe';
import {
  NO_GPU_BACKEND_ERROR,
  availableMib,
  checkpointRepoId,
  computeFitEstimate,
  selectCandidates,
  synthesize,
} from './autoOptSynthesize';
import {
  AutoOptResult,
  AutoOptRunRecord,
  AutoOptStageStatus,
  AutoOptStartRequest,
  BenchPlanEntry,
  BenchPoint,
  FitEstimate,
  HardwareSnapshot,
  ModelFacts,
  SamplingDefaults,
  SynthInputs,
  wizardAnswersFrom,
} from './autoOptTypes';

export const AUTOOPT_STAGES = [
  'snapshot', 'model_facts', 'hf_metadata', 'fit_estimate', 'bench_job', 'synthesize',
] as const;

const JOB_POLL_MS = 1500;

export interface ControllerCallbacks {
  stage(name: string, status: AutoOptStageStatus, patch?: { duration_ms?: number; error?: string; data?: Record<string, unknown> }): void;
  progress(detail: string): void;
  fit(estimate: FitEstimate): void;
  bench(points: BenchPoint[]): void;
  jobCreated(jobId: string, synthInputs: SynthInputs): void;
}

export interface ControllerOutcome {
  result: AutoOptResult;
  summary: string;
}

const RECURRENT_ARCHS = new Set([
  'mamba', 'mamba2', 'rwkv6', 'rwkv6qwen2', 'rwkv7', 'arwkv7',
  'jamba', 'falcon-h1', 'granitehybrid', 'nemotron-h', 'lfm2', 'plamo2',
]);

function isAbort(err: unknown): boolean {
  return (err as { name?: string } | null)?.name === 'AbortError';
}

function errorMessage(err: unknown): string {
  if (err instanceof Error) return err.message;
  return String(err || 'Unknown error');
}

function abortError(): Error {
  return Object.assign(new Error('cancelled by user'), { name: 'AbortError' });
}

function sleep(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) return reject(abortError());
    const timer = setTimeout(resolve, ms);
    signal.addEventListener('abort', () => { clearTimeout(timer); reject(abortError()); }, { once: true });
  });
}

function parseGb(value: unknown): number {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string') {
    const match = /([\d.]+)/.exec(value);
    const n = match ? Number(match[1]) : NaN;
    if (Number.isFinite(n)) return value.toLowerCase().includes('mb') ? n / 1024 : n;
  }
  return 0;
}

function snapshotFromSystemInfo(info: Record<string, unknown>): HardwareSnapshot {
  const hw: HardwareSnapshot = {
    gpus: [],
    has_igpu: false,
    ram_is_vram: false,
    host_ram_gb: parseGb(info['Physical Memory']),
    installed_backends: [],
    os: String(info['OS Version'] || ''),
  };
  const devices = (info.devices || {}) as Record<string, unknown>;
  let sharedMemoryGpu = false;
  const readGpus = (list: unknown, vendor: string) => {
    if (!Array.isArray(list)) return;
    for (const raw of list) {
      const g = raw as Record<string, unknown>;
      if (g?.available !== true) continue;
      const virtualGb = Number(g.virtual_mem_gb);
      const vramGb = Number(g.vram_gb) || 0;
      const shared = Number.isFinite(virtualGb) && virtualGb > vramGb;
      hw.gpus.push({
        vendor,
        name: String(g.name || ''),
        family: String(g.family || ''),
        vram_gb: shared ? virtualGb : vramGb,
      });
      if (shared) sharedMemoryGpu = true;
    }
  };
  readGpus(devices.amd_gpu, 'amd');
  readGpus(devices.nvidia_gpu, 'nvidia');
  hw.has_igpu = sharedMemoryGpu;
  hw.ram_is_vram = sharedMemoryGpu && hw.gpus.length === 1;

  const recipes = (info.recipes || {}) as Record<string, { backends?: Record<string, { state?: unknown }> }>;
  const backends = recipes.llamacpp?.backends || {};
  for (const [name, backendInfo] of Object.entries(backends)) {
    const state = String(backendInfo?.state || '');
    if (state === 'installed' || state === 'update_available') hw.installed_backends.push(name);
  }
  return hw;
}

function factsFromModelDetail(detail: Record<string, unknown>): ModelFacts {
  const metadata = (detail.metadata && typeof detail.metadata === 'object' && !Array.isArray(detail.metadata))
    ? detail.metadata as Record<string, unknown>
    : null;
  const labels = Array.isArray(detail.labels) ? detail.labels.map(l => String(l).toLowerCase()) : [];
  const architecture = String(metadata?.architecture || '');
  const fullAttentionInterval = Number(metadata?.full_attention_interval) || 0;
  const expertCount = Number(metadata?.expert_count) || 0;
  const sizeGb = Number(detail.size) || Number(metadata?.file_size_gb) || 0;
  return {
    architecture,
    block_count: Number(metadata?.block_count) || 0,
    expert_count: expertCount,
    full_attention_interval: fullAttentionInterval,
    swa_layer_count: Number(metadata?.swa_layer_count) || 0,
    n_ctx_train: Number(metadata?.context_length) || Number(detail.max_context_window) || 0,
    kv_bytes_per_token: Number(metadata?.kv_bytes_per_token) || 0,
    weights_mib: sizeGb * 1024,
    is_moe: expertCount > 1,
    is_hybrid_or_recurrent: fullAttentionInterval > 0 || RECURRENT_ARCHS.has(architecture),
    has_mtp: labels.includes('mtp'),
    base_model_repo: String(metadata?.base_model_repo || ''),
    checkpoint: String(detail.checkpoint || ''),
    metadata_present: !!metadata,
  };
}

function hfRepoIdFromUrl(urlOrId: string): string {
  const marker = 'huggingface.co/';
  const pos = urlOrId.indexOf(marker);
  let id = pos === -1 ? urlOrId : urlOrId.slice(pos + marker.length);
  while (id.endsWith('/')) id = id.slice(0, -1);
  return id;
}

async function fetchHfJson(url: string, signal: AbortSignal): Promise<Record<string, unknown> | null> {
  const resp = await fetch(url, { signal });
  if (!resp.ok) return null;
  try {
    return await resp.json() as Record<string, unknown>;
  } catch {
    return null;
  }
}

async function resolveBaseModelRepo(ggufBaseModelRepo: string, checkpoint: string, signal: AbortSignal): Promise<string> {
  if (ggufBaseModelRepo) return hfRepoIdFromUrl(ggufBaseModelRepo);
  const repo = checkpointRepoId(checkpoint);
  if (!repo) return '';
  const card = await fetchHfJson(`https://huggingface.co/api/models/${repo}`, signal);
  const baseModel = (card?.cardData as Record<string, unknown> | undefined)?.base_model;
  if (typeof baseModel === 'string') return baseModel;
  if (Array.isArray(baseModel) && typeof baseModel[0] === 'string') return baseModel[0];
  return '';
}

function samplingFromConfig(gc: Record<string, unknown>, base: string, file: string): SamplingDefaults {
  const sd: SamplingDefaults = { source: `hf:${base}/${file}` };
  if (typeof gc.temperature === 'number') sd.temperature = gc.temperature;
  if (typeof gc.top_p === 'number') sd.top_p = gc.top_p;
  if (typeof gc.min_p === 'number') sd.min_p = gc.min_p;
  if (typeof gc.top_k === 'number') sd.top_k = gc.top_k;
  return sd;
}

function hasSamplingValues(sd: SamplingDefaults): boolean {
  return sd.temperature !== undefined || sd.top_p !== undefined
    || sd.min_p !== undefined || sd.top_k !== undefined;
}

async function fetchSamplingDefaults(base: string, signal: AbortSignal): Promise<SamplingDefaults | null> {
  const gc = await fetchHfJson(`https://huggingface.co/${base}/resolve/main/generation_config.json`, signal);
  if (gc) {
    const sd = samplingFromConfig(gc, base, 'generation_config.json');
    if (hasSamplingValues(sd)) return sd;
  }
  const cfg = await fetchHfJson(`https://huggingface.co/${base}/resolve/main/config.json`, signal);
  if (cfg) {
    const nested = cfg.generation_config;
    if (nested && typeof nested === 'object' && !Array.isArray(nested)) {
      const sd = samplingFromConfig(nested as Record<string, unknown>, base, 'config.json');
      if (hasSamplingValues(sd)) return sd;
    }
    const top = samplingFromConfig(cfg, base, 'config.json');
    if (hasSamplingValues(top)) return top;
  }
  return null;
}

function benchPointsFromContext(plan: BenchPlanEntry[], context: Record<string, unknown>): BenchPoint[] {
  const readMetrics = (ttftKey?: string, tpsKey?: string, vramKey?: string) => {
    const ttft = ttftKey ? Number(context[ttftKey]) : NaN;
    const tps = tpsKey ? Number(context[tpsKey]) : NaN;
    const vram = vramKey ? Number(context[vramKey]) : NaN;
    const ok = (Number.isFinite(ttft) && ttft > 0) || (Number.isFinite(tps) && tps > 0);
    return { ttft, tps, vram, ok };
  };
  return plan.map(entry => {
    const primary = readMetrics(entry.ttft_key, entry.tps_key, entry.vram_key);
    const usedPrimary = primary.ok;
    const fell = !usedPrimary && entry.fallback_ttft_key !== undefined
      ? readMetrics(entry.fallback_ttft_key, entry.fallback_tps_key, entry.fallback_vram_key)
      : null;
    const m = usedPrimary ? primary : (fell && fell.ok ? fell : primary);
    const ok = m.ok;
    const loadedCtx = usedPrimary
      ? entry.ctx_size
      : (fell && fell.ok ? (entry.fallback_ctx_size ?? entry.ctx_size) : entry.ctx_size);
    return {
      backend: entry.backend,
      label: entry.label,
      ctx_size: loadedCtx,
      llamacpp_args: entry.llamacpp_args,
      params: entry.params,
      ttft_ms: ok && Number.isFinite(m.ttft) ? m.ttft : 0,
      tps: ok && Number.isFinite(m.tps) ? m.tps : 0,
      vram_gb: Number.isFinite(m.vram) && m.vram > 0 ? m.vram : -1,
      ok,
      ...(ok ? {} : { error: 'not measured (config failed or skipped)' }),
      ...(ok && entry.ctx_probe ? { max_loaded_ctx: loadedCtx } : {}),
    };
  });
}

function jobErrorMessage(job: JobRecord): string {
  const failed = job.steps.find(step => step.status === 'failed' && step.error);
  return failed?.error || 'the benchmark job failed on the server';
}

function isTerminal(status: JobRecord['status']): boolean {
  return status === 'completed' || status === 'failed' || status === 'interrupted';
}

async function pollBenchJob(
  jobId: string,
  plan: BenchPlanEntry[],
  stepLabels: Record<string, string>,
  cb: ControllerCallbacks,
  signal: AbortSignal,
): Promise<JobRecord> {
  while (true) {
    if (signal.aborted) {
      throw abortError();
    }
    let job: JobRecord;
    try {
      job = await api.getJob(jobId, signal);
    } catch (err) {
      if (isAbort(err)) {
        throw abortError();
      }
      throw err;
    }
    const running = job.steps.find(step => step.status === 'running');
    if (running) cb.progress(stepLabels[running.id] || `${running.op} · ${running.id}`);
    cb.bench(benchPointsFromContext(plan, job.context || {}));
    if (isTerminal(job.status)) return job;
    await sleep(JOB_POLL_MS, signal);
  }
}

async function finishBenchJob(
  jobId: string,
  synth: SynthInputs,
  request: AutoOptStartRequest,
  cb: ControllerCallbacks,
  signal: AbortSignal,
): Promise<ControllerOutcome> {
  const answers = wizardAnswersFrom(request.answers);
  const job = await pollBenchJob(jobId, synth.plan, synth.step_labels, cb, signal);

  if (job.status === 'interrupted') throw abortError();
  if (job.status === 'failed') {
    cb.stage('bench_job', 'failed', { error: jobErrorMessage(job) });
    throw new Error(jobErrorMessage(job));
  }
  const bench = benchPointsFromContext(synth.plan, job.context || {});
  cb.bench(bench);
  if (!bench.some(point => point.ok)) {
    cb.stage('bench_job', 'failed', { error: 'no configuration produced a measurement' });
    throw new Error('the benchmark job produced no usable measurements');
  }
  cb.stage('bench_job', 'completed');

  let result: AutoOptResult | null = null;
  cb.stage('synthesize', 'running');
  try {
    result = synthesize(synth.hardware, synth.facts, answers, synth.fits, bench, synth.sampling);
  } catch (err) {
    cb.stage('synthesize', 'failed', { error: errorMessage(err) });
    throw new Error('preset synthesis failed');
  }
  cb.stage('synthesize', 'completed');

  const summary = `${result.primary.llamacpp_backend} · ctx ${result.primary.ctx_size}`
    + (result.primary.llamacpp_args ? ` · ${result.primary.llamacpp_args}` : '');
  return { result, summary };
}

export async function executeAutoOptRun(
  request: AutoOptStartRequest,
  signal: AbortSignal,
  cb: ControllerCallbacks,
): Promise<ControllerOutcome> {
  const answers = wizardAnswersFrom(request.answers);
  const withBench = request.budget !== 'quick';

  const throwIfAborted = () => { if (signal.aborted) throw abortError(); };

  const runStage = async (name: string, body: () => Promise<void>): Promise<boolean> => {
    throwIfAborted();
    cb.stage(name, 'running');
    const t0 = performance.now();
    let error = '';
    let ok = true;
    try {
      await body();
    } catch (err) {
      if (isAbort(err)) {
        cb.stage(name, 'failed', { duration_ms: Math.round(performance.now() - t0) });
        throw err;
      }
      ok = false;
      error = errorMessage(err);
    }
    cb.stage(name, ok ? 'completed' : 'failed', {
      duration_ms: Math.round(performance.now() - t0),
      ...(error ? { error } : {}),
    });
    return ok;
  };

  let hw: HardwareSnapshot | null = null;
  let facts: ModelFacts | null = null;
  let sampling: SamplingDefaults | undefined;
  const fits: FitEstimate[] = [];

  if (!await runStage('snapshot', async () => {
    hw = snapshotFromSystemInfo(await api.systemInfo());
  })) {
    throw new Error('could not inspect system hardware');
  }

  if (!await runStage('model_facts', async () => {
    const detail = await api.modelDetail(request.model) as unknown as Record<string, unknown>;
    if (String(detail.recipe || '') !== 'llamacpp') throw new Error('model recipe is not llamacpp');
    if (detail.downloaded !== true) throw new Error('model is not downloaded');
    facts = factsFromModelDetail(detail);
    if (!facts.metadata_present) {
      cb.stage('model_facts', 'running', { data: { note: 'model metadata unavailable — using coarse fit heuristics' } });
    }
  })) {
    throw new Error('could not read model metadata');
  }
  const hardware = hw!;
  const modelFacts = facts!;

  if (answers.allow_network) {
    await runStage('hf_metadata', async () => {
      const base = await resolveBaseModelRepo(modelFacts.base_model_repo, modelFacts.checkpoint, signal);
      if (!base) {
        cb.stage('hf_metadata', 'running', { data: { note: 'no sampling defaults published (base model not resolvable)' } });
        return;
      }
      const found = await fetchSamplingDefaults(base, signal);
      if (found) {
        sampling = found;
        cb.stage('hf_metadata', 'running', { data: { base_model: base, sampling_source: found.source } });
      } else {
        cb.stage('hf_metadata', 'running', { data: { base_model: base, note: 'no sampling defaults published' } });
      }
    });
  } else {
    cb.stage('hf_metadata', 'skipped');
  }
  throwIfAborted();

  const candidates = selectCandidates(hardware, answers);
  if (candidates.length === 0) throw new Error(NO_GPU_BACKEND_ERROR);

  await runStage('fit_estimate', async () => {
    const avail = availableMib(hardware);
    for (const backend of candidates) {
      const fit = computeFitEstimate({
        backend,
        availableMib: avail,
        weightsMib: modelFacts.weights_mib,
        kvBytesPerToken: modelFacts.kv_bytes_per_token,
        blockCount: modelFacts.block_count,
        isMoe: modelFacts.is_moe,
        nCtxTrain: modelFacts.n_ctx_train,
        degraded: !modelFacts.metadata_present,
      });
      cb.fit(fit);
      fits.push(fit);
    }
    if (fits.length === 0) throw new Error('no GPU backend available to fit against');
  });
  throwIfAborted();

  const primaryFit = fits.find(f => f.ok && f.backend === candidates[0]) || fits[0] || null;

  if (!withBench) {
    cb.stage('bench_job', 'skipped');
    let result: AutoOptResult | null = null;
    if (!await runStage('synthesize', async () => {
      result = synthesize(hardware, modelFacts, answers, fits, [], sampling);
    })) {
      throw new Error('preset synthesis failed');
    }
    result!.primary.rationale.push(
      'Fast Scan: configuration derived from heuristics and not load-validated. '
      + 'Run a Benchmark to confirm it loads on this hardware.');
    const summary = `${result!.primary.llamacpp_backend} · ctx ${result!.primary.ctx_size}`
      + (result!.primary.llamacpp_args ? ` · ${result!.primary.llamacpp_args}` : '');
    return { result: result!, summary };
  }

  const recipe = buildBenchRecipe(primaryFit, answers, hardware, modelFacts, request.model, request.budget, candidates);
  const synth: SynthInputs = {
    hardware, facts: modelFacts, fits, sampling,
    plan: recipe.plan, step_labels: recipe.step_labels,
  };

  cb.stage('bench_job', 'running');
  throwIfAborted();
  const { id: jobId } = await api.createJob(`autoopt-${request.model}`, recipe.steps, recipe.inputs);
  cb.jobCreated(jobId, synth);

  return finishBenchJob(jobId, synth, request, cb, signal);
}

export async function resumeAutoOptRun(
  run: AutoOptRunRecord,
  cb: ControllerCallbacks,
  signal: AbortSignal,
): Promise<ControllerOutcome> {
  if (!run.job_id || !run.synth_inputs) {
    throw new Error('cannot resume: the run has no server job to re-attach to');
  }
  const request: AutoOptStartRequest = {
    model: run.model,
    budget: run.budget,
    allow_unload: run.allow_unload,
    answers: run.answers,
  };
  cb.stage('bench_job', 'running');
  return finishBenchJob(run.job_id, run.synth_inputs, request, cb, signal);
}
