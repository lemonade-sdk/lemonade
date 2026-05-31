import type { ModelInfo } from '../../api';
import type { ModelCapability } from '../../modelCapabilities';
import { scopedStorageKey } from '../accounts/accountStore';
import { COLLECTION_OMNI_RECIPE } from '../collections/collectionModels';

export type CustomModelCapability = Extract<ModelCapability, 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding' | 'reranking'>;

export interface CustomModelComponentRoles {
  llm?: string;
  vision?: string;
  image?: string;
  edit?: string;
  transcription?: string;
  speech?: string;
}

export interface CustomModelRecord {
  id: string;
  name: string;
  display_name: string;
  checkpoint: string;
  recipe: string;
  type: CustomModelCapability;
  labels: string[];
  downloaded: boolean;
  custom: true;
  max_context_window?: number;
  components?: string[];
  component_roles?: CustomModelComponentRoles;
  createdAt: number;
  updatedAt: number;
}

export interface CustomModelDraft {
  name: string;
  displayName: string;
  checkpoint: string;
  recipe: string;
  capability: CustomModelCapability;
  maxContextWindow?: number;
  labels?: string[];
  components?: string[];
  componentRoles?: CustomModelComponentRoles;
}

const CUSTOM_MODELS_KEY = 'custom_models';

function normalizeModelName(name: string): string {
  const cleaned = name.trim().replace(/\s+/g, '-').replace(/[^a-zA-Z0-9._\-/]/g, '-').replace(/-+/g, '-');
  if (!cleaned) return '';
  return cleaned.startsWith('user.') ? cleaned : `user.${cleaned}`;
}

function normalizeComponentName(value?: string): string {
  return String(value || '').trim();
}

function defaultRecipe(capability: CustomModelCapability, components?: string[]): string {
  switch (capability) {
    case 'image': return 'sd-cpp';
    case 'audio': return 'whispercpp';
    case 'tts': return 'kokoro';
    case 'embedding': return 'llamacpp';
    case 'reranking': return 'llamacpp';
    case 'omni': return components && components.length > 0 ? COLLECTION_OMNI_RECIPE : 'llamacpp';
    default: return 'llamacpp';
  }
}

function labelsFor(capability: CustomModelCapability, extra: string[] = []): string[] {
  const base = ['custom'];
  switch (capability) {
    case 'chat': base.push('chat'); break;
    case 'omni': base.push('omni', 'multimodal', 'vision-language'); break;
    case 'image': base.push('image'); break;
    case 'audio': base.push('audio', 'transcription'); break;
    case 'tts': base.push('tts'); break;
    case 'embedding': base.push('embedding'); break;
    case 'reranking': base.push('reranking'); break;
  }
  return [...new Set([...base, ...extra.map(l => l.trim().toLowerCase()).filter(Boolean)])];
}

function isRecord(value: unknown): value is CustomModelRecord {
  if (!value || typeof value !== 'object') return false;
  const obj = value as Record<string, unknown>;
  return typeof obj.id === 'string'
    && typeof obj.name === 'string'
    && typeof obj.checkpoint === 'string'
    && typeof obj.recipe === 'string'
    && typeof obj.type === 'string'
    && obj.custom === true;
}

export function loadCustomModels(scope: string): CustomModelRecord[] {
  try {
    const raw = localStorage.getItem(scopedStorageKey(scope, CUSTOM_MODELS_KEY));
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    const items = Array.isArray(parsed?.models) ? parsed.models : [];
    return items.filter(isRecord);
  } catch { return []; }
}

function saveCustomModels(scope: string, models: CustomModelRecord[]): void {
  localStorage.setItem(scopedStorageKey(scope, CUSTOM_MODELS_KEY), JSON.stringify({ version: 2, models }));
}

