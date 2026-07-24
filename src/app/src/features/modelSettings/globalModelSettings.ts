import type { LoadedModel, ModelInfo } from '../../api';
import { scopedStorageKey } from '../accounts/accountStore';

export const GLOBAL_MODEL_SETTINGS_EVENT = 'lemonade:global-model-settings-changed';

export type ResourceBudgetMode = 'server' | 'vram' | 'memory';
export type ModelLoadingPolicy = 'keep-loaded' | 'single-active' | 'budget-aware';
export type ModelEvictionPolicy = 'lru' | 'largest' | 'oldest-process' | 'manual';

export interface GlobalModelSettings {
  resourceBudgetMode: ResourceBudgetMode;
  resourceBudgetGb: number;
  autoEvictOnPressure: boolean;
  loadingPolicy: ModelLoadingPolicy;
  evictionPolicy: ModelEvictionPolicy;
  protectPinnedModels: boolean;
  collapseThinkingByDefault: boolean;
  automaticModelUpdates: boolean;
  lastAutomaticUpdateAt: string | null;
}

export const DEFAULT_GLOBAL_MODEL_SETTINGS: GlobalModelSettings = {
  resourceBudgetMode: 'server',
  resourceBudgetGb: 16,
  autoEvictOnPressure: false,
  loadingPolicy: 'keep-loaded',
  evictionPolicy: 'lru',
  protectPinnedModels: true,
  collapseThinkingByDefault: true,
  automaticModelUpdates: false,
  lastAutomaticUpdateAt: null,
};

function settingsKey(scope: string): string {
  return scopedStorageKey(scope, 'global_model_settings');
}

function pinnedModelsKey(scope: string): string {
  return scopedStorageKey(scope, 'pinned_models');
}

function finiteBudget(value: unknown): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return DEFAULT_GLOBAL_MODEL_SETTINGS.resourceBudgetGb;
  return Math.max(1, Math.min(1024, Math.round(parsed * 10) / 10));
}

function oneOf<T extends string>(value: unknown, values: readonly T[], fallback: T): T {
  return values.includes(value as T) ? value as T : fallback;
}

export function sanitizeGlobalModelSettings(value: unknown): GlobalModelSettings {
  const raw = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Partial<GlobalModelSettings>
    : {};
  return {
    resourceBudgetMode: oneOf(raw.resourceBudgetMode, ['server', 'vram', 'memory'] as const, DEFAULT_GLOBAL_MODEL_SETTINGS.resourceBudgetMode),
    resourceBudgetGb: finiteBudget(raw.resourceBudgetGb),
    autoEvictOnPressure: raw.autoEvictOnPressure === true,
    loadingPolicy: oneOf(raw.loadingPolicy, ['keep-loaded', 'single-active', 'budget-aware'] as const, DEFAULT_GLOBAL_MODEL_SETTINGS.loadingPolicy),
    evictionPolicy: oneOf(raw.evictionPolicy, ['lru', 'largest', 'oldest-process', 'manual'] as const, DEFAULT_GLOBAL_MODEL_SETTINGS.evictionPolicy),
    protectPinnedModels: raw.protectPinnedModels !== false,
    collapseThinkingByDefault: raw.collapseThinkingByDefault !== false,
    automaticModelUpdates: raw.automaticModelUpdates === true,
    lastAutomaticUpdateAt: typeof raw.lastAutomaticUpdateAt === 'string' && raw.lastAutomaticUpdateAt.trim()
      ? raw.lastAutomaticUpdateAt
      : null,
  };
}

export function loadGlobalModelSettings(scope: string): GlobalModelSettings {
  try {
    const raw = localStorage.getItem(settingsKey(scope));
    return raw ? sanitizeGlobalModelSettings(JSON.parse(raw)) : { ...DEFAULT_GLOBAL_MODEL_SETTINGS };
  } catch {
    return { ...DEFAULT_GLOBAL_MODEL_SETTINGS };
  }
}

