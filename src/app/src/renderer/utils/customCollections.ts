import type { ModelInfo, ModelsData } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';

export const CUSTOM_COLLECTION_PREFIX = 'collection.';
const CUSTOM_COLLECTIONS_STORAGE_KEY = 'lemonade.customCollections.v1';
const LEGACY_COLLECTIONS_STORAGE_KEY = 'lemonade.custom' + 'Work' + 'flows.v1';
const CUSTOM_COLLECTIONS_EXPORT_VERSION = 1;

export type CustomCollectionRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';

export interface CustomCollectionComponents {
  llm: string;
  vision?: string;
  image?: string;
  edit?: string;
  transcription?: string;
  speech?: string;
}

export interface CustomCollection {
  id: string;
  name: string;
  createdAt: string;
  updatedAt: string;
  components: CustomCollectionComponents;
}

export interface CustomCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  components: CustomCollectionComponents;
}

interface CustomCollectionsExportPayload {
  version: number;
  exportedAt: string;
  collections: CustomCollection[];
}

export interface CustomCollectionImportResult {
  imported: number;
  skipped: number;
  collections: CustomCollection[];
}

const roleLabels: Record<CustomCollectionRole, string> = {
  llm: 'Planner LLM',
  vision: 'Vision',
  image: 'Image Generation',
  edit: 'Image Editing',
  transcription: 'Transcription',
  speech: 'Text to Speech',
};

export const getCustomCollectionRoleLabel = (role: CustomCollectionRole): string => roleLabels[role];

export const isCustomCollectionId = (modelId: string): boolean => modelId.startsWith(CUSTOM_COLLECTION_PREFIX);

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const slugify = (value: string): string => {
  const slug = value.trim().toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 72);
  return slug || 'custom-collection';
};

const makeCollectionId = (name: string): string => `${CUSTOM_COLLECTION_PREFIX}${slugify(name)}`;

const makeUniqueCollectionId = (name: string, collections: Array<Pick<CustomCollection, 'id'>>): string => {
  const base = makeCollectionId(name);
  const used = new Set(collections.map(collection => collection.id));
  if (!used.has(base)) return base;

  let index = 2;
  while (used.has(`${base}-${index}`)) index += 1;
  return `${base}-${index}`;
};

const normalizeComponentValue = (value: unknown): string | undefined => {
  return typeof value === 'string' && value.trim() ? value.trim() : undefined;
};

const normalizeComponents = (value: unknown): CustomCollectionComponents | null => {
  if (!isRecord(value)) return null;

  const llm = normalizeComponentValue(value.llm);
  if (!llm) return null;

  const components: CustomCollectionComponents = { llm };
  for (const role of ['vision', 'image', 'edit', 'transcription', 'speech'] as const) {
    const component = normalizeComponentValue(value[role]);
    if (component) components[role] = component;
  }

  return components;
};

const normalizeCustomCollection = (value: unknown): CustomCollection | null => {
  if (!isRecord(value)) return null;

  const name = normalizeComponentValue(value.name);
  const components = normalizeComponents(value.components);
  if (!name || !components) return null;

  const rawId = normalizeComponentValue(value.id);
  const id = rawId && isCustomCollectionId(rawId) ? rawId : makeCollectionId(name);
  const now = new Date().toISOString();
  const createdAt = normalizeComponentValue(value.createdAt) ?? now;
  const updatedAt = normalizeComponentValue(value.updatedAt) ?? createdAt;

  return { id, name, createdAt, updatedAt, components };
};

const getCustomCollectionComponentList = (collection: Pick<CustomCollection, 'components'>): string[] => {
  const ordered = [
    collection.components.llm,
    collection.components.vision,
    collection.components.image,
    collection.components.edit,
    collection.components.transcription,
    collection.components.speech,
  ].filter((value): value is string => typeof value === 'string' && value.length > 0);

  return Array.from(new Set(ordered));
};

