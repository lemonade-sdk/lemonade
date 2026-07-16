import api, { type ModelInfo } from '../../api';
import {
  activePresetForModel,
  saveBackendTuning,
  saveOptimizedModelTuning,
  type RecipeOptions,
  type SamplingParams,
} from '../../presetStore';
import type { AutoOptRecommendation, AutoOptRunRecord, SamplingDefaults } from './autoOptTypes';

export interface SavedModelTuningResult {
  presetId: string;
  presetName: string;
}

export interface SavedBackendTuningResult {
  key: string;
  backend: string;
}

export function recommendationRecipeOptions(rec: AutoOptRecommendation): RecipeOptions {
  const options: RecipeOptions = {};
  if (rec.ctx_size > 0) options.ctx_size = rec.ctx_size;
  if (rec.llamacpp_backend) options.llamacpp_backend = rec.llamacpp_backend;
  if (rec.llamacpp_args) options.llamacpp_args = rec.llamacpp_args;
  return options;
}

function installedBackends(systemInfo: Record<string, unknown> | null | undefined): string[] {
  const recipes = (systemInfo?.recipes || {}) as Record<string, { backends?: Record<string, { state?: unknown }> }>;
  const backends = recipes.llamacpp?.backends || {};
  return Object.entries(backends)
    .filter(([, info]) => ['installed', 'update_available', 'update_required'].includes(String(info?.state || '')))
    .map(([name]) => name);
}

function currentModelInfo(run: AutoOptRunRecord, modelInfo?: ModelInfo | null): ModelInfo | null {
  if (modelInfo) return modelInfo;
  const target = run.model.trim().toLowerCase();
  return api.allModels.find(model =>
    String((model as Record<string, unknown>).model_name || model.name || model.id || '')
      .trim()
      .toLowerCase() === target) || null;
}

export function assertRunApplicable(
  run: AutoOptRunRecord,
  modelInfo: ModelInfo | null | undefined,
  rec: AutoOptRecommendation = run.result?.primary as AutoOptRecommendation,
): void {
  const runCheckpoint = String(run.checkpoint || '').trim();
  const currentCheckpoint = String((modelInfo as Record<string, unknown> | null | undefined)?.checkpoint || '').trim();
  if (runCheckpoint && currentCheckpoint && runCheckpoint !== currentCheckpoint) {
    throw new Error(`This run was measured on a different build of ${run.model} `
      + `(${runCheckpoint} vs current ${currentCheckpoint}). Re-run AutoOpt to apply it.`);
  }
  const backend = rec?.llamacpp_backend;
  const installed = installedBackends(api.systemInfoData);
  if (backend && installed.length > 0 && !installed.includes(backend)) {
    throw new Error(`The recommended backend "${backend}" is not installed on this server. `
      + 'Re-run AutoOpt to pick an available backend.');
  }
}

function samplingFromDefaults(defaults: SamplingDefaults | undefined): SamplingParams {
  if (!defaults) return {};
  const sampling: SamplingParams = {};
  if (defaults.temperature !== undefined) sampling.temperature = defaults.temperature;
  if (defaults.top_p !== undefined) sampling.top_p = defaults.top_p;
  if (defaults.top_k !== undefined) sampling.top_k = defaults.top_k;
  if (defaults.min_p !== undefined) sampling.min_p = defaults.min_p;
  return sampling;
}

/**
 * Persist the selected AutoOpt recommendation in the tuning layer belonging to
 * the model's currently linked Preset. No Preset is created or modified.
 */
export function saveRunToModelTuning(
  run: AutoOptRunRecord,
  rec: AutoOptRecommendation,
  modelInfo?: ModelInfo | null,
): SavedModelTuningResult {
  const resolvedModel = currentModelInfo(run, modelInfo);
  assertRunApplicable(run, resolvedModel, rec);
  const preset = activePresetForModel(run.model);
  saveOptimizedModelTuning(run.model, {
    recipe_options: recommendationRecipeOptions(rec),
    sampling: samplingFromDefaults(run.result?.sampling_defaults),
  }, preset.id, run.id);
  return { presetId: preset.id, presetName: preset.name };
}

/**
 * Replace the args for the exact backend selected by AutoOpt. This deliberately
 * stores only backend-wide arguments; context and sampling remain model tuning.
 */
export function applyRunToBackend(
  run: AutoOptRunRecord,
  rec: AutoOptRecommendation,
): SavedBackendTuningResult {
  assertRunApplicable(run, currentModelInfo(run), rec);
  const backend = String(rec.llamacpp_backend || '').trim();
  const args = String(rec.llamacpp_args || '').trim();
  if (!backend) throw new Error('This recommendation does not name a llama.cpp backend.');
  if (!args) throw new Error('This recommendation does not contain backend arguments to save.');
  const key = `llamacpp:${backend}`;
  // saveBackendTuning replaces the exact entry, so an AutoOpt assignment
  // intentionally overwrites earlier manual or optimized args for this backend.
  saveBackendTuning(key, args, 'optimized', run.id);
  return { key, backend };
}

/** Load once with the selected recommendation and persist nothing. */
export async function applyRunNow(
  run: AutoOptRunRecord,
  rec: AutoOptRecommendation,
): Promise<void> {
  assertRunApplicable(run, currentModelInfo(run), rec);
  const loaded = api.loadedModels.some(model => model.model_name === run.model);
  const tempOptions = { ...recommendationRecipeOptions(rec), save_options: false };
  if (loaded) await api.reloadModel(run.model, tempOptions);
  else await api.loadModel(run.model, tempOptions);
}
