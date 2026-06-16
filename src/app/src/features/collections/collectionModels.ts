import type { LoadedModel, ModelInfo } from '../../api';
import { capabilityFromModelInfo } from '../../modelCapabilities';

export const COLLECTION_OMNI_RECIPE = 'collection.omni';

export function modelInfoName(model: ModelInfo): string {
  return String((model as any).model_name || model.name || model.id || '').trim();
}

export function isCollectionRecipe(recipe?: string | null): boolean {
  const normalized = String(recipe || '').trim().toLowerCase();
  return normalized === COLLECTION_OMNI_RECIPE || normalized === 'collection';
}

export function getCollectionComponents(model?: ModelInfo | null): string[] {
  if (!model) return [];
  const candidates = [
    (model as any).components,
    (model as any).component_models,
    (model as any).composite_models,
    (model as any).recipe_options?.components,
  ];
  const raw = candidates.find(Array.isArray);
  if (!Array.isArray(raw)) return [];
  return Array.from(new Set(raw.filter((item): item is string => typeof item === 'string' && item.trim().length > 0).map(item => item.trim())));
}

export function isCollectionModel(model?: ModelInfo | null): boolean {
  return !!model && isCollectionRecipe((model as any).recipe) && getCollectionComponents(model).length > 0;
}

export function findModelInfoByName(models: ModelInfo[], name?: string | null): ModelInfo | null {
  if (!name) return null;
  const target = name.toLowerCase();
  return models.find(model => modelInfoName(model).toLowerCase() === target) || null;
}

export function isCollectionFullyLoaded(model: ModelInfo, loadedModels: LoadedModel[]): boolean {
  const loaded = new Set(loadedModels.map(m => m.model_name.toLowerCase()));
  const components = getCollectionComponents(model);
  return components.length > 0 && components.every(component => loaded.has(component.toLowerCase()));
}

export function isCollectionFullyDownloaded(model: ModelInfo, allModels: ModelInfo[]): boolean {
  const components = getCollectionComponents(model);
  if (components.length === 0) return false;
  return components.every(component => {
    const info = findModelInfoByName(allModels, component);
    return Boolean(info && (info as any).downloaded === true);
  });
}

export function virtualLoadedCollection(model: ModelInfo, loadedModels: LoadedModel[]): LoadedModel | null {
  if (!isCollectionFullyLoaded(model, loadedModels)) return null;
  const devices = Array.from(new Set(getCollectionComponents(model)
    .map(component => loadedModels.find(loaded => loaded.model_name.toLowerCase() === component.toLowerCase())?.device)
    .filter((device): device is string => typeof device === 'string' && device.length > 0)));
  return {
    model_name: modelInfoName(model),
    checkpoint: String((model as any).checkpoint || ''),
    recipe: String((model as any).recipe || COLLECTION_OMNI_RECIPE),
    device: devices.length === 1 ? devices[0] : (devices.length > 1 ? 'mixed' : ''),
    backend_url: '',
    pid: 0,
    type: 'omni',
    last_use: Date.now(),
    recipe_options: { virtual_collection: true, components: getCollectionComponents(model) },
  };
}

export function withVirtualLoadedCollections(loadedModels: LoadedModel[], models: ModelInfo[]): LoadedModel[] {
  const existing = new Set(loadedModels.map(model => model.model_name.toLowerCase()));
  const virtuals = models
    .filter(isCollectionModel)
    .map(model => virtualLoadedCollection(model, loadedModels))
    .filter((model): model is LoadedModel => !!model && !existing.has(model.model_name.toLowerCase()));
  return [...loadedModels, ...virtuals];
}

export function getPrimaryChatComponent(model: ModelInfo | null | undefined, allModels: ModelInfo[]): string | null {
  const components = getCollectionComponents(model);
  if (components.length === 0) return null;
  const chat = components.find(component => {
    const info = findModelInfoByName(allModels, component);
    const cap = info ? capabilityFromModelInfo(info) : 'unknown';
    return cap === 'chat' || cap === 'omni' || cap === 'unknown';
  });
  return chat || components[0] || null;
}

export function getVisionChatComponent(model: ModelInfo | null | undefined, allModels: ModelInfo[]): string | null {
  const components = getCollectionComponents(model);
  return components.find(component => {
    const info = findModelInfoByName(allModels, component);
    const labels = (info?.labels || []).map(label => label.toLowerCase());
    const cap = info ? capabilityFromModelInfo(info) : 'unknown';
    return cap === 'omni' || labels.includes('vision-language') || labels.includes('image-input') || labels.includes('vlm');
  }) || null;
}

export function getAudioTranscriptionComponent(model: ModelInfo | null | undefined, allModels: ModelInfo[]): string | null {
  const components = getCollectionComponents(model);
  return components.find(component => {
    const info = findModelInfoByName(allModels, component);
    const labels = (info?.labels || []).map(label => label.toLowerCase());
    const cap = info ? capabilityFromModelInfo(info) : 'unknown';
    return cap === 'audio' || labels.includes('transcription') || labels.includes('realtime-transcription') || labels.includes('asr');
  }) || null;
}

export function collectionComponentLabel(model: ModelInfo): string {
  const count = getCollectionComponents(model).length;
  return count === 1 ? '1 component' : `${count} components`;
}
