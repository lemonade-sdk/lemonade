import type { ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { scopedStorageKey } from './accounts/accountStore';

export type LemonadeDefaultModelTier = 'tiny' | 'quality';
export type LemonadeDefaultModelIcon = 'minimize-2' | 'gem';

export interface LemonadeDefaultChatModel {
  name: string;
  tier: LemonadeDefaultModelTier;
  label: string;
  icon: LemonadeDefaultModelIcon;
  description: string;
}

/**
 * The only place that defines GUI3's downloadable chat fallbacks.
 * Replacing a default model must not require touching ChatView behavior.
 */
export const LEMONADE_DEFAULT_CHAT_MODELS: readonly LemonadeDefaultChatModel[] = [
  {
    name: 'Bonsai-8B-gguf',
    tier: 'tiny',
    label: 'default tiny',
    icon: 'minimize-2',
    description: 'Smaller Lemonade default · downloads when first used',
  },
  {
    name: 'Qwen3.5-4B-GGUF',
    tier: 'quality',
    label: 'default quality',
    icon: 'gem',
    description: 'Higher-quality Lemonade default · downloads when first used',
  },
] as const;

export const INITIAL_LEMONADE_DEFAULT_MODEL = LEMONADE_DEFAULT_CHAT_MODELS[0];

const PREFERRED_DEFAULT_KEY = 'chat_preferred_default_model';
const LAST_READY_MODEL_KEY = 'chat_last_ready_model';

function modelName(info: ModelInfo | null | undefined): string {
  return String((info as any)?.model_name || info?.name || info?.id || '').trim();
}

function safeGet(scope: string, key: string): string | null {
  if (typeof localStorage === 'undefined') return null;
  try {
    const value = localStorage.getItem(scopedStorageKey(scope, key));
    return value?.trim() || null;
  } catch {
    return null;
  }
}

function safeSet(scope: string, key: string, value: string): void {
  if (typeof localStorage === 'undefined') return;
  try {
    localStorage.setItem(scopedStorageKey(scope, key), value);
  } catch {
    // Browser storage is best-effort; the in-memory fallback still works.
  }
}

export function lemonadeDefaultModel(name: string | null | undefined): LemonadeDefaultChatModel | null {
  const target = String(name || '').trim().toLowerCase();
  if (!target) return null;
  return LEMONADE_DEFAULT_CHAT_MODELS.find(model => model.name.toLowerCase() === target) || null;
}

export function lemonadeDefaultModelInfo(model: LemonadeDefaultChatModel): ModelInfo {
  return {
    id: model.name,
    name: model.name,
    display_name: model.name,
    recipe: 'llamacpp',
    type: 'llm',
    labels: ['chat', 'lemonade-default', `default-${model.tier}`],
    suggested: true,
    downloaded: false,
    lemonade_default_tier: model.tier,
  };
}

export function loadPreferredDefaultModelName(scope: string): string {
  const stored = safeGet(scope, PREFERRED_DEFAULT_KEY);
  return lemonadeDefaultModel(stored)?.name || INITIAL_LEMONADE_DEFAULT_MODEL.name;
}

export function savePreferredDefaultModelName(scope: string, name: string): string {
  const resolved = lemonadeDefaultModel(name)?.name || INITIAL_LEMONADE_DEFAULT_MODEL.name;
  safeSet(scope, PREFERRED_DEFAULT_KEY, resolved);
  return resolved;
}

export function loadLastReadyModelName(scope: string): string | null {
  return safeGet(scope, LAST_READY_MODEL_KEY);
}

export function saveLastReadyModelName(scope: string, name: string): void {
  const normalized = name.trim();
  if (normalized) safeSet(scope, LAST_READY_MODEL_KEY, normalized);
}

export function modelIsDownloaded(info: ModelInfo | null | undefined): boolean {
  if (!info) return false;
  if ((info as any).downloaded === true || (info as any).is_downloaded === true) return true;
  const status = String((info as any).status || '').trim().toLowerCase();
  if (status === 'downloaded' || status === 'ready' || status === 'local') return true;
  return (info.labels || []).some(label => ['downloaded', 'ready', 'local', 'installed'].includes(String(label).toLowerCase()));
}

export function modelCanAnswerChat(info: ModelInfo | null | undefined): boolean {
  if (!info) return false;
  const capability = capabilityFromModelInfo(info);
  return capability === 'chat' || capability === 'omni';
}

function lastUsedValue(info: ModelInfo): number {
  const raw = (info as any).last_used ?? (info as any).last_use ?? (info as any).updated_at ?? 0;
  if (typeof raw === 'number' && Number.isFinite(raw)) return raw;
  if (typeof raw === 'string') {
    const numeric = Number(raw);
    if (Number.isFinite(numeric)) return numeric;
    const parsed = Date.parse(raw);
    if (Number.isFinite(parsed)) return parsed;
  }
  return 0;
}

/**
 * Finds the previous chat model only while it is still present locally.
 * The exact remembered name wins; registry last-used metadata is a recovery
 * fallback for a cleared browser profile or a model loaded by another client.
 */
export function resolveLastReadyChatModel(
  models: readonly ModelInfo[],
  rememberedName: string | null | undefined,
): ModelInfo | null {
  const ready = models.filter(model => modelIsDownloaded(model) && modelCanAnswerChat(model));
  const remembered = String(rememberedName || '').trim().toLowerCase();
  if (remembered) {
    const exact = ready.find(model => modelName(model).toLowerCase() === remembered);
    if (exact) return exact;
  }
  return [...ready].sort((a, b) => lastUsedValue(b) - lastUsedValue(a))[0] || null;
}

export function modelInfoName(info: ModelInfo | null | undefined): string {
  return modelName(info);
}