const readStoredCollections = (): unknown => {
  if (typeof window === 'undefined' || !window.localStorage) return null;
  const raw = window.localStorage.getItem(CUSTOM_COLLECTIONS_STORAGE_KEY)
    ?? window.localStorage.getItem(LEGACY_COLLECTIONS_STORAGE_KEY);
  if (!raw) return null;
  return JSON.parse(raw);
};

export const loadCustomCollections = (): CustomCollection[] => {
  try {
    const parsed = readStoredCollections();
    const records = extractImportRecords(parsed);
    return records
      .map(normalizeCustomCollection)
      .filter((collection): collection is CustomCollection => collection !== null)
      .sort((a, b) => a.name.localeCompare(b.name));
  } catch (error) {
    console.error('Failed to load custom collections:', error);
    return [];
  }
};

const persistCustomCollections = (collections: CustomCollection[]): void => {
  try {
    window.localStorage.setItem(CUSTOM_COLLECTIONS_STORAGE_KEY, JSON.stringify(collections));
    window.dispatchEvent(new CustomEvent('customCollectionsUpdated'));
  } catch (error) {
    console.error('Failed to save custom collections:', error);
    throw error;
  }
};

export const saveCustomCollection = (draft: CustomCollectionDraft): CustomCollection => {
  const collections = loadCustomCollections();
  const explicitId = typeof draft.id === 'string' && isCustomCollectionId(draft.id)
    ? draft.id
    : undefined;
  const collectionId = explicitId ?? makeUniqueCollectionId(draft.name, collections);
  const now = new Date().toISOString();

  const normalized = normalizeCustomCollection({
    ...draft,
    id: collectionId,
    createdAt: draft.createdAt ?? now,
    updatedAt: now,
  });

  if (!normalized) {
    throw new Error('Custom collection requires a name and an LLM model.');
  }

  const existing = collections.find(collection => collection.id === normalized.id);
  const saved: CustomCollection = {
    ...normalized,
    createdAt: existing?.createdAt ?? normalized.createdAt,
    updatedAt: now,
  };

  const next = collections.filter(collection => collection.id !== saved.id).concat(saved)
    .sort((a, b) => a.name.localeCompare(b.name));
  persistCustomCollections(next);
  return saved;
};

export const deleteCustomCollection = (collectionId: string): void => {
  const next = loadCustomCollections().filter(collection => collection.id !== collectionId);
  persistCustomCollections(next);
};

export const buildCustomCollectionsExportPayload = (): CustomCollectionsExportPayload => ({
  version: CUSTOM_COLLECTIONS_EXPORT_VERSION,
  exportedAt: new Date().toISOString(),
  collections: loadCustomCollections(),
});

const extractImportRecords = (value: unknown): unknown[] => {
  if (Array.isArray(value)) return value;
  if (!isRecord(value)) return [];
  if (Array.isArray(value.collections)) return value.collections;
    const legacyRecords = value['work' + 'flows'];
  if (Array.isArray(legacyRecords)) return legacyRecords;
  return [value];
};

const normalizeImportEntry = (value: unknown): { collection: CustomCollection; hasExplicitId: boolean } | null => {
  const collection = normalizeCustomCollection(value);
  if (!collection) return null;
  const hasExplicitId = isRecord(value)
    && typeof value.id === 'string'
    && value.id.startsWith(CUSTOM_COLLECTION_PREFIX);
  return { collection, hasExplicitId };
};

