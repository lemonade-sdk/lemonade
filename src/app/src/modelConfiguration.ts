import type { ModelInfo } from './api';
import { capabilityFromModelInfo, type ModelCapability } from './modelCapabilities';
import { getBackendCatalogSnapshot } from './features/backends/backendCatalogStore';
import { storageKey } from './storage';

/**
 * Recipe ids belong to lemond, not to a closed client list: a backend the
 * server publishes but this build has never heard of must still typecheck.
 * `auto` is the one client-side value, meaning "let the server choose".
 */
export type RecipeName = string;

export type ThinkingMode = 'none' | 'normal' | 'smart' | 'smart_extra';
export type TuningValueSource = 'custom' | 'built_in' | 'generic' | 'optimized';

/**
 * Option keys likewise come from `/v1/system-info` `recipes[].options[]`, so
 * this is an open record validated at runtime by sanitizeRecipeOptions rather
 * than a compile-time allowlist. The keys that are NOT in options[] and so
 * cannot be discovered from the server are named in the RESIDUAL_* sets below.
 */
export type RecipeOptionValue = string | number | boolean;
export type RecipeOptions = Record<string, RecipeOptionValue | undefined>;

export interface SamplingParams {
  temperature?: number;
  top_p?: number;
  top_k?: number;
  min_p?: number;
  repeat_penalty?: number;
}

export interface TuningValues {
  temperature?: Partial<Record<'precise' | 'balanced' | 'exploratory' | 'creative', number>>;
  context?: Partial<Record<'small' | 'medium' | 'large', number>>;
}

export interface ModelTuning {
  intent_values?: TuningValues;
  recipe_options: RecipeOptions;
  sampling: SamplingParams;
  engine_hint?: RecipeName;
  source?: 'model' | 'user' | 'optimized';
  updated_at?: string;
}

export interface BackendTuning {
  args: string;
  source: 'user' | 'optimized';
  updated_at?: string;
}

export interface ResolvedModelTuning {
  tuning: ModelTuning;
  max_context: number;
  thinking_mode: ThinkingMode;
  sources: {
    recipe_options: Partial<Record<keyof RecipeOptions, TuningValueSource>>;
    sampling: Partial<Record<keyof SamplingParams, TuningValueSource>>;
    thinking_mode: TuningValueSource;
  };
}

export const DEFAULT_CONTEXT_SIZE = 4096;
export const MODEL_CONFIGURATION_EVENT = 'lemonade:model-configuration-changed';

const LS_BACKEND_TUNINGS = 'backend_tunings';
const LS_MODEL_TUNINGS = 'model_tunings';
const LS_MODEL_TUNINGS_MIGRATION_MARKER = 'model_tunings_migrated_v3';
const LS_STORAGE_MIGRATION_CONFLICTS = 'storage_migration_conflicts_v1';

/**
 * The option keys lemond does not publish in `options[]`, and so cannot be
 * discovered: `ctx_size` and `merge_args` are common options living in
 * src/cpp/server/recipe_options.cpp, `speed` comes from a model's
 * `sample_params`, and `mmproj_enabled` is a client-side toggle. Every other
 * key's type is read from the catalog.
 */
const RESIDUAL_NUMBER_OPTIONS = new Set(['ctx_size', 'speed']);
const RESIDUAL_BOOLEAN_OPTIONS = new Set(['merge_args', 'mmproj_enabled']);

function recipeDescriptor(recipe: string) {
  return getBackendCatalogSnapshot().catalog?.byRecipe.get(String(recipe || '').toLowerCase());
}

export const THINKING_MODE_LABELS: Record<ThinkingMode, string> = {
  none: 'None',
  normal: 'Normal',
  smart: 'Smart',
  'smart_extra': 'Smart Extra',
};

function scopedStorageKey(key: string): string {
  return storageKey(key);
}

function emitModelConfigurationEvent(): void {
  try {
    window.dispatchEvent(new CustomEvent(MODEL_CONFIGURATION_EVENT));
  } catch {}
}

export function backendArgsFieldForRecipe(recipe: string): string | null {
  return recipeDescriptor(recipe)?.argsOptionName ?? null;
}

/** The `<recipe>_backend` option, whose name is irregular for sd-cpp. */
export function backendFieldForRecipe(recipe: string): string | null {
  return recipeDescriptor(recipe)?.backendOptionName ?? null;
}

/** True once lemond has described a recipe that takes args, so a UI may offer an args editor. */
export function backendSupportsArgs(recipe: string): boolean {
  return backendArgsFieldForRecipe(recipe) !== null;
}

/**
 * Whether stored backend args for a recipe may be kept. Silence is not a
 * denial: before lemond has answered, every recipe looks argsless, and reusing
 * backendSupportsArgs here would drop the user's saved args on read and erase
 * them on the next write. Only a recipe the server has actually described - and
 * described as taking no args - loses its entry.
 */
