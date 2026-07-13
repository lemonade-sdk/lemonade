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
  use_vision?: boolean;
  allow_network: boolean;
  backends_to_consider?: string[];
}

export interface AutoOptStartRequest {
  model: string;
  budget: AutoOptBudget;
  allow_unload: boolean;
  answers: AutoOptAnswers;
}

export interface AutoOptProgress {
  stage: string;
  stage_index: number;
  stage_count: number;
  percent?: number;
  detail?: string;
  eta_seconds?: number;
}

export interface AutoOptRunSummary {
  id: string;
  model: string;
  status: AutoOptRunStatus;
  budget: AutoOptBudget;
  created_at: string;
  finished_at?: string;
  summary?: string;
  error?: string;
  lemonade_version?: string;
  progress?: AutoOptProgress;
}

export interface AutoOptStage {
  name: string;
  status: AutoOptStageStatus;
  duration_ms?: number;
  error?: string;
  data?: Record<string, unknown>;
}

export interface AutoOptExpected {
  pp_ts?: number | Record<string, number>;
  tg_ts?: number | Record<string, number>;
  vram_mib?: number;
}

export interface AutoOptSamplingDefaults {
  temperature?: number;
  top_p?: number;
  top_k?: number;
  min_p?: number;
  source: string;
}

export interface AutoOptRecommendation {
  label: string;
  llamacpp_backend?: string;
  ctx_size?: number;
  mmproj_enabled?: boolean;
  llamacpp_args: string;
  rationale: string[];
  tradeoff?: string;
  expected?: AutoOptExpected;
}

export interface AutoOptResult {
  primary: AutoOptRecommendation;
  alternatives: AutoOptRecommendation[];
  sampling_defaults?: AutoOptSamplingDefaults;
}

export interface AutoOptMeasurements {
  fit: Array<Record<string, unknown>>;
  bench: Array<Record<string, unknown>>;
}

export interface AutoOptRunDetail extends AutoOptRunSummary {
  answers?: AutoOptAnswers;
  stages: AutoOptStage[];
  measurements?: AutoOptMeasurements;
  result?: AutoOptResult;
}

export function isAutoOptRunActive(run: Pick<AutoOptRunSummary, 'status'>): boolean {
  return run.status === 'queued' || run.status === 'running';
}