export function saveGlobalModelSettings(scope: string, settings: GlobalModelSettings): GlobalModelSettings {
  const sanitized = sanitizeGlobalModelSettings(settings);
  try { localStorage.setItem(settingsKey(scope), JSON.stringify(sanitized)); } catch { /* best effort */ }
  try {
    window.dispatchEvent(new CustomEvent(GLOBAL_MODEL_SETTINGS_EVENT, { detail: { scope, settings: sanitized } }));
  } catch { /* best effort */ }
  return sanitized;
}

export function loadPinnedModelNames(scope: string): string[] {
  try {
    const raw = localStorage.getItem(pinnedModelsKey(scope));
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed)
      ? Array.from(new Set(parsed.map(value => String(value).trim()).filter(Boolean)))
      : [];
  } catch {
    return [];
  }
}

export function savePinnedModelNames(scope: string, names: Iterable<string>): string[] {
  const normalized = Array.from(new Set([...names].map(name => String(name).trim()).filter(Boolean)));
  try { localStorage.setItem(pinnedModelsKey(scope), JSON.stringify(normalized)); } catch { /* best effort */ }
  return normalized;
}

export function automaticUpdateIsDue(settings: GlobalModelSettings, now = Date.now(), intervalMs = 24 * 60 * 60 * 1000): boolean {
  if (!settings.automaticModelUpdates) return false;
  if (!settings.lastAutomaticUpdateAt) return true;
  const last = Date.parse(settings.lastAutomaticUpdateAt);
  return !Number.isFinite(last) || now - last >= intervalMs;
}

export function isMemoryPressureError(error: unknown): boolean {
  const text = error instanceof Error ? error.message : String(error || '');
  return /(?:out of memory|\boom\b|vram|cuda allocation|hip allocation|memory pressure|failed to allocate|insufficient memory)/i.test(text);
}

function modelName(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  return String((model as any).model_name || model.name || model.id || '').trim();
}

export function estimatedModelSizeGb(model: ModelInfo | null | undefined): number {
  const size = Number((model as any)?.size);
  return Number.isFinite(size) && size > 0 ? size : 0;
}

function modelComponentNames(model: ModelInfo | null | undefined): string[] {
  const raw = (model as any)?.components;
  if (Array.isArray(raw)) return raw.filter((value): value is string => typeof value === 'string' && value.trim().length > 0);
  if (!raw || typeof raw !== 'object') return [];
  return Object.values(raw).filter((value): value is string => typeof value === 'string' && value.trim().length > 0);
}

/** Estimate the concrete footprint of virtual collections from their components. */
export function estimatedModelFootprintGb(
  model: ModelInfo | null | undefined,
  allModels: ModelInfo[],
  seen = new Set<string>(),
): number {
  if (!model) return 0;
  const name = modelName(model).toLowerCase();
  if (name && seen.has(name)) return 0;
  const nextSeen = new Set(seen);
  if (name) nextSeen.add(name);

  const components = modelComponentNames(model);
  if (components.length === 0) return estimatedModelSizeGb(model);
  const byName = new Map(allModels.map(item => [modelName(item).toLowerCase(), item] as const));
  const componentTotal = components.reduce((sum, componentName) => {
    const component = byName.get(componentName.toLowerCase());
    return sum + estimatedModelFootprintGb(component, allModels, nextSeen);
  }, 0);
  return componentTotal > 0 ? componentTotal : estimatedModelSizeGb(model);
}

export interface EvictionCandidate {
  name: string;
  loaded: LoadedModel;
  info: ModelInfo | null;
  estimatedSizeGb: number;
}