function backendArgsAreObsolete(recipe: string): boolean {
  const descriptor = recipeDescriptor(recipe);
  return descriptor !== undefined && descriptor.argsOptionName === null;
}

export interface SessionArgsOverride {
  recipe: string;
  args: string;
}

const sessionArgsOverrides = new Map<string, SessionArgsOverride>();

function sessionArgsKey(modelName: string): string {
  return String(modelName || '').trim().toLowerCase();
}

export function getSessionArgsOverride(modelName: string): SessionArgsOverride | null {
  return sessionArgsOverrides.get(sessionArgsKey(modelName)) || null;
}

export function setSessionArgsOverride(modelName: string, recipe: string, args: string): void {
  sessionArgsOverrides.set(sessionArgsKey(modelName), { recipe, args });
  emitModelConfigurationEvent();
}

export function clearSessionArgsOverride(modelName: string): void {
  if (sessionArgsOverrides.delete(sessionArgsKey(modelName))) emitModelConfigurationEvent();
}

function isBlankValue(value: unknown): boolean {
  return value === undefined || value === null || value === '';
}

function positiveNumberValue(value: unknown): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : undefined;
}

function optionalNumberValue(value: unknown): number | undefined {
  if (isBlankValue(value)) return undefined;
  const n = Number(value);
  return Number.isFinite(n) ? n : undefined;
}

function optionalStringValue(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim();
  return trimmed ? trimmed : undefined;
}

/**
 * Which option keys are numeric or boolean, from the schema lemond publishes
 * plus the residue it does not. Used only to coerce values that arrive as
 * strings; already-typed values pass through untouched.
 */
function optionKeysOfType(typeName: string, residue: Set<string>): Set<string> {
  const keys = new Set(residue);
  for (const descriptor of getBackendCatalogSnapshot().catalog?.recipes ?? []) {
    for (const option of descriptor.options) {
      if (option.typeName === typeName) keys.add(option.name);
    }
  }
  return keys;
}

export function sanitizeRecipeOptions(options: Partial<RecipeOptions> | null | undefined): RecipeOptions {
  const out: RecipeOptions = {};
  if (!options || typeof options !== 'object') return out;
  const numberKeys = optionKeysOfType('SIZE', RESIDUAL_NUMBER_OPTIONS);
  const booleanKeys = optionKeysOfType('BOOL', RESIDUAL_BOOLEAN_OPTIONS);
  for (const [rawKey, value] of Object.entries(options)) {
    // Legacy spelling in tuning saved by earlier builds.
    const key = rawKey === 'sd_cpp_backend' ? 'sd-cpp_backend' : rawKey;
    if (isBlankValue(value)) continue;
    if (booleanKeys.has(key)) {
      if (typeof value === 'boolean') (out as Record<string, unknown>)[key] = value;
      continue;
    }
    if (numberKeys.has(key)) {
      const n = optionalNumberValue(value);
      if (n !== undefined) (out as Record<string, unknown>)[key] = n;
      continue;
    }
    if (typeof value === 'string') {
      const str = optionalStringValue(value);
      if (str !== undefined) (out as Record<string, unknown>)[key] = str;
      continue;
    }
    (out as Record<string, unknown>)[key] = value;
  }
  return out;
}

export function sanitizeSamplingParams(params: Partial<SamplingParams> | null | undefined): SamplingParams {
  const out: SamplingParams = {};
  if (!params || typeof params !== 'object') return out;
  for (const key of ['temperature', 'top_p', 'top_k', 'min_p', 'repeat_penalty'] as Array<keyof SamplingParams>) {
    const n = optionalNumberValue(params[key]);
    if (n !== undefined) out[key] = n;
  }
  return out;
}

function sanitizeTuningValues(raw: TuningValues | null | undefined): TuningValues {
  const temperature: NonNullable<TuningValues['temperature']> = {};
  const context: NonNullable<TuningValues['context']> = {};
  const rawTemperature = raw?.temperature && typeof raw.temperature === 'object' ? raw.temperature : {};
  const rawContext = raw?.context && typeof raw.context === 'object' ? raw.context : {};
  for (const hint of ['precise', 'balanced', 'exploratory', 'creative'] as const) {
    const value = optionalNumberValue(rawTemperature[hint]);
    if (value !== undefined && value >= 0) temperature[hint] = value;
  }
  for (const hint of ['small', 'medium', 'large'] as const) {
    const value = positiveNumberValue(rawContext[hint]);
    if (value !== undefined) context[hint] = Math.round(value);
  }
  return {
    ...(Object.keys(temperature).length ? { temperature } : {}),
    ...(Object.keys(context).length ? { context } : {}),
  };
}