export function upsertCustomModel(scope: string, draft: CustomModelDraft): CustomModelRecord {
  const now = Date.now();
  const normalizedComponents = Array.from(new Set((draft.components || []).map(normalizeComponentName).filter(Boolean)));
  const componentRoles: CustomModelComponentRoles = {
    llm: normalizeComponentName(draft.componentRoles?.llm),
    vision: normalizeComponentName(draft.componentRoles?.vision),
    image: normalizeComponentName(draft.componentRoles?.image),
    edit: normalizeComponentName(draft.componentRoles?.edit),
    transcription: normalizeComponentName(draft.componentRoles?.transcription),
    speech: normalizeComponentName(draft.componentRoles?.speech),
  };
  const explicitRoleComponents = Object.values(componentRoles).filter(Boolean) as string[];
  const components = Array.from(new Set([...normalizedComponents, ...explicitRoleComponents]));
  const name = normalizeModelName(draft.name);
  if (name.length < 7) throw new Error('Custom model name must contain at least 2 characters after the user. prefix.');
  const capability = draft.capability;
  const hasCheckpoint = draft.checkpoint.trim().length > 0;
  const isCollectionOmni = capability === 'omni' && components.length > 0;
  if (!hasCheckpoint && !isCollectionOmni) throw new Error('Checkpoint, repo id, or local model path is required. Omni collections can instead reference existing component model names.');

  const current = loadCustomModels(scope);
  const existing = current.find(m => m.name.toLowerCase() === name.toLowerCase());
  const record: CustomModelRecord = {
    id: existing?.id || `custom.${now.toString(36)}.${Math.random().toString(36).slice(2, 8)}`,
    name,
    display_name: draft.displayName.trim() || name,
    checkpoint: draft.checkpoint.trim(),
    recipe: draft.recipe.trim() || defaultRecipe(capability, components),
    type: capability,
    labels: labelsFor(capability, draft.labels || []),
    downloaded: true,
    custom: true,
    max_context_window: draft.maxContextWindow,
    components: components.length ? components : undefined,
    component_roles: Object.fromEntries(Object.entries(componentRoles).filter(([, value]) => Boolean(value))) as CustomModelComponentRoles,
    createdAt: existing?.createdAt || now,
    updatedAt: now,
  };
  if (Object.keys(record.component_roles || {}).length === 0) delete record.component_roles;
  if (record.type === 'omni' && record.components?.length) {
    record.recipe = COLLECTION_OMNI_RECIPE;
    record.checkpoint = record.checkpoint || record.components[0];
  }
  saveCustomModels(scope, [record, ...current.filter(m => m.id !== record.id && m.name.toLowerCase() !== name.toLowerCase())]);
  return record;
}

export function deleteCustomModel(scope: string, idOrName: string): void {
  const current = loadCustomModels(scope);
  saveCustomModels(scope, current.filter(m => m.id !== idOrName && m.name !== idOrName));
}

export function customModelToModelInfo(record: CustomModelRecord): ModelInfo {
  return {
    id: record.name,
    name: record.name,
    display_name: record.display_name,
    checkpoint: record.checkpoint,
    recipe: record.recipe,
    type: record.type,
    labels: record.labels,
    downloaded: true,
    custom: true,
    max_context_window: record.max_context_window,
    components: record.components,
    component_roles: record.component_roles,
    createdAt: new Date(record.createdAt).toISOString(),
  };
}

export function customRegistrationOptions(model: ModelInfo): Record<string, unknown> | undefined {
  if (!(model as any).custom) return undefined;
  const checkpoint = String((model as any).checkpoint || '').trim();
  const recipe = String((model as any).recipe || '').trim();
  const type = String((model as any).type || '').trim();
  const labels = Array.isArray(model.labels) ? model.labels : [];
  const components = Array.isArray((model as any).components) ? (model as any).components.filter((c: unknown): c is string => typeof c === 'string' && c.trim().length > 0) : [];
  const opts: Record<string, unknown> = { custom: true };
  if (checkpoint) opts.checkpoint = checkpoint;
  if (recipe) opts.recipe = recipe;
  if (type) opts.type = type;
  if (labels.length) opts.labels = labels;
  if (components.length) opts.components = components;
  if ((model as any).component_roles) opts.component_roles = (model as any).component_roles;
  if ((model as any).max_context_window) opts.ctx_size = (model as any).max_context_window;
  return opts;
}

export function customLoadOptions(model: ModelInfo): Record<string, unknown> | undefined {
  if (!(model as any).custom) return undefined;
  const opts = customRegistrationOptions(model) || { custom: true };
  return { ...opts, save_options: false };
}

export const CUSTOM_CAPABILITIES: Array<{ value: CustomModelCapability; label: string; hint: string }> = [
  { value: 'chat', label: 'Chat', hint: 'Text chat through /chat/completions' },
  { value: 'omni', label: 'Omni', hint: 'Multimodal model or Omni collection of existing text/vision/audio models' },
  { value: 'image', label: 'Image', hint: 'Image generation endpoint' },
  { value: 'audio', label: 'Audio', hint: 'Audio transcription endpoint' },
  { value: 'tts', label: 'TTS', hint: 'Text-to-speech endpoint' },
  { value: 'embedding', label: 'Embedding', hint: 'Utility model; not selectable in composer' },
  { value: 'reranking', label: 'Reranking', hint: 'Utility model; not selectable in composer' },
];
