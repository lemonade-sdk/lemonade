import type { ModelInfo } from '../../api';
import type { ModelCapability } from '../../modelCapabilities';
import { scopedStorageKey } from '../accounts/accountStore';
import { COLLECTION_OMNI_RECIPE } from '../collections/collectionModels';
import { routerRegistrationOptions } from '../router/routerStore';

export type CustomModelCapability = Extract<ModelCapability, 'chat' | 'omni' | 'image' | 'audio' | 'audio-generation' | 'tts' | 'model3d' | 'embedding' | 'reranking'>;

export interface CustomModelComponentRoles {
  llm?: string;
  vision?: string;
  image?: string;
  edit?: string;
  transcription?: string;
  speech?: string;
}

export interface CustomOmniToolDefinition {
  id?: string;
  name: string;
  description: string;
  target_model: string;
  system_prompt?: string;
  prompt_template?: string;
  parameters?: Record<string, unknown>;
  max_tokens?: number;
}

export interface CustomModelRecord {
  id: string;
  name: string;
  display_name: string;
  checkpoint: string;
  checkpoints?: Record<string, string>;
  mmproj?: string;
  recipe: string;
  type: CustomModelCapability;
  labels: string[];
  downloaded: boolean;
  custom: true;
  max_context_window?: number;
  components?: string[];
  component_roles?: CustomModelComponentRoles;
  recipe_options?: Record<string, unknown>;
  system_prompt?: string;
  custom_tools?: CustomOmniToolDefinition[];
  createdAt: number;
  updatedAt: number;
}