function normalizeRecipeName(value: unknown): RecipeName | undefined {
  const rawName = String(value || '').trim().toLowerCase();
  // Legacy spellings in tuning saved by earlier builds.
  const name = rawName === 'sd_cpp' ? 'sd-cpp' : rawName === 'ryzenai_llm' ? 'ryzenai-llm' : rawName;
  return name || undefined;
}

export function sanitizeModelTuning(raw: Partial<ModelTuning> | null | undefined): ModelTuning {
  const intent_values = sanitizeTuningValues(raw?.intent_values);
  const recipe_options = sanitizeRecipeOptions(raw?.recipe_options || {});
  const sampling = sanitizeSamplingParams(raw?.sampling || {});
  const engine_hint = normalizeRecipeName(raw?.engine_hint);
  return {
    ...(Object.keys(intent_values).length ? { intent_values } : {}),
    recipe_options,
    sampling,
    ...(engine_hint ? { engine_hint } : {}),
    source: raw?.source === 'user' || raw?.source === 'model' || raw?.source === 'optimized' ? raw.source : undefined,
    updated_at: typeof raw?.updated_at === 'string' ? raw.updated_at : undefined,
  };
}

function sanitizeBackendTuning(raw: Partial<BackendTuning> | null | undefined): BackendTuning | null {
  if (!raw || typeof raw !== 'object') return null;
  const args = typeof raw.args === 'string' ? raw.args.trim() : '';
  if (!args) return null;
  return {
    args,
    source: raw.source === 'optimized' ? 'optimized' : 'user',
    ...(typeof raw.updated_at === 'string' && raw.updated_at ? { updated_at: raw.updated_at } : {}),
  };
}

export function loadBackendTunings(): Record<string, BackendTuning> {
  try {
    const raw = localStorage.getItem(scopedStorageKey(LS_BACKEND_TUNINGS));
    const parsed = raw ? JSON.parse(raw) : {};
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
    const normalized: Record<string, BackendTuning> = {};
    for (const [key, value] of Object.entries(parsed)) {
      const tuning = sanitizeBackendTuning(value as Partial<BackendTuning>);
      const recipe = key.split(':')[0] || '';
      if (tuning && !backendArgsAreObsolete(recipe)) normalized[key] = tuning;
    }
    return normalized;
  } catch {
    return {};
  }
}

export function saveBackendTunings(tunings: Record<string, BackendTuning>): void {
  const normalized: Record<string, BackendTuning> = {};
  for (const [key, value] of Object.entries(tunings)) {
    const recipe = key.split(':')[0] || '';
    const tuning = sanitizeBackendTuning(value);
    if (tuning && !backendArgsAreObsolete(recipe)) normalized[key] = tuning;
  }
  localStorage.setItem(scopedStorageKey(LS_BACKEND_TUNINGS), JSON.stringify(normalized));
  emitModelConfigurationEvent();
}

export function backendTuningForKey(key: string): BackendTuning | null {
  return loadBackendTunings()[key] || null;
}

export function saveBackendTuning(key: string, args: string, source: BackendTuning['source'] = 'user'): void {
  if (!key) return;
  const recipe = key.split(':')[0] || '';
  if (backendArgsAreObsolete(recipe)) return;
  const next = { ...loadBackendTunings() };
  const trimmed = String(args || '').trim();
  if (!trimmed) {
    delete next[key];
  } else {
    next[key] = { args: trimmed, source, updated_at: new Date().toISOString() };
  }
  saveBackendTunings(next);
}

export function resetBackendTuning(key: string): void {
  if (!key) return;
  const next = { ...loadBackendTunings() };
  delete next[key];
  saveBackendTunings(next);
}

function isStorageRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function storageValuesEqual(left: unknown, right: unknown): boolean {
  try {
    return JSON.stringify(left) === JSON.stringify(right);
  } catch {
    return left === right;
  }
}

const LEGACY_DEFAULT_RECORD_SUFFIX_ORDER = ['default', 's_default', 's__default'] as const;
const LEGACY_DEFAULT_RECORD_SUFFIXES = new Set<string>(LEGACY_DEFAULT_RECORD_SUFFIX_ORDER);
const LEGACY_RECORD_SEPARATOR = '@@';

