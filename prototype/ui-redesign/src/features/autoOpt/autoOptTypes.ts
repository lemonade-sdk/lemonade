export type AutoOptBudget = 'quick' | 'standard' | 'thorough';
export type AutoOptRunStatus = 'queued' | 'running' | 'completed' | 'failed' | 'cancelled';
export type AutoOptStageStatus = 'pending' | 'running' | 'completed' | 'failed' | 'skipped';
export type AutoOptParallelMode = 'single' | 'parallel';
export type AutoOptKvCacheQuant = 'none' | 'q8_0' | 'q5_1' | 'q4_0';
export type AutoOptRamHeadroom = 'normal' | 'reduced' | 'minimal' | 'disabled';

export interface AutoOptParallelAnswer {
  mode: AutoOptParallelMode;
  slots?: number;
  dedicated?: boolean;
}

export interface AutoOptAnswers {
  parallel: AutoOptParallelAnswer;
  kv_cache_quant: AutoOptKvCacheQuant;
  ram_headroom: AutoOptRamHeadroom;
  allow_network: boolean;
  backends_to_consider?: string[];
}

export interface AutoOptStartRequest {
  model: string;
  budget: AutoOptBudget;
  allow_unload: boolean;
  answers: AutoOptAnswers;
}

export interface WizardAnswers {
  parallel: boolean;
  slots: number;
  dedicated_slots: boolean;
  kv_cache_quant: AutoOptKvCacheQuant;
  ram_headroom: AutoOptRamHeadroom;
  allow_network: boolean;
  backends_to_consider: string[];
}

export function wizardAnswersFrom(answers: AutoOptAnswers): WizardAnswers {
  const parallel = answers.parallel?.mode === 'parallel';
  return {
    parallel,
    slots: Math.max(1, answers.parallel?.slots ?? (parallel ? 2 : 1)),
    dedicated_slots: answers.parallel?.dedicated ?? true,
    kv_cache_quant: answers.kv_cache_quant || 'none',
    ram_headroom: answers.ram_headroom || 'normal',
    allow_network: answers.allow_network !== false,
    backends_to_consider: answers.backends_to_consider || [],
  };
}

export interface AutoOptProgress {
  stage: string;
  stage_index: number;
  stage_count: number;
  detail?: string;
}

export interface AutoOptStage {
  name: string;
  status: AutoOptStageStatus;
  duration_ms?: number;
  error?: string;
  data?: Record<string, unknown>;
}

export interface HardwareGpu {
  vendor: string;
  name: string;
  family: string;
  vram_gb: number;
}

export interface HardwareSnapshot {
  gpus: HardwareGpu[];
  has_igpu: boolean;
  ram_is_vram: boolean;
  host_ram_gb: number;
  installed_backends: string[];
  os: string;
}

export interface ModelFacts {
  architecture: string;
  block_count: number;
  expert_count: number;
  full_attention_interval: number;
  swa_layer_count: number;
  n_ctx_train: number;
  kv_bytes_per_token: number;
  weights_mib: number;
  is_moe: boolean;
  is_hybrid_or_recurrent: boolean;
  has_mtp: boolean;
  base_model_repo: string;
  checkpoint: string;
  metadata_present: boolean;
}

export interface FitEstimate {
  backend: string;
  fits_fully: boolean;
  fitted_ctx: number;
  fitted_ngl: number;
  fitted_ncmoe: number;
  weights_mib: number;
  kv_mib: number;
  compute_mib: number;
  total_mib: number;
  available_mib: number;
  degraded: boolean;
  ok: boolean;
  note?: string;
}

export interface BenchParams {
  d?: number;
  b?: number;
  ub?: number;
  spec_n?: number;
  ladder?: boolean;
}

export interface BenchPoint {
  backend: string;
  label: string;
  ctx_size: number;
  llamacpp_args: string;
  params: BenchParams;
  ttft_ms: number;
  tps: number;
  vram_gb: number;
  ok: boolean;
  error?: string;
  max_loaded_ctx?: number;
}

export interface SamplingDefaults {
  temperature?: number;
  top_p?: number;
  min_p?: number;
  top_k?: number;
  source: string;
}

export interface AutoOptExpected {
  ttft_ms?: number;
  tps?: number;
  vram_gb?: number;
}

export interface AutoOptRecommendation {
  label: string;
  tradeoff?: string;
  llamacpp_backend: string;
  ctx_size: number;
  llamacpp_args: string;
  rationale: string[];
  expected?: AutoOptExpected;
}

export interface AutoOptResult {
  primary: AutoOptRecommendation;
  alternatives: AutoOptRecommendation[];
  sampling_defaults?: SamplingDefaults;
}

export interface AutoOptMeasurements {
  fit: FitEstimate[];
  bench: BenchPoint[];
}

export interface BenchPlanEntry {
  label: string;
  backend: string;
  ctx_size: number;
  llamacpp_args: string;
  params: BenchParams;
  ctx_probe?: boolean;
  ttft_key: string;
  tps_key: string;
  vram_key: string;
  fallback_ctx_size?: number;
  fallback_ttft_key?: string;
  fallback_tps_key?: string;
  fallback_vram_key?: string;
}

export interface SynthInputs {
  hardware: HardwareSnapshot;
  facts: ModelFacts;
  fits: FitEstimate[];
  sampling?: SamplingDefaults;
  plan: BenchPlanEntry[];
  step_labels: Record<string, string>;
}

export interface AutoOptRunRecord {
  id: string;
  model: string;
  checkpoint: string;
  budget: AutoOptBudget;
  answers: AutoOptAnswers;
  allow_unload: boolean;
  status: AutoOptRunStatus;
  created_at: string;
  finished_at?: string;
  summary?: string;
  error?: string;
  lemonade_version?: string;
  progress?: AutoOptProgress;
  stages: AutoOptStage[];
  measurements: AutoOptMeasurements;
  result?: AutoOptResult;
  job_id?: string;
  synth_inputs?: SynthInputs;
}

export function isAutoOptRunActive(run: Pick<AutoOptRunRecord, 'status'>): boolean {
  return run.status === 'queued' || run.status === 'running';
}
