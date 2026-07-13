import api, { type ModelInfo } from '../../api';
import {
  contextHintFromValue,
  labelsFor,
  loadApplied,
  loadUserPresets,
  modelContextSize,
  sanitizePreset,
  saveApplied,
  saveOptimizedModelTuning,
  saveUserPresets,
  TEMPERATURE_HINT_VALUES,
  TEMPERATURE_HINTS,
  type Preset,
  type RecipeOptions,
  type SamplingParams,
  type TemperatureHint,
} from '../../presetStore';
import type { AutoOptRecommendation, AutoOptRunDetail, AutoOptSamplingDefaults } from './autoOptTypes';

function modelShortName(model: string): string {
  const parts = model.split('/').filter(Boolean);
  return parts[parts.length - 1] || model;
}

function nearestTemperatureHint(value: number | undefined): TemperatureHint {
  if (value === undefined || !Number.isFinite(value)) return 'balanced';
  return TEMPERATURE_HINTS.reduce((best, hint) =>
    Math.abs(TEMPERATURE_HINT_VALUES[hint] - value) < Math.abs(TEMPERATURE_HINT_VALUES[best] - value) ? hint : best,
  'balanced' as TemperatureHint);
}

function recommendationRecipeOptions(rec: AutoOptRecommendation): RecipeOptions {
  const options: RecipeOptions = {};
  if (rec.ctx_size !== undefined) options.ctx_size = rec.ctx_size;
  if (rec.llamacpp_backend) options.llamacpp_backend = rec.llamacpp_backend;
  if (rec.llamacpp_args) options.llamacpp_args = rec.llamacpp_args;
  if (rec.mmproj_enabled !== undefined) options.mmproj_enabled = rec.mmproj_enabled;
  return options;
}

function samplingFromDefaults(defaults: AutoOptSamplingDefaults | undefined): SamplingParams {
  if (!defaults) return {};
  const sampling: SamplingParams = {};
  if (defaults.temperature !== undefined) sampling.temperature = defaults.temperature;
  if (defaults.top_p !== undefined) sampling.top_p = defaults.top_p;
  if (defaults.top_k !== undefined) sampling.top_k = defaults.top_k;
  if (defaults.min_p !== undefined) sampling.min_p = defaults.min_p;
  return sampling;
}

export function createPresetFromRun(
  run: AutoOptRunDetail,
  rec: AutoOptRecommendation,
  modelInfo: ModelInfo | null | undefined,
): Preset {
  const samplingDefaults = run.result?.sampling_defaults;
  const preset = sanitizePreset({
    id: `u-${Date.now()}`,
    name: `AutoOpt · ${modelShortName(run.model)}`,
    description: rec.rationale?.[0] || run.summary || '',
    applies_to: modelInfo ? labelsFor(modelInfo) : ['chat'],
    temperature_hint: nearestTemperatureHint(samplingDefaults?.temperature),
    context_hint: rec.ctx_size !== undefined
      ? contextHintFromValue(rec.ctx_size, modelContextSize(modelInfo))
      : 'medium',
    thinking_mode: 'normal',
    recipe_options: {},
    sampling: {},
    engine_hint: 'llamacpp',
    starter: false,
    auto_opt_run_id: run.id,
    auto_opt_enabled: true,
  });
  if (!preset) throw new Error('Could not build a preset from this run.');
  saveUserPresets([preset, ...loadUserPresets()]);
  saveOptimizedModelTuning(run.model, {
    recipe_options: recommendationRecipeOptions(rec),
    sampling: samplingFromDefaults(samplingDefaults),
  }, preset.id, run.id);
  saveApplied({ ...loadApplied(), [run.model]: preset.id });
  return preset;
}

export async function applyRunNow(
  run: AutoOptRunDetail,
  rec: AutoOptRecommendation,
  { save }: { save: boolean },
): Promise<void> {
  if (save) {
    const loaded = api.loadedModels.some(model => model.model_name === run.model);
    if (loaded) await api.reloadModel(run.model);
    else await api.loadModel(run.model);
    return;
  }
  await api.loadModel(run.model, { ...recommendationRecipeOptions(rec), save_options: false });
}