export interface CustomModelDraft {
  name: string;
  displayName: string;
  checkpoint: string;
  checkpoints?: Record<string, string>;
  mmproj?: string;
  recipe: string;
  capability: CustomModelCapability;
  maxContextWindow?: number;
  labels?: string[];
  components?: string[];
  componentRoles?: CustomModelComponentRoles;
  recipeOptions?: Record<string, unknown>;
  system_prompt?: string;
  customTools?: CustomOmniToolDefinition[];
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

function normalizeToolName(value?: string): string {
  return String(value || '')
    .trim()
    .replace(/\s+/g, '_')
    .replace(/[^a-zA-Z0-9_-]/g, '_')
    .replace(/_+/g, '_')
    .replace(/^([^a-zA-Z_])/, '_$1')
    .slice(0, 64);
}

function normalizeCustomTools(value?: CustomOmniToolDefinition[]): CustomOmniToolDefinition[] {
  if (!Array.isArray(value)) return [];
  const seen = new Set<string>();
  const tools: CustomOmniToolDefinition[] = [];
  for (const raw of value) {
    if (!raw || typeof raw !== 'object') continue;
    const anyRaw = raw as CustomOmniToolDefinition & { targetModel?: string; model?: string };
    const name = normalizeToolName(anyRaw.name);
    const targetModel = normalizeComponentName(anyRaw.target_model || anyRaw.targetModel || anyRaw.model);
    if (!name || !targetModel || seen.has(name.toLowerCase())) continue;
    seen.add(name.toLowerCase());
    const description = String(raw.description || `Ask ${targetModel}`).trim();
    const parameters = isPlainObject(raw.parameters) ? { ...raw.parameters } : undefined;
    const maxTokens = Number(raw.max_tokens);
    tools.push({
      id: String(raw.id || name),
      name,
      description: description || `Ask ${targetModel}`,
      target_model: targetModel,
      system_prompt: String(raw.system_prompt || '').trim() || undefined,
      prompt_template: String(raw.prompt_template || '').trim() || undefined,
      parameters,
      max_tokens: Number.isFinite(maxTokens) && maxTokens > 0 ? Math.floor(maxTokens) : undefined,
    });
  }
  return tools;
}

function defaultRecipe(capability: CustomModelCapability, components?: string[]): string {
  switch (capability) {
    case 'image': return 'sd-cpp';
    case 'audio': return 'whispercpp';
    case 'audio-generation': return 'thinksound';
    case 'tts': return 'kokoro';
    case 'model3d': return 'trellis';
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
    case 'audio-generation': base.push('audio-generation'); break;
    case 'tts': base.push('tts'); break;
    case 'model3d': base.push('3d', 'image-to-3d'); break;
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
  const customTools = normalizeCustomTools(draft.customTools);
  const customToolComponents = customTools.map(tool => tool.target_model);
  const normalizedComponents = Array.from(new Set([...(draft.components || []), ...customToolComponents].map(normalizeComponentName).filter(Boolean)));
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
    checkpoints: draft.checkpoints && Object.keys(draft.checkpoints).length ? draft.checkpoints : undefined,
    mmproj: draft.mmproj?.trim() || undefined,
    recipe: draft.recipe.trim() || defaultRecipe(capability, components),
    type: capability,
    labels: labelsFor(capability, draft.labels || []),
    downloaded: true,
    custom: true,
    max_context_window: draft.maxContextWindow,
    components: components.length ? components : undefined,
    component_roles: Object.fromEntries(Object.entries(componentRoles).filter(([, value]) => Boolean(value))) as CustomModelComponentRoles,
    recipe_options: isPlainObject(draft.recipeOptions) && Object.keys(draft.recipeOptions).length ? { ...draft.recipeOptions } : undefined,
    system_prompt: draft.system_prompt?.trim() || undefined,
    custom_tools: customTools.length ? customTools : undefined,
    createdAt: existing?.createdAt || now,
    updatedAt: now,
  };
  if (Object.keys(record.component_roles || {}).length === 0) delete record.component_roles;
  if (record.recipe_options && Object.keys(record.recipe_options).length === 0) delete record.recipe_options;
  if (record.type === 'omni' && record.components?.length) {
    record.recipe = COLLECTION_OMNI_RECIPE;
    record.checkpoint = '';
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
    checkpoints: record.checkpoints,
    mmproj: record.mmproj,
    recipe: record.recipe,
    type: record.type,
    labels: record.labels,
    downloaded: true,
    custom: true,
    max_context_window: record.max_context_window,
    components: record.components,
    component_roles: record.component_roles,
    recipe_options: record.recipe_options,
    system_prompt: record.system_prompt,
    custom_tools: record.custom_tools,
    createdAt: new Date(record.createdAt).toISOString(),
  };
}

export function customRegistrationOptions(model: ModelInfo): Record<string, unknown> | undefined {
  const routerOptions = routerRegistrationOptions(model);
  if (routerOptions) return routerOptions;
  if (!(model as any).custom) return undefined;
  const checkpoint = String((model as any).checkpoint || '').trim();
  const recipe = String((model as any).recipe || '').trim();
  const type = String((model as any).type || '').trim();
  const labels = Array.isArray(model.labels) ? model.labels.map(label => String(label).trim()).filter(Boolean) : [];
  const serverLabels = Array.from(new Set(labels.filter(label => label !== 'custom')));
  const components = Array.isArray((model as any).components) ? (model as any).components.filter((c: unknown): c is string => typeof c === 'string' && c.trim().length > 0) : [];
  if ((recipe === COLLECTION_OMNI_RECIPE || type === 'omni') && components.length) {
    const opts: Record<string, unknown> = { recipe: COLLECTION_OMNI_RECIPE, components };
    const systemPrompt = String((model as any).system_prompt || '').trim();
    if (systemPrompt) opts.system_prompt = systemPrompt;
    if (Array.isArray((model as any).custom_tools) && (model as any).custom_tools.length) opts.custom_tools = (model as any).custom_tools;
    if (serverLabels.length) opts.labels = serverLabels;
    return opts;
  }

  const opts: Record<string, unknown> = { custom: true };
  if (serverLabels.length) opts.labels = serverLabels;
  const checkpoints = isPlainObject((model as any).checkpoints) ? (model as any).checkpoints as Record<string, unknown> : null;
  if (checkpoints && Object.keys(checkpoints).length > 0) {
    opts.checkpoints = Object.fromEntries(Object.entries(checkpoints).filter(([, value]) => typeof value === 'string' && value.trim()).map(([key, value]) => [key, String(value).trim()]));
  } else if (checkpoint) {
    opts.checkpoint = checkpoint;
  }
  if (recipe) opts.recipe = recipe;
  if (isPlainObject((model as any).recipe_options)) opts.recipe_options = { ...(model as any).recipe_options };
  // Current /v1/pull registration uses capability booleans rather than a generic type/labels payload.
  if (labels.includes('reasoning')) opts.reasoning = true;
  if (labels.some(label => ['vision', 'omni', 'multimodal', 'vision-language', 'image-input'].includes(label))) opts.vision = true;
  if (type === 'embedding' || labels.some(label => label === 'embedding' || label === 'embeddings')) opts.embedding = true;
  if (type === 'reranking' || labels.some(label => label === 'reranking' || label === 'reranker')) opts.reranking = true;
  const mmproj = String((model as any).mmproj || '').trim();
  if (mmproj && !(checkpoints && Object.prototype.hasOwnProperty.call(checkpoints, 'mmproj'))) opts.mmproj = mmproj;
  if (type !== 'omni' && (model as any).max_context_window) opts.ctx_size = (model as any).max_context_window;
  return opts;
}

export function customLoadOptions(model: ModelInfo): Record<string, unknown> | undefined {
  if (!(model as any).custom) return undefined;
  const opts: Record<string, unknown> = { save_options: false };
  if (isPlainObject((model as any).recipe_options)) Object.assign(opts, (model as any).recipe_options);
  const type = String((model as any).type || '').trim();
  if (type !== 'omni' && (model as any).max_context_window) opts.ctx_size = (model as any).max_context_window;
  return opts;
}

export const CUSTOM_CAPABILITIES: Array<{ value: CustomModelCapability; label: string; hint: string }> = [
  { value: 'chat', label: 'Chat', hint: 'Text chat through /chat/completions' },
  { value: 'omni', label: 'Omni', hint: 'Multimodal model or Omni collection of existing text/vision/audio models' },
  { value: 'image', label: 'Image', hint: 'Image generation endpoint' },
  { value: 'audio', label: 'Transcription', hint: 'Audio transcription endpoint' },
  { value: 'audio-generation', label: 'Audio generation', hint: 'Music or sound effects through /audio/generations' },
  { value: 'tts', label: 'TTS', hint: 'Text-to-speech endpoint' },
  { value: 'model3d', label: '3D', hint: 'Image-to-3D reconstruction through /3d/generations' },
  { value: 'embedding', label: 'Embedding', hint: 'Utility model; not selectable in composer' },
  { value: 'reranking', label: 'Reranking', hint: 'Utility model; not selectable in composer' },
];

export interface CustomModelImportResult {
  imported: number;
  skipped: number;
  errors: string[];
}

function valueString(source: Record<string, unknown>, keys: string[]): string {
  for (const key of keys) {
    const value = source[key];
    if (typeof value === 'string' && value.trim()) return value.trim();
  }
  return '';
}

function valueNumber(source: Record<string, unknown>, keys: string[]): number | undefined {
  for (const key of keys) {
    const value = source[key];
    const n = Number(value);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return undefined;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function valueStringArray(value: unknown): string[] {
  if (Array.isArray(value)) return value.map(v => String(v).trim()).filter(Boolean);
  if (typeof value === 'string') return value.split(',').map(v => v.trim()).filter(Boolean);
  return [];
}

function normalizeImportedCapability(raw: string, labels: string[]): CustomModelCapability {
  const lower = raw.toLowerCase().trim();
  const labelText = labels.join(' ').toLowerCase();
  if (['omni', 'multimodal', 'vlm', 'vision'].includes(lower) || labelText.includes('omni') || labelText.includes('vision-language')) return 'omni';
  if (['3d', 'model3d', '3d-generation', 'image-to-3d'].includes(lower) || labelText.includes('image-to-3d') || labelText.includes('3d')) return 'model3d';
  if (['audio-generation', 'music-generation', 'sound-generation', 'sfx'].includes(lower) || labelText.includes('audio-generation')) return 'audio-generation';
  if (['image', 'image-generation', 'diffusion'].includes(lower) || labelText.includes('image')) return 'image';
  if (['audio', 'transcription', 'asr', 'stt'].includes(lower) || labelText.includes('transcription')) return 'audio';
  if (['tts', 'speech', 'text-to-speech'].includes(lower) || labelText.includes('tts')) return 'tts';
  if (['embedding', 'embeddings'].includes(lower) || labelText.includes('embedding')) return 'embedding';
  if (['reranking', 'reranker', 'rerank'].includes(lower) || labelText.includes('reranking')) return 'reranking';
  return 'chat';
}

function normalizeImportedRecord(raw: unknown, index: number): CustomModelDraft | null {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const source = raw as Record<string, unknown>;
  const labels = valueStringArray(source.labels);
  const components = valueStringArray(source.components);
  const rolesRaw = source.component_roles || source.componentRoles;
  const componentRoles = rolesRaw && typeof rolesRaw === 'object' && !Array.isArray(rolesRaw)
    ? rolesRaw as CustomModelComponentRoles
    : undefined;
  const capability = normalizeImportedCapability(valueString(source, ['capability', 'type', 'kind']), labels);
  const displayName = valueString(source, ['displayName', 'display_name', 'title']) || valueString(source, ['name', 'model_name', 'id']) || `Imported model ${index + 1}`;
  const rawCheckpoints = isPlainObject(source.checkpoints) ? source.checkpoints : undefined;
  const checkpoints = rawCheckpoints
    ? Object.fromEntries(Object.entries(rawCheckpoints).filter(([, value]) => typeof value === 'string' && value.trim()).map(([key, value]) => [key, String(value).trim()]))
    : undefined;
  const checkpoint = valueString(source, ['checkpoint', 'path', 'repo', 'model_path', 'modelPath'])
    || (checkpoints?.main ?? Object.values(checkpoints || {})[0] ?? '');
  const mmproj = valueString(source, ['mmproj']) || checkpoints?.mmproj;
  const name = valueString(source, ['name', 'model_name', 'id']) || displayName;
  const recipe = valueString(source, ['recipe', 'backend']) || defaultRecipe(capability, components);
  const recipeOptions = isPlainObject(source.recipe_options) ? source.recipe_options : undefined;
  const systemPrompt = valueString(source, ['system_prompt', 'systemPrompt']);
  const customToolsRaw = Array.isArray(source.custom_tools) ? source.custom_tools : (Array.isArray(source.customTools) ? source.customTools : []);
  const customTools = normalizeCustomTools(customToolsRaw as CustomOmniToolDefinition[]);
  return {
    name,
    displayName,
    checkpoint,
    checkpoints,
    mmproj,
    recipe,
    capability,
    maxContextWindow: valueNumber(source, ['maxContextWindow', 'max_context_window', 'ctx_size']),
    labels,
    components,
    componentRoles,
    recipeOptions,
    system_prompt: systemPrompt || undefined,
    customTools,
  };
}

export function exportCustomModelsPayload(scope: string): Record<string, unknown> {
  return {
    version: 2,
    exportedAt: new Date().toISOString(),
    models: loadCustomModels(scope),
  };
}

function looksLikeModelPayload(value: unknown): boolean {
  if (!isPlainObject(value)) return false;
  return ['model_name', 'name', 'id', 'display_name', 'checkpoint', 'checkpoints', 'components', 'recipe'].some(key => key in value);
}

function importItemsFromPayload(payload: unknown): unknown[] {
  if (Array.isArray(payload)) return payload;
  if (!isPlainObject(payload)) return payload ? [payload] : [];
  const embedded = Array.isArray(payload.models) ? payload.models : [];
  return looksLikeModelPayload(payload) ? [payload, ...embedded] : embedded;
}

export function importCustomModels(scope: string, payload: unknown): CustomModelImportResult {
  const rawItems = importItemsFromPayload(payload);
  const result: CustomModelImportResult = { imported: 0, skipped: 0, errors: [] };
  rawItems.forEach((item, index) => {
    const draft = normalizeImportedRecord(item, index);
    if (!draft) {
      result.skipped += 1;
      result.errors.push(`Entry ${index + 1} is not a model object.`);
      return;
    }
    try {
      upsertCustomModel(scope, draft);
      result.imported += 1;
    } catch (err) {
      result.skipped += 1;
      result.errors.push(`Entry ${index + 1}: ${err instanceof Error ? err.message : String(err)}`);
    }
  });
  return result;
}