function compareStorageKeys(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function migrateLegacyModelConfigurationStorage(): boolean {
  try {
    const markerKey = scopedStorageKey(LS_MODEL_TUNINGS_MIGRATION_MARKER);
    if (localStorage.getItem(markerKey) === 'true') return true;

    const tuningKey = scopedStorageKey(LS_MODEL_TUNINGS);
    const raw = localStorage.getItem(tuningKey);
    if (!raw) {
      localStorage.setItem(markerKey, 'true');
      return true;
    }

    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      return false;
    }
    if (!isStorageRecord(parsed)) return false;

    const next = { ...parsed };
    const legacyRecordsByModel = new Map<string, { sourceKey: string; suffix: string; value: unknown }[]>();
    const legacySourceKeys = new Set<string>();
    let changed = false;

    for (const sourceKey of Object.keys(parsed)) {
      const separatorIndex = sourceKey.lastIndexOf(LEGACY_RECORD_SEPARATOR);
      if (separatorIndex <= 0) continue;
      const modelName = sourceKey.slice(0, separatorIndex);
      const suffix = sourceKey.slice(separatorIndex + LEGACY_RECORD_SEPARATOR.length).trim().toLowerCase();
      if (!modelName || !LEGACY_DEFAULT_RECORD_SUFFIXES.has(suffix)) continue;

      const records = legacyRecordsByModel.get(modelName) || [];
      records.push({ sourceKey, suffix, value: parsed[sourceKey] });
      legacyRecordsByModel.set(modelName, records);
      legacySourceKeys.add(sourceKey);
    }

    const conflicts: Record<string, unknown> = {};
    for (const [modelName, records] of [...legacyRecordsByModel.entries()].sort(([left], [right]) => compareStorageKeys(left, right))) {
      records.sort((left, right) => {
        const suffixOrder = LEGACY_DEFAULT_RECORD_SUFFIX_ORDER.indexOf(left.suffix as typeof LEGACY_DEFAULT_RECORD_SUFFIX_ORDER[number])
          - LEGACY_DEFAULT_RECORD_SUFFIX_ORDER.indexOf(right.suffix as typeof LEGACY_DEFAULT_RECORD_SUFFIX_ORDER[number]);
        return suffixOrder || compareStorageKeys(left.sourceKey, right.sourceKey);
      });

      const hasDirectValue = Object.prototype.hasOwnProperty.call(parsed, modelName) && !legacySourceKeys.has(modelName);
      const selectedValue = hasDirectValue ? parsed[modelName] : records[0].value;
      if (!hasDirectValue) next[modelName] = selectedValue;

      for (const record of records) {
        if (!storageValuesEqual(selectedValue, record.value)) {
          conflicts[`model_tunings:${encodeURIComponent(modelName)}:${encodeURIComponent(record.sourceKey)}`] = record.value;
        }
        delete next[record.sourceKey];
        changed = true;
      }
    }

    if (Object.keys(conflicts).length > 0) {
      const conflictKey = scopedStorageKey(LS_STORAGE_MIGRATION_CONFLICTS);
      const existingRaw = localStorage.getItem(conflictKey);
      let existing: Record<string, unknown> = {};
      if (existingRaw) {
        try {
          const parsedConflicts = JSON.parse(existingRaw);
          if (!isStorageRecord(parsedConflicts)) return false;
          existing = parsedConflicts;
        } catch {
          return false;
        }
      }
      const merged = { ...existing };
      for (const [key, value] of Object.entries(conflicts)) {
        if (Object.prototype.hasOwnProperty.call(merged, key)) {
          if (storageValuesEqual(merged[key], value)) continue;
          let collision = 2;
          let collisionKey = `${key}:${collision}`;
          while (Object.prototype.hasOwnProperty.call(merged, collisionKey)
            && !storageValuesEqual(merged[collisionKey], value)) {
            collisionKey = `${key}:${++collision}`;
          }
          merged[collisionKey] = value;
        } else {
          merged[key] = value;
        }
      }
      try {
        localStorage.setItem(conflictKey, JSON.stringify(merged));
      } catch {
        return false;
      }
    }

    if (changed) {
      try {
        localStorage.setItem(tuningKey, JSON.stringify(next));
      } catch {
        return false;
      }
      for (const sourceKey of legacySourceKeys) localStorage.removeItem(sourceKey);
    }
    try {
      localStorage.setItem(markerKey, 'true');
    } catch {
      return false;
    }
    return true;
  } catch {
    return false;
  }
}

function hasConcreteTuning(tuning: ModelTuning | null | undefined): boolean {
  return !!tuning && (
    Object.keys(tuning.intent_values?.temperature || {}).length > 0
    || Object.keys(tuning.intent_values?.context || {}).length > 0
    || Object.keys(tuning.recipe_options || {}).length > 0
    || Object.keys(tuning.sampling || {}).length > 0
    || !!tuning.engine_hint
  );
}

export function loadModelTunings(): Record<string, ModelTuning> {
  migrateLegacyModelConfigurationStorage();
  try {
    const raw = localStorage.getItem(scopedStorageKey(LS_MODEL_TUNINGS));
    const parsed = raw ? JSON.parse(raw) : {};
    if (!isStorageRecord(parsed)) return {};
    const normalized: Record<string, ModelTuning> = {};
    for (const [storedKey, tuning] of Object.entries(parsed)) {
      if (isStorageRecord(tuning)) normalized[storedKey] = sanitizeModelTuning(tuning as Partial<ModelTuning>);
    }
    return normalized;
  } catch {
    return {};
  }
}

