import { storageKey } from '../../storage';

export const ROLE_IDS = [
  'default', 'explore', 'plan', 'solve', 'investigate', 'diagnose', 'code',
  'code_review', 'audit', 'explain', 'advise', 'write', 'creative_write', 'transform',
] as const;
export type SmartRole = typeof ROLE_IDS[number];
export interface SmartRoleState { active: SmartRole; evidence: number[] }
export interface SmartRoleSettings {
  enabled: boolean;
  decisionModel: string;
  override: 'auto' | SmartRole;
}
const KEY = storageKey('smart_roles_settings');
export const SMART_ROLE_SETTINGS_CHANGED = 'lemonade:smart-roles-changed';
export const DEFAULT_SMART_ROLE_SETTINGS: SmartRoleSettings = { enabled: false, decisionModel: '', override: 'auto' };
export function isSmartRole(id: unknown): id is SmartRole {
  return typeof id === 'string' && ROLE_IDS.some(role => role === id);
}
export function loadSmartRoleSettings(): SmartRoleSettings {
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return { ...DEFAULT_SMART_ROLE_SETTINGS };
    const s = JSON.parse(raw) as Partial<SmartRoleSettings>;
    return {
      enabled: s.enabled === true,
      decisionModel: typeof s.decisionModel === 'string' ? s.decisionModel.slice(0, 200) : '',
      override: s.override === 'auto' || isSmartRole(s.override) ? s.override : 'auto',
    };
  } catch { return { ...DEFAULT_SMART_ROLE_SETTINGS }; }
}
export function saveSmartRoleSettings(settings: SmartRoleSettings): void {
  try {
    localStorage.setItem(KEY, JSON.stringify(settings));
    window.dispatchEvent(new Event(SMART_ROLE_SETTINGS_CHANGED));
  } catch { /* optional local preference */ }
}
export function validSmartRoleState(raw: unknown): raw is SmartRoleState {
  if (!raw || typeof raw !== 'object') return false;
  const s = raw as Partial<SmartRoleState>;
  return isSmartRole(s.active) && Array.isArray(s.evidence) && s.evidence.length === ROLE_IDS.length &&
    s.evidence.every(n => typeof n === 'number' && Number.isFinite(n) && n >= 0 && n <= 3);
}
export function smartRoleRequest(settings: SmartRoleSettings, state?: SmartRoleState): Record<string, unknown> | null {
  if (!settings.enabled) return null;
  if (settings.override !== 'auto') return { mode: 'manual', role: settings.override };
  return { mode: 'auto', ...(settings.decisionModel ? { decision_model: settings.decisionModel } : {}),
    ...(state && validSmartRoleState(state) ? { state } : {}) };
}
export const SMART_ROLE_LABELS: Record<SmartRole, string> = {
  default: 'Default', explore: 'Explore', plan: 'Plan', solve: 'Solve', investigate: 'Investigate',
  diagnose: 'Diagnose', code: 'Code', code_review: 'Code Review', audit: 'Audit',
  explain: 'Explain', advise: 'Advise', write: 'Write', creative_write: 'Creative Writing',
  transform: 'Transform',
};
