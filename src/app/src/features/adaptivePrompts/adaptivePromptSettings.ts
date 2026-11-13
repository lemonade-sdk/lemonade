import { storageKey } from '../../storage';

export type AdaptivePromptMode = 'rules' | 'llm' | 'manual';

export interface AdaptivePromptSettings {
  enabled: boolean;
  mode: AdaptivePromptMode;
  decisionModel: string;
  manualIds: string[];
}

export interface AdaptivePromptModuleInfo {
  id: string;
  type: 'role' | 'skill';
  group: string;
  excludes?: string[]; // older servers may omit compatibility metadata
  description: string;
  prompt: string;
  keywords: string[];
}

export interface AdaptivePromptCatalogResponse {
  source: 'built-in' | 'custom';
  count: number;
  modules: AdaptivePromptModuleInfo[];
}

export const ADAPTIVE_PROMPT_SELECTION_EVENT = 'lemonade:adaptive-prompt-selection';
export const DEFAULT_ADAPTIVE_PROMPT_SETTINGS: AdaptivePromptSettings = {
  enabled: false,
  mode: 'rules',
  decisionModel: '',
  manualIds: [],
};

const SETTINGS_KEY = storageKey('adaptive_prompt_settings');
const LAST_SELECTION_KEY = storageKey('adaptive_prompt_last_selection');
const ID = /^[a-z0-9_-]{1,64}$/;

export function sanitizeAdaptivePromptSettings(value: unknown): AdaptivePromptSettings {
  const input = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Partial<AdaptivePromptSettings> : {};
  return {
    enabled: input.enabled === true,
    mode: input.mode === 'llm' || input.mode === 'manual' ? input.mode : 'rules',
    decisionModel: typeof input.decisionModel === 'string' ? input.decisionModel.slice(0, 200) : '',
    manualIds: Array.isArray(input.manualIds)
      ? [...new Set(input.manualIds.filter((v): v is string => typeof v === 'string' && ID.test(v)))].slice(0, 3)
      : [],
  };
}

export function loadAdaptivePromptSettings(): AdaptivePromptSettings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    return raw ? sanitizeAdaptivePromptSettings(JSON.parse(raw)) : { ...DEFAULT_ADAPTIVE_PROMPT_SETTINGS, manualIds: [] };
  } catch {
    return { ...DEFAULT_ADAPTIVE_PROMPT_SETTINGS, manualIds: [] };
  }
}

export function saveAdaptivePromptSettings(settings: AdaptivePromptSettings): AdaptivePromptSettings {
  const next = sanitizeAdaptivePromptSettings(settings);
  try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(next)); } catch { /* non-critical local preference */ }
  return next;
}

// Absent when disabled: unchanged server/API behavior for other clients.
export function adaptivePromptRequest(settings: AdaptivePromptSettings): Record<string, unknown> | null {
  if (!settings.enabled) return null;
  if (settings.mode === 'manual') return { mode: 'manual', ids: settings.manualIds, trace: true };
  if (settings.mode === 'llm') return {
    mode: 'llm',
    ...(settings.decisionModel ? { decision_model: settings.decisionModel } : {}),
    trace: true,
  };
  return { mode: 'rules', trace: true };
}

// Match the server's role/group/excludes rules before submitting manual IDs.
// The newly clicked module wins over conflicting earlier choices.
export function updateManualAdaptiveIds(
  selected: string[], id: string, modules: AdaptivePromptModuleInfo[],
): string[] {
  if (selected.includes(id)) return selected.filter(value => value !== id);
  const picked = modules.find(module => module.id === id);
  if (!picked) return selected;
  const retained = selected.filter(previousId => {
    const previous = modules.find(module => module.id === previousId);
    if (!previous) return false;
    if (picked.type === 'role' && previous.type === 'role') return false;
    if (picked.group && picked.group === previous.group) return false;
    if (picked.excludes?.includes(previousId) || previous.excludes?.includes(id)) return false;
    return true;
  });
  if (picked.type === 'skill' && retained.filter(previousId =>
    modules.find(module => module.id === previousId)?.type === 'skill').length >= 2) {
    return selected; // no slot; user may uncheck an existing skill first
  }
  return [...retained, id];
}

export function recordAdaptivePromptSelection(ids: string[]): void {
  const selection = ids.filter(id => ID.test(id));
  try { localStorage.setItem(LAST_SELECTION_KEY, JSON.stringify(selection)); } catch { /* non-critical */ }
  try { window.dispatchEvent(new CustomEvent(ADAPTIVE_PROMPT_SELECTION_EVENT, { detail: selection })); } catch { /* non-critical */ }
}

export function loadLastAdaptivePromptSelection(): string[] | null {
  try {
    const raw = localStorage.getItem(LAST_SELECTION_KEY);
    if (raw === null) return null;
    const parsed: unknown = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((id): id is string => typeof id === 'string' && ID.test(id)) : null;
  } catch { return null; }
}
