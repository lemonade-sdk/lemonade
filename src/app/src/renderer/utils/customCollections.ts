import type { ModelInfo, ModelsData } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';

export const CUSTOM_COLLECTION_PREFIX = 'collection.';
export const CUSTOM_COLLECTIONS_STORAGE_KEY = 'lemonade.customCollections.v1';
export const CUSTOM_COLLECTIONS_EXPORT_VERSION = 1;

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
  components: CustomCollectionComponents;
  createdAt: string;
  updatedAt: string;
}

export interface CustomCollectionExportPayload {
  version: number;
  exportedAt: string;
  collections: CustomCollection[];
}

export interface CustomCollectionImportResult {
  imported: number;
  skipped: number;
  collections: CustomCollection[];
}

export type CustomCollectionDraft = Pick<CustomCollection, 'name' | 'components'> & Partial<Pick<CustomCollection, 'id' | 'createdAt' | 'updatedAt'>>;

const ROLE_LABELS: Record<CustomCollectionRole, string> = {
  llm: 'LLM',
  vision: 'Vision / image analysis',
  image: 'Image generation',
  edit: 'Image editing',
  transcription: 'Speech-to-text',
  speech: 'Text-to-speech',
};

const hasStorage = (): boolean => {
  return typeof window !== 'undefined' && typeof window.localStorage !== 'undefined';
};

const slugify = (value: string): string => {
  const slug = value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
  return slug || 'custom-collection';
};

const buildCollectionId = (name: string): string => `${CUSTOM_COLLECTION_PREFIX}${slugify(name)}`;

const normalizeId = (name: string, explicitId?: string): string => {
  if (explicitId && explicitId.startsWith(CUSTOM_COLLECTION_PREFIX)) {
    return explicitId;
  }
  return buildCollectionId(name);
};

const makeUniqueCollectionId = (name: string, collections: CustomCollection[]): string => {
  const baseId = buildCollectionId(name);
  const existingIds = new Set(collections.map((collection) => collection.id));
  if (!existingIds.has(baseId)) return baseId;

  let suffix = 2;
  let candidate = `${baseId}-${suffix}`;
  while (existingIds.has(candidate)) {
    suffix += 1;
    candidate = `${baseId}-${suffix}`;
  }
  return candidate;
};

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const normalizeOptionalModel = (value: unknown): string | undefined => {
  return typeof value === 'string' && value.trim().length > 0 ? value.trim() : undefined;
};

const normalizeComponents = (value: unknown): CustomCollectionComponents | null => {
  if (!isRecord(value)) return null;
  const llm = normalizeOptionalModel(value.llm);
  if (!llm) return null;
  return {
    llm,
    vision: normalizeOptionalModel(value.vision),
    image: normalizeOptionalModel(value.image),
    edit: normalizeOptionalModel(value.edit),
    transcription: normalizeOptionalModel(value.transcription),
    speech: normalizeOptionalModel(value.speech),
  };
};

export const normalizeCustomCollection = (value: unknown): CustomCollection | null => {
  if (!isRecord(value)) return null;
  const name = typeof value.name === 'string' && value.name.trim().length > 0
    ? value.name.trim()
    : 'Custom Collection';
  const components = normalizeComponents(value.components);
  if (!components) return null;
  const now = new Date().toISOString();
  return {
    id: normalizeId(name, typeof value.id === 'string' ? value.id : undefined),
    name,
    components,
    createdAt: typeof value.createdAt === 'string' ? value.createdAt : now,
    updatedAt: typeof value.updatedAt === 'string' ? value.updatedAt : now,
  };
};

export const isCustomCollectionId = (modelId: string): boolean => {
  return modelId.startsWith(CUSTOM_COLLECTION_PREFIX);
};

export const getCustomCollectionRoleLabel = (role: CustomCollectionRole): string => ROLE_LABELS[role];

export const getCustomCollectionComponentList = (collection: Pick<CustomCollection, 'components'>): string[] => {
  const ordered = [
    collection.components.llm,
    collection.components.vision,
    collection.components.image,
    collection.components.edit,
    collection.components.transcription,
    collection.components.speech,
  ].filter((model): model is string => typeof model === 'string' && model.length > 0);

  return Array.from(new Set(ordered));
};

