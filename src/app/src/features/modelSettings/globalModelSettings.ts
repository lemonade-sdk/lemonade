import { storageKey } from '../../storage';

export const GLOBAL_MODEL_SETTINGS_EVENT = 'lemonade:global-model-settings-changed';

export interface GlobalModelSettings {
  collapseThinkingByDefault: boolean;
}

export interface EvictionRuntimeSettings {
  autoEvict: boolean;
  autoEvictThresholdPercent: number;
}

export interface MemoryRuntimeSettings extends EvictionRuntimeSettings {
  maxLoadedModels: number;
}

export const DEFAULT_GLOBAL_MODEL_SETTINGS: GlobalModelSettings = {
  collapseThinkingByDefault: true,
};

function settingsKey(): string {
  return storageKey('global_model_settings');
}

function pinnedModelsKey(): string {
  return storageKey('pinned_models');
}

export function sanitizeGlobalModelSettings(value: unknown): GlobalModelSettings {
  const raw = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Partial<GlobalModelSettings>
    : {};
  return {
    collapseThinkingByDefault: raw.collapseThinkingByDefault !== false,
  };
}

export function loadGlobalModelSettings(): GlobalModelSettings {
  try {
    const raw = localStorage.getItem(settingsKey());
    if (!raw) return { ...DEFAULT_GLOBAL_MODEL_SETTINGS };
    const parsed = JSON.parse(raw);
    const sanitized = sanitizeGlobalModelSettings(parsed);
    // Drop lifecycle fields written by older GUI3 builds. They are server-owned
    // and must never be restored or translated into runtime configuration.
    if (JSON.stringify(parsed) !== JSON.stringify(sanitized)) {
      localStorage.setItem(settingsKey(), JSON.stringify(sanitized));
    }
    return sanitized;
  } catch {
    return { ...DEFAULT_GLOBAL_MODEL_SETTINGS };
  }
}

export function saveGlobalModelSettings(settings: GlobalModelSettings): GlobalModelSettings {
  const sanitized = sanitizeGlobalModelSettings(settings);
  try { localStorage.setItem(settingsKey(), JSON.stringify(sanitized)); } catch { /* best effort */ }
  try {
    window.dispatchEvent(new CustomEvent(GLOBAL_MODEL_SETTINGS_EVENT, { detail: { settings: sanitized } }));
  } catch { /* best effort */ }
  return sanitized;
}

export function loadPinnedModelNames(): string[] {
  try {
    const raw = localStorage.getItem(pinnedModelsKey());
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed)
      ? Array.from(new Set(parsed.map(value => String(value).trim()).filter(Boolean)))
      : [];
  } catch {
    return [];
  }
}

export function savePinnedModelNames(names: Iterable<string>): string[] {
  const normalized = Array.from(new Set([...names].map(name => String(name).trim()).filter(Boolean)));
  try { localStorage.setItem(pinnedModelsKey(), JSON.stringify(normalized)); } catch { /* best effort */ }
  try {
    window.dispatchEvent(new CustomEvent(GLOBAL_MODEL_SETTINGS_EVENT, { detail: { pinnedModelNames: normalized } }));
  } catch { /* best effort */ }
  return normalized;
}

function validMaxLoadedModels(value: unknown): number {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || (parsed < -1 || parsed === 0)) {
    throw new Error('Maximum loaded models per type must be -1 (unlimited) or a positive integer.');
  }
  return parsed;
}

function thresholdFraction(value: unknown): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed <= 0 || parsed > 1) {
    throw new Error('Eviction threshold must be greater than 0% and at most 100%.');
  }
  return parsed;
}

/** Read the effective eviction controls from the server runtime config snapshot. */
export function evictionRuntimeSettingsFromConfig(config: Record<string, unknown>): EvictionRuntimeSettings {
  // These fallbacks mirror RuntimeConfig's effective defaults when the optional
  // keys have not yet been materialized in config.json.
  const autoEvict = typeof config.auto_evict === 'boolean' ? config.auto_evict : false;
  const fraction = config.auto_evict_threshold_pct === undefined
    ? 0.90
    : thresholdFraction(config.auto_evict_threshold_pct);
  return {
    autoEvict,
    autoEvictThresholdPercent: fraction * 100,
  };
}

export function memoryRuntimeSettingsFromConfig(config: Record<string, unknown>): MemoryRuntimeSettings {
  return {
    maxLoadedModels: validMaxLoadedModels(config.max_loaded_models),
    ...evictionRuntimeSettingsFromConfig(config),
  };
}

export function memoryRuntimeConfigChanges(settings: MemoryRuntimeSettings): Record<string, unknown> {
  const maxLoadedModels = validMaxLoadedModels(settings.maxLoadedModels);
  const percent = Number(settings.autoEvictThresholdPercent);
  if (!Number.isFinite(percent) || percent <= 0 || percent > 100) {
    throw new Error('Eviction threshold must be greater than 0% and at most 100%.');
  }
  return {
    max_loaded_models: maxLoadedModels,
    auto_evict: settings.autoEvict,
    auto_evict_threshold_pct: percent / 100,
  };
}

export function autoCheckModelUpdatesFromConfig(config: Record<string, unknown>): boolean {
  return typeof config.auto_check_model_updates === 'boolean'
    ? config.auto_check_model_updates
    : true;
}