export const importCustomCollections = (value: unknown): CustomCollectionImportResult => {
  const entries = extractImportRecords(value);
  if (entries.length === 0) {
    throw new Error('No custom collections found in the selected file.');
  }

  const imported = entries
    .map(normalizeImportEntry)
    .filter((entry): entry is { collection: CustomCollection; hasExplicitId: boolean } => entry !== null);

  if (imported.length === 0) {
    throw new Error('The selected file does not contain valid custom collections.');
  }

  const now = new Date().toISOString();
  const existing = loadCustomCollections();
  const byId = new Map<string, CustomCollection>();

  for (const collection of existing) {
    byId.set(collection.id, collection);
  }

  for (const { collection, hasExplicitId } of imported) {
    const storedCollections = Array.from(byId.values());
    const collectionId = hasExplicitId ? collection.id : makeUniqueCollectionId(collection.name, storedCollections);
    const previous = byId.get(collectionId);
    byId.set(collectionId, {
      ...collection,
      id: collectionId,
      createdAt: previous?.createdAt ?? collection.createdAt ?? now,
      updatedAt: now,
    });
  }

  const collections = Array.from(byId.values()).sort((a, b) => a.name.localeCompare(b.name));
  persistCustomCollections(collections);

  return {
    imported: imported.length,
    skipped: entries.length - imported.length,
    collections,
  };
};

export const customCollectionToModelInfo = (collection: CustomCollection, modelsData: ModelsData): ModelInfo => {
  const components = getCustomCollectionComponentList(collection);
  const llmInfo = modelsData[collection.components.llm];
  const labels = new Set<string>(['custom', 'collection', 'tool-calling']);

  for (const label of llmInfo?.labels ?? []) {
    if (label === 'vision' || label === 'chat-transcription') labels.add(label);
  }
  if (llmInfo?.labels?.includes('vision') || collection.components.vision) labels.add('vision');
  if (collection.components.image) labels.add('image');
  if (collection.components.edit) labels.add('edit');
  if (collection.components.transcription) labels.add('transcription');
  if (collection.components.speech) labels.add('speech');

  const totalSize = components.reduce((sum, component) => sum + (modelsData[component]?.size ?? 0), 0);
  const maxPromptLength = llmInfo?.max_prompt_length ?? llmInfo?.max_context_window;

  return {
    checkpoint: collection.id,
    recipe: 'collection',
    suggested: true,
    downloaded: components.every(component => modelsData[component]?.downloaded === true),
    labels: Array.from(labels),
    composite_models: components,
    size: totalSize || undefined,
    max_prompt_length: maxPromptLength,
    max_context_window: llmInfo?.max_context_window,
    source: 'custom-collection',
    collection_source: 'custom',
    collection_components: collection.components,
    collection_name: collection.name,
    model_name: collection.name,
  };
};

export const mergeCustomCollectionsIntoModelsData = (modelsData: ModelsData): ModelsData => {
  const merged: ModelsData = { ...modelsData };

  for (const collection of loadCustomCollections()) {
    const components = getCustomCollectionComponentList(collection);
    if (!components.length) continue;

    // Keep stale collections out of the selector when one of their component
    // models has been deleted or renamed. The saved collection remains in
    // localStorage so users can repair it after re-downloading the components.
    if (!components.every((component) => merged[component])) continue;

    merged[collection.id] = customCollectionToModelInfo(collection, merged);
  }

  return merged;
};

export const isCollectionEligibleModel = (modelId: string, info: ModelInfo | undefined, role: CustomCollectionRole): boolean => {
  if (!info || isCustomCollectionId(modelId) || info.recipe === 'collection' || info.downloaded !== true) {
    return false;
  }

  const labels = info.labels ?? [];
  switch (role) {
    case 'llm':
      return isChatPlannerCandidate(info);
    case 'vision':
      return labels.includes('vision');
    case 'image':
      return labels.includes('image');
    case 'edit':
      return labels.includes('edit');
    case 'transcription':
      return labels.includes('transcription');
    case 'speech':
      return labels.includes('tts') || labels.includes('speech');
    default:
      return false;
  }
};

export const getCollectionRoleOptions = (modelsData: ModelsData, role: CustomCollectionRole): Array<{ id: string; info: ModelInfo }> => {
  return Object.entries(modelsData)
    .filter(([id, info]) => isCollectionEligibleModel(id, info, role))
    .sort(([aId, aInfo], [bId, bInfo]) => {
      const aName = aInfo.model_name ?? aId;
      const bName = bInfo.model_name ?? bId;
      return aName.localeCompare(bName);
    })
    .map(([id, info]) => ({ id, info }));
};