export const loadCustomCollections = (): CustomCollection[] => {
  if (!hasStorage()) return [];
  try {
    const raw = window.localStorage.getItem(CUSTOM_COLLECTIONS_STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed
      .map(normalizeCustomCollection)
      .filter((collection): collection is CustomCollection => collection !== null)
      .sort((a, b) => a.name.localeCompare(b.name));
  } catch (error) {
    console.error('Failed to load custom collections:', error);
    return [];
  }
};

const persistCustomCollections = (collections: CustomCollection[]): void => {
  if (!hasStorage()) return;
  try {
    window.localStorage.setItem(CUSTOM_COLLECTIONS_STORAGE_KEY, JSON.stringify(collections));
    window.dispatchEvent(new CustomEvent('customCollectionsUpdated'));
  } catch (error) {
    console.error('Failed to save custom collections:', error);
  }
};

export const saveCustomCollection = (draft: CustomCollectionDraft): CustomCollection => {
  const collections = loadCustomCollections();
  const explicitId = typeof draft.id === 'string' && draft.id.startsWith(CUSTOM_COLLECTION_PREFIX)
    ? draft.id
    : undefined;
  const collectionId = explicitId ?? makeUniqueCollectionId(draft.name, collections);

  const normalized = normalizeCustomCollection({
    ...draft,
    id: collectionId,
    createdAt: draft.createdAt,
    updatedAt: new Date().toISOString(),
  });

  if (!normalized) {
    throw new Error('Custom collection requires a name and an LLM model.');
  }

  const existing = collections.find((collection) => collection.id === normalized.id);
  const saved: CustomCollection = {
    ...normalized,
    createdAt: existing?.createdAt ?? normalized.createdAt,
  };
  const next = collections.filter((collection) => collection.id !== saved.id).concat(saved)
    .sort((a, b) => a.name.localeCompare(b.name));

  persistCustomCollections(next);
  return saved;
};

export const deleteCustomCollection = (collectionId: string): void => {
  const next = loadCustomCollections().filter((collection) => collection.id !== collectionId);
  persistCustomCollections(next);
};


export const buildCustomCollectionsExportPayload = (): CustomCollectionExportPayload => ({
  version: CUSTOM_COLLECTIONS_EXPORT_VERSION,
  exportedAt: new Date().toISOString(),
  collections: loadCustomCollections(),
});

const extractImportCandidates = (value: unknown): unknown[] => {
  if (Array.isArray(value)) return value;
  if (!isRecord(value)) return [];

  if (Array.isArray(value.collections)) return value.collections;

  // Be permissive for hand-authored files and single-collection exports.
  if (isRecord(value.components)) return [value];

  return [];
};

interface NormalizedImportCandidate {
  collection: CustomCollection;
  hasExplicitId: boolean;
}

const normalizeImportCandidate = (value: unknown): NormalizedImportCandidate | null => {
  const collection = normalizeCustomCollection(value);
  if (!collection) return null;

  const hasExplicitId = isRecord(value)
    && typeof value.id === 'string'
    && value.id.startsWith(CUSTOM_COLLECTION_PREFIX);

  return { collection, hasExplicitId };
};

export const importCustomCollections = (value: unknown): CustomCollectionImportResult => {
  const candidates = extractImportCandidates(value);
  if (candidates.length === 0) {
    throw new Error('No custom collections found in the selected file.');
  }

  const imported = candidates
    .map(normalizeImportCandidate)
    .filter((candidate): candidate is NormalizedImportCandidate => candidate !== null);

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
    skipped: candidates.length - imported.length,
    collections,
  };
};

export const customCollectionToModelInfo = (collection: CustomCollection, modelsData: ModelsData): ModelInfo => {
  const components = getCustomCollectionComponentList(collection);
  const llmInfo = modelsData[collection.components.llm];
  const downloaded = components.length > 0 && components.every((component) => modelsData[component]?.downloaded === true);
  const labels = new Set<string>(['custom', 'collection', 'tool-calling']);

  if (llmInfo?.labels?.includes('vision') || collection.components.vision) labels.add('vision');
  if (collection.components.image) labels.add('image');
  if (collection.components.edit) labels.add('edit');
  if (collection.components.transcription) labels.add('transcription');
  if (collection.components.speech) labels.add('speech');

  return {
    checkpoint: '',
    recipe: 'collection',
    suggested: true,
    downloaded,
    labels: Array.from(labels),
    composite_models: components,
    max_prompt_length: llmInfo?.max_prompt_length,
    source: 'custom-collection',
    collection_source: 'custom',
    collection_components: collection.components,
    collection_name: collection.name,
  };
};

export const mergeCustomCollectionsIntoModelsData = (modelsData: ModelsData): ModelsData => {
  const merged: ModelsData = { ...modelsData };

  for (const collection of loadCustomCollections()) {
    const components = getCustomCollectionComponentList(collection);
    if (components.length === 0) continue;

    // Keep stale collections out of the selector when one of their component
    // models has been deleted or renamed. The collection remains in localStorage
    // and comes back automatically if the component models reappear.
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
  const hasLabel = (...needles: string[]) => labels.some((label) => needles.includes(label));

  switch (role) {
    case 'llm':
      return isChatPlannerCandidate(info);
    case 'vision':
      return hasLabel('vision');
    case 'image':
      return hasLabel('image');
    case 'edit':
      return hasLabel('edit');
    case 'transcription':
      return hasLabel('transcription') || (hasLabel('audio') && !hasLabel('vision', 'tool-calling', 'tools'));
    case 'speech':
      return hasLabel('speech', 'tts');
    default:
      return false;
  }
};

export const getCollectionRoleOptions = (modelsData: ModelsData, role: CustomCollectionRole): Array<{ id: string; info: ModelInfo }> => {
  return Object.entries(modelsData)
    .filter(([id, info]) => isCollectionEligibleModel(id, info, role))
    .map(([id, info]) => ({ id, info }))
    .sort((a, b) => a.id.localeCompare(b.id));
};