export function evictionCandidates(
  loadedModels: LoadedModel[],
  allModels: ModelInfo[],
  targetName: string,
  pinnedNames: Iterable<string>,
  settings: GlobalModelSettings,
): EvictionCandidate[] {
  const target = targetName.toLowerCase();
  const pinned = new Set([...pinnedNames].map(name => name.toLowerCase()));
  const infos = new Map(allModels.map(model => [modelName(model).toLowerCase(), model] as const));
  const candidates = loadedModels
    .filter(loaded => loaded.model_name.toLowerCase() !== target)
    .filter(loaded => !settings.protectPinnedModels || !pinned.has(loaded.model_name.toLowerCase()))
    .map(loaded => {
      const info = infos.get(loaded.model_name.toLowerCase()) || null;
      return { name: loaded.model_name, loaded, info, estimatedSizeGb: estimatedModelSizeGb(info) };
    });

  if (settings.evictionPolicy === 'largest') {
    return candidates.sort((a, b) => b.estimatedSizeGb - a.estimatedSizeGb || a.loaded.last_use - b.loaded.last_use);
  }
  if (settings.evictionPolicy === 'oldest-process') {
    return candidates.sort((a, b) => a.loaded.pid - b.loaded.pid || a.loaded.last_use - b.loaded.last_use);
  }
  return candidates.sort((a, b) => a.loaded.last_use - b.loaded.last_use || b.estimatedSizeGb - a.estimatedSizeGb);
}

export function estimatedLoadedSizeGb(loadedModels: LoadedModel[], allModels: ModelInfo[]): number {
  const infos = new Map(allModels.map(model => [modelName(model).toLowerCase(), model] as const));
  return loadedModels.reduce((sum, loaded) => sum + estimatedModelSizeGb(infos.get(loaded.model_name.toLowerCase())), 0);
}

export function evictionPlanForLoad(
  loadedModels: LoadedModel[],
  allModels: ModelInfo[],
  target: ModelInfo | null,
  pinnedNames: Iterable<string>,
  settings: GlobalModelSettings,
): string[] {
  if (settings.evictionPolicy === 'manual') return [];
  const targetName = modelName(target);
  const candidates = evictionCandidates(loadedModels, allModels, targetName, pinnedNames, settings);
  if (settings.loadingPolicy === 'single-active') return candidates.map(candidate => candidate.name);
  if (settings.loadingPolicy !== 'budget-aware' || settings.resourceBudgetMode === 'server') return [];

  const budget = settings.resourceBudgetGb;
  let projected = estimatedLoadedSizeGb(loadedModels, allModels) + estimatedModelFootprintGb(target, allModels);
  const plan: string[] = [];
  for (const candidate of candidates) {
    if (projected <= budget) break;
    plan.push(candidate.name);
    projected -= candidate.estimatedSizeGb;
  }
  return plan;
}

export interface GlobalModelLoadPolicyContext<T> {
  loadedModels: LoadedModel[];
  allModels: ModelInfo[];
  target: ModelInfo | null;
  pinnedNames: Iterable<string>;
  settings: GlobalModelSettings;
  unload: (modelName: string) => Promise<unknown>;
  load: () => Promise<T>;
}

/**
 * Apply the client-side model policy around one top-level load operation.
 * Internal component loads can stay inside the supplied callback, so an Omni
 * collection is treated as one unit instead of evicting its own components.
 */
export async function loadWithGlobalModelPolicy<T>(context: GlobalModelLoadPolicyContext<T>): Promise<T> {
  const {
    loadedModels,
    allModels,
    target,
    pinnedNames,
    settings,
    unload,
    load,
  } = context;

  const plannedEvictions = evictionPlanForLoad(
    loadedModels,
    allModels,
    target,
    pinnedNames,
    settings,
  );
  for (const modelNameValue of plannedEvictions) await unload(modelNameValue);

  try {
    return await load();
  } catch (initialError) {
    if (!settings.autoEvictOnPressure
      || settings.evictionPolicy === 'manual'
      || !isMemoryPressureError(initialError)) {
      throw initialError;
    }

    const targetName = modelName(target);
    const alreadyEvicted = new Set(plannedEvictions.map(value => value.toLowerCase()));
    const recoveryCandidates = evictionCandidates(
      loadedModels,
      allModels,
      targetName,
      pinnedNames,
      settings,
    ).filter(candidate => !alreadyEvicted.has(candidate.name.toLowerCase()));

    let lastError: unknown = initialError;
    for (const candidate of recoveryCandidates) {
      await unload(candidate.name);
      try {
        return await load();
      } catch (retryError) {
        lastError = retryError;
        if (!isMemoryPressureError(retryError)) throw retryError;
      }
    }
    throw lastError;
  }
}