export function saveModelTunings(tunings: Record<string, ModelTuning>): void {
  migrateLegacyModelConfigurationStorage();
  localStorage.setItem(scopedStorageKey(LS_MODEL_TUNINGS), JSON.stringify(tunings));
  emitModelConfigurationEvent();
}

export function loadModelTuning(modelName: string): ModelTuning | null {
  if (!modelName) return null;
  return loadModelTunings()[modelName] || null;
}

export function saveModelTuning(modelName: string, tuning: Partial<ModelTuning>): void {
  if (!modelName) return;
  const sanitized = sanitizeModelTuning({ ...tuning, source: 'user', updated_at: new Date().toISOString() });
  const next = { ...loadModelTunings() };
  if (hasConcreteTuning(sanitized)) next[modelName] = sanitized;
  else delete next[modelName];
  saveModelTunings(next);
}

export function resetModelTuning(modelName: string): void {
  if (!modelName) return;
  const next = { ...loadModelTunings() };
  delete next[modelName];
  saveModelTunings(next);
}

export function hasModelTuning(modelName: string): boolean {
  return hasConcreteTuning(loadModelTuning(modelName));
}

function readNumberFrom(value: unknown, paths: string[][]): number | undefined {
  for (const path of paths) {
    let current: unknown = value;
    for (const key of path) {
      if (!current || typeof current !== 'object') {
        current = undefined;
        break;
      }
      current = (current as Record<string, unknown>)[key];
    }
    const n = Number(current);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return undefined;
}

function readStringFrom(value: unknown, paths: string[][]): string | undefined {
  for (const path of paths) {
    let current: unknown = value;
    for (const key of path) {
      if (!current || typeof current !== 'object') {
        current = undefined;
        break;
      }
      current = (current as Record<string, unknown>)[key];
    }
    const str = optionalStringValue(current);
    if (str !== undefined) return str;
  }
  return undefined;
}

function recipesForModel(model: ModelInfo | null | undefined): unknown[] {
  return Array.isArray(model?.recipes) ? model.recipes : [];
}

function recipeName(recipe: unknown): string {
  if (!recipe || typeof recipe !== 'object') return '';
  const record = recipe as Record<string, unknown>;
  const name = String(record.recipe || record.name || record.id || '').trim().toLowerCase();
  return name === 'sd_cpp' ? 'sd-cpp' : name === 'ryzenai_llm' ? 'ryzenai-llm' : name;
}

function activeRecipeName(model: ModelInfo | null | undefined): string {
  const rawDirect = String((model as Record<string, unknown> | null | undefined)?.recipe || '').trim().toLowerCase();
  const direct = rawDirect === 'sd_cpp' ? 'sd-cpp' : rawDirect === 'ryzenai_llm' ? 'ryzenai-llm' : rawDirect;
  if (direct) return direct;
  return recipeName(recipesForModel(model)[0]);
}

export function modelSupportsContextSize(model: ModelInfo | null | undefined): boolean {
  if (!model) return false;
  const capability = capabilityFromModelInfo(model);
  if (capability === 'chat') return true;
  if (capability !== 'unknown') return false;
  return recipeDescriptor(activeRecipeName(model))?.usesCtxSize === true;
}

function readNumberFromActiveRecipe(model: ModelInfo | null | undefined, paths: string[][]): number | undefined {
  const recipes = recipesForModel(model);
  const active = activeRecipeName(model);
  if (active) {
    for (const recipe of recipes) {
      if (recipeName(recipe) === active) {
        const value = readNumberFrom(recipe, paths);
        if (value) return value;
      }
    }
  }
  for (const recipe of recipes) {
    const value = readNumberFrom(recipe, paths);
    if (value) return value;
  }
  return undefined;
}

function readStringFromActiveRecipe(model: ModelInfo | null | undefined, paths: string[][]): string | undefined {
  const recipes = recipesForModel(model);
  const active = activeRecipeName(model);
  if (active) {
    for (const recipe of recipes) {
      if (recipeName(recipe) === active) {
        const value = readStringFrom(recipe, paths);
        if (value !== undefined) return value;
      }
    }
  }
  for (const recipe of recipes) {
    const value = readStringFrom(recipe, paths);
    if (value !== undefined) return value;
  }
  return undefined;
}

function readNumberFromModelOrRecipe(model: ModelInfo | null | undefined, paths: string[][]): number | undefined {
  return readNumberFrom(model, paths) ?? readNumberFromActiveRecipe(model, paths);
}

function readStringFromModelOrRecipe(model: ModelInfo | null | undefined, paths: string[][]): string | undefined {
  return readStringFrom(model, paths) ?? readStringFromActiveRecipe(model, paths);
}

const MAX_CONTEXT_PATHS = [
  ['max_context_window'], ['max_ctx_size'], ['max_ctx'], ['context_window'], ['context_length'], ['max_sequence_length'],
  ['metadata', 'max_context_window'], ['metadata', 'context_length'],
  ['recipe_options', 'max_context_window'], ['recipe_options', 'max_ctx_size'], ['recipe_options', 'max_ctx'],
  ['options', 'max_context_window'], ['options', 'max_ctx_size'], ['options', 'max_ctx'],
];

const DEFAULT_CONTEXT_PATHS = [
  ['recipe_options', 'ctx_size'], ['options', 'ctx_size'], ['ctx_size'], ['n_ctx'],
];

export function modelContextSize(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): number {
  const maximum = readNumberFromModelOrRecipe(model, MAX_CONTEXT_PATHS);
  if (maximum) return Math.round(maximum);
  const configured = readNumberFromModelOrRecipe(model, DEFAULT_CONTEXT_PATHS);
  if (configured) return Math.round(configured);
  const fallback = Number(fallbackCtxSize);
  return Number.isFinite(fallback) && fallback > 0 ? Math.round(fallback) : DEFAULT_CONTEXT_SIZE;
}

export function modelDefaultContextSize(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): number {
  const configured = readNumberFromModelOrRecipe(model, DEFAULT_CONTEXT_PATHS);
  return Math.min(Math.round(configured || Number(fallbackCtxSize) || DEFAULT_CONTEXT_SIZE), modelContextSize(model, fallbackCtxSize));
}

/**
 * Where a model may carry an option's value besides its own key. The generic
 * spellings are per option TYPE and the legacy ones per option NAME - both
 * describe the model registry's shape, so a new backend adds none of them.
 */
const GENERIC_PATHS_BY_TYPE: Record<string, string[][]> = {
  BACKEND: [['recipe_options', 'backend'], ['options', 'backend'], ['backend'], ['default_backend'], ['recommended_backend']],
  ARGS: [['args']],
  DEVICES: [['device']],
};

const LEGACY_PATHS_BY_OPTION: Record<string, string[][]> = {
  steps: [['sample_steps']],
  cfg_scale: [['sample_params', 'guidance', 'txt_cfg'], ['txt_cfg']],
};

function optionPaths(name: string, typeName: string): string[][] {
  return [
    ['recipe_options', name],
    ['options', name],
    ['sample_params', name],
    [name],
    ...(LEGACY_PATHS_BY_OPTION[name] ?? []),
    ...(GENERIC_PATHS_BY_TYPE[typeName] ?? []),
  ];
}

export function modelDefaultRecipeOptions(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): RecipeOptions {
  if (!model) return {};
  const capability = capabilityFromModelInfo(model);
  const descriptor = recipeDescriptor(activeRecipeName(model));
  const out: RecipeOptions = {};

  // ctx_size is a common option, not a descriptor one: lemond publishes only
  // whether a recipe takes it. Chat surfaces still default it when the recipe
  // is not in the catalog.
  if (descriptor?.usesCtxSize || capability === 'chat' || capability === 'omni' || capability === 'unknown') {
    out.ctx_size = modelDefaultContextSize(model, fallbackCtxSize);
  }

  for (const option of descriptor?.options ?? []) {
    const paths = optionPaths(option.name, option.typeName);
    const value = option.typeName === 'SIZE'
      ? readNumberFromModelOrRecipe(model, paths)
      : readStringFromModelOrRecipe(model, paths);
    if (value !== undefined) out[option.name] = value;
  }

  // voice/speed live in the model's sample_params, not in any descriptor.
  if (capability === 'tts' || descriptor?.modality === 'Text-to-speech') {
    out.voice = readStringFromModelOrRecipe(model, [['recipe_options', 'voice'], ['sample_params', 'voice'], ['default_voice'], ['voice']]);
    out.speed = readNumberFromModelOrRecipe(model, [['recipe_options', 'speed'], ['sample_params', 'speed'], ['default_speed'], ['speed']]);
  }

  return sanitizeRecipeOptions(out);
}

export function modelDefaultSampling(model: ModelInfo | null | undefined): SamplingParams {
  if (!model) return {};
  return sanitizeSamplingParams({
    temperature: readNumberFrom(model, [['sampling', 'temperature'], ['sample_params', 'temperature'], ['recipe_options', 'temperature'], ['temperature']]),
    top_p: readNumberFrom(model, [['sampling', 'top_p'], ['sample_params', 'top_p'], ['recipe_options', 'top_p'], ['top_p']]),
    top_k: readNumberFrom(model, [['sampling', 'top_k'], ['sample_params', 'top_k'], ['recipe_options', 'top_k'], ['top_k']]),
    repeat_penalty: readNumberFrom(model, [['sampling', 'repeat_penalty'], ['sample_params', 'repeat_penalty'], ['recipe_options', 'repeat_penalty'], ['repeat_penalty']]),
  });
}

function sourceMapForRecipe(options: RecipeOptions, source: TuningValueSource): Partial<Record<keyof RecipeOptions, TuningValueSource>> {
  return Object.fromEntries(Object.keys(options).map(key => [key, source])) as Partial<Record<keyof RecipeOptions, TuningValueSource>>;
}

function sourceMapForSampling(params: SamplingParams, source: TuningValueSource): Partial<Record<keyof SamplingParams, TuningValueSource>> {
  return Object.fromEntries(Object.keys(params).map(key => [key, source])) as Partial<Record<keyof SamplingParams, TuningValueSource>>;
}

function tuningContextCeiling(tuning: Partial<ModelTuning> | null | undefined): number {
  if (!tuning) return 0;
  const values = [
    tuning.recipe_options?.ctx_size,
    ...Object.values(tuning.intent_values?.context || {}),
  ].map(Number).filter(value => Number.isFinite(value) && value > 0);
  return values.length ? Math.max(...values) : 0;
}

function resolvedMaximumContext(
  model: ModelInfo | null | undefined,
  fallbackCtxSize: unknown,
  ...tunings: Array<Partial<ModelTuning> | null | undefined>
): number {
  return Math.max(modelContextSize(model, fallbackCtxSize), ...tunings.map(tuningContextCeiling));
}

export function modelBaseTuningForModel(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): ModelTuning {
  return {
    recipe_options: modelDefaultRecipeOptions(model, fallbackCtxSize),
    sampling: modelDefaultSampling(model),
    source: 'model',
  };
}

export function effectiveModelTuningForModel(
  modelName: string,
  model: ModelInfo | null | undefined,
  fallbackCtxSize?: unknown,
): ResolvedModelTuning {
  const user = loadModelTuning(modelName);
  const maxContext = resolvedMaximumContext(model, fallbackCtxSize, user);
  const recipe_options = modelDefaultRecipeOptions(model, fallbackCtxSize);
  const sampling = modelDefaultSampling(model);
  const recipeSources = sourceMapForRecipe(recipe_options, 'built_in');
  const samplingSources = sourceMapForSampling(sampling, 'built_in');

  if (user) {
    Object.assign(recipe_options, user.recipe_options || {});
    Object.assign(sampling, user.sampling || {});
    Object.assign(recipeSources, sourceMapForRecipe(user.recipe_options || {}, user.source === 'optimized' ? 'optimized' : 'custom'));
    Object.assign(samplingSources, sourceMapForSampling(user.sampling || {}, user.source === 'optimized' ? 'optimized' : 'custom'));
  }

  return {
    tuning: {
      recipe_options: sanitizeRecipeOptions(recipe_options),
      sampling: sanitizeSamplingParams(sampling),
      engine_hint: user?.engine_hint || (activeRecipeName(model) as RecipeName) || 'auto',
      source: user ? (user.source === 'optimized' ? 'optimized' : 'user') : 'model',
      updated_at: user?.updated_at,
    },
    max_context: maxContext,
    thinking_mode: 'normal',
    sources: {
      recipe_options: recipeSources,
      sampling: samplingSources,
      thinking_mode: 'generic',
    },
  };
}

function pickRecipeOptions(options: RecipeOptions, keys: Array<keyof RecipeOptions>): RecipeOptions {
  const picked: RecipeOptions = {};
  for (const key of keys) {
    const value = options[key];
    if (value !== undefined && value !== '') (picked as Record<string, unknown>)[key] = value;
  }
  return picked;
}

/** Which of lemond's modalities serve each UI capability. */
const MODALITIES_BY_CAPABILITY: Record<string, string[]> = {
  image: ['Image generation'],
  audio: ['Speech-to-text'],
  transcription: ['Speech-to-text'],
  'audio-generation': ['Audio generation'],
  tts: ['Text-to-speech'],
  model3d: ['3D generation'],
  chat: ['Text generation'],
  omni: ['Text generation'],
  vision: ['Text generation'],
  code: ['Text generation'],
  embedding: ['Text generation'],
  reranking: ['Text generation'],
};

/** Keys lemond does not publish, so they cannot be discovered per capability. */
const RESIDUAL_KEYS_BY_CAPABILITY: Record<string, string[]> = {
  tts: ['voice', 'speed'],
  chat: ['ctx_size', 'mmproj_enabled'],
  omni: ['ctx_size', 'mmproj_enabled'],
  vision: ['ctx_size', 'mmproj_enabled'],
  code: ['ctx_size', 'mmproj_enabled'],
  embedding: ['ctx_size', 'mmproj_enabled'],
  reranking: ['ctx_size', 'mmproj_enabled'],
};

export function recipeOptionsForCapability(
  options: RecipeOptions,
  capability: ModelCapability | 'all' | 'vision' | 'code' | 'transcription',
): RecipeOptions {
  if (!options || Object.keys(options).length === 0) return {};
  const modalities = MODALITIES_BY_CAPABILITY[capability];
  if (!modalities) return { ...options };

  // Every option any backend of this modality declares, so a backend added to
  // lemond keeps its settings instead of having them filtered away.
  const keys = new Set<string>(['merge_args', ...(RESIDUAL_KEYS_BY_CAPABILITY[capability] ?? [])]);
  for (const descriptor of getBackendCatalogSnapshot().catalog?.recipes ?? []) {
    if (!modalities.includes(descriptor.modality)) continue;
    for (const option of descriptor.options) keys.add(option.name);
  }
  return pickRecipeOptions(options, [...keys]);
}

export interface BackendResolutionContext {
  explicitOptions?: RecipeOptions | null;
  modelOptions?: RecipeOptions | null;
  systemInfo?: Record<string, unknown> | null;
}

export interface ActiveBackendTuning {
  key: string;
  recipe: string;
  backend: string;
  tuning: BackendTuning;
}

function normalizeBackendValue(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim().toLowerCase();
  return trimmed ? trimmed : undefined;
}

function recipeDefaultBackend(systemInfo: Record<string, unknown> | null | undefined, recipe: string): string | undefined {
  const recipes = (systemInfo?.recipes ?? null) as Record<string, { default_backend?: unknown }> | null;
  return normalizeBackendValue(recipes?.[recipe]?.default_backend);
}

function concreteBackendForRecipe(
  recipe: string,
  explicitOptions: RecipeOptions | null | undefined,
  modelOptions: RecipeOptions | null | undefined,
  systemInfo: Record<string, unknown> | null | undefined,
): string | undefined {
  const field = backendFieldForRecipe(recipe);
  return (field ? normalizeBackendValue(explicitOptions?.[field]) : undefined)
    ?? (field ? normalizeBackendValue(modelOptions?.[field]) : undefined)
    ?? recipeDefaultBackend(systemInfo, recipe);
}

export function activeBackendTuningForModel(
  model?: ModelInfo | null,
  ctx?: BackendResolutionContext,
): ActiveBackendTuning | null {
  if (!model) return null;
  const tunings = loadBackendTunings();
  if (Object.keys(tunings).length === 0) return null;

  const recipes: string[] = [];
  const active = String((model as Record<string, unknown>).recipe || '').trim().toLowerCase();
  if (active) recipes.push(active);
  for (const recipe of recipesForModel(model)) {
    const name = recipeName(recipe);
    if (name && !recipes.includes(name)) recipes.push(name);
  }

  for (const recipe of recipes) {
    if (!backendSupportsArgs(recipe)) continue;
    const backend = concreteBackendForRecipe(recipe, ctx?.explicitOptions, ctx?.modelOptions, ctx?.systemInfo);
    if (!backend) continue;
    const key = `${recipe}:${backend}`;
    const tuning = tunings[key];
    if (tuning) return { key, recipe, backend, tuning };
  }
  return null;
}

export function recipeOptionsForModel(
  modelName: string,
  model?: ModelInfo | null,
  explicitOptions?: RecipeOptions | null,
  systemInfo?: Record<string, unknown> | null,
): RecipeOptions | undefined {
  const capability = model ? capabilityFromModelInfo(model) : undefined;
  const directTuning = loadModelTuning(modelName);
  const modelOptions = model
    ? recipeOptionsForCapability(directTuning?.recipe_options || {}, capability!)
    : (directTuning?.recipe_options || {});
  if (modelSupportsContextSize(model) && modelOptions.ctx_size === undefined) {
    modelOptions.ctx_size = -1;
  }

  const backendTuning = activeBackendTuningForModel(model, {
    explicitOptions,
    modelOptions,
    systemInfo,
  });
  const backendOptions: RecipeOptions = {};
  if (backendTuning) {
    const field = backendArgsFieldForRecipe(backendTuning.recipe);
    if (field) (backendOptions as Record<string, unknown>)[field] = backendTuning.tuning.args;
  }

  const merged: RecipeOptions = { ...backendOptions, ...modelOptions };
  const sessionOverride = getSessionArgsOverride(modelName);
  if (sessionOverride) {
    const field = backendArgsFieldForRecipe(sessionOverride.recipe);
    if (field) {
      (merged as Record<string, unknown>)[field] = sessionOverride.args;
      merged.merge_args = false;
    }
  }
  Object.assign(merged, explicitOptions || {});
  return Object.keys(merged).length > 0 ? merged : undefined;
}

export type ChatRequestOptions = SamplingParams;

export function samplingForModel(modelName: string, _model?: ModelInfo | null): ChatRequestOptions {
  return { ...(loadModelTuning(modelName)?.sampling || {}) };
}
