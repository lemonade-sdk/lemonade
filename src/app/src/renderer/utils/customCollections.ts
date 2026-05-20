import type { ModelInfo, ModelsData } from './modelData';
import { USER_MODEL_PREFIX } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { COLLECTION_OMNI_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';

export const CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX;
const CUSTOM_COLLECTIONS_EXPORT_VERSION = 2;

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
  createdAt?: string;
  updatedAt?: string;
  components: CustomCollectionComponents;
}

export interface CustomCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  components: CustomCollectionComponents;
}

export interface CustomCollectionsExportPayload {
  version: number;
  exportedAt: string;
  collections: CustomCollection[];
}

export interface CustomCollectionImportResult {
  imported: number;
  skipped: number;
  collections: CustomCollectionDraft[];
}

export interface CustomCollectionPullRequest {
  model_name: string;
  recipe: typeof COLLECTION_OMNI_MODEL_RECIPE;
  components: string[];
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

export const isCustomCollectionModel = (modelId: string, info?: ModelInfo): boolean => {
  return isCustomCollectionId(modelId) && isCollectionRecipe(info?.recipe);
};

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const cleanName = (value: string): string => {
  return value.trim().replace(/^user\./, '');
};

const slugify = (value: string): string => {
  const slug = cleanName(value)
    .replace(/[^a-zA-Z0-9._-]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 72);
  return slug || 'CustomCollection';
};

export const makeCollectionId = (name: string): string => {
  const trimmed = name.trim();
  return trimmed.startsWith(CUSTOM_COLLECTION_PREFIX)
    ? trimmed
    : `${CUSTOM_COLLECTION_PREFIX}${slugify(trimmed)}`;
};

export const getCollectionDisplayName = (modelId: string): string => {
  return modelId.startsWith(CUSTOM_COLLECTION_PREFIX)
    ? modelId.slice(CUSTOM_COLLECTION_PREFIX.length)
    : modelId;
};

const normalizeComponentValue = (value: unknown): string | undefined => {
  return typeof value === 'string' && value.trim() ? value.trim() : undefined;
};

const firstComponentWithLabel = (
  components: string[],
  modelsData: ModelsData,
  labelsToMatch: string[],
): string | undefined => {
  const labels = new Set(labelsToMatch);
  return components.find((component) => (modelsData[component]?.labels ?? []).some((label) => labels.has(label)));
};

const inferComponentsFromList = (components: string[], modelsData: ModelsData): CustomCollectionComponents | null => {
  const ordered = components.filter((component): component is string => typeof component === 'string' && component.length > 0);
  if (ordered.length === 0) return null;

  const llm = ordered.find((component) => isChatPlannerCandidate(modelsData[component])) ?? ordered[0];
  const result: CustomCollectionComponents = { llm };

  const vision = firstComponentWithLabel(ordered, modelsData, ['vision']);
  if (vision) result.vision = vision;

  const image = firstComponentWithLabel(ordered, modelsData, ['image']);
  if (image) result.image = image;

  const edit = firstComponentWithLabel(ordered, modelsData, ['edit']);
  if (edit) result.edit = edit;

  const transcription = firstComponentWithLabel(ordered, modelsData, ['transcription', 'audio']);
  if (transcription) result.transcription = transcription;

  const speech = firstComponentWithLabel(ordered, modelsData, ['tts', 'speech']);
  if (speech) result.speech = speech;

  return result;
};

const normalizeComponents = (value: unknown, modelsData: ModelsData = {}): CustomCollectionComponents | null => {
  if (Array.isArray(value)) {
    return inferComponentsFromList(value.filter((item): item is string => typeof item === 'string'), modelsData);
  }
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

export const getCustomCollectionComponentList = (collection: { components: CustomCollectionComponents }): string[] => {
  const components = collection.components;
  return Array.from(new Set([
    components.llm,
    components.vision,
    components.image,
    components.edit,
    components.transcription,
    components.speech,
  ].filter((value): value is string => typeof value === 'string' && value.length > 0)));
};

export const modelEntryToCustomCollection = (
  modelId: string,
  info: ModelInfo | undefined,
  modelsData: ModelsData,
): CustomCollection | null => {
  if (!isCustomCollectionModel(modelId, info)) return null;

  const components = normalizeComponents(info?.components, modelsData);
  if (!components) return null;

  return {
    id: modelId,
    name: getCollectionDisplayName(modelId),
    components,
  };
};

export const normalizeCustomCollection = (value: unknown, modelsData: ModelsData = {}): CustomCollectionDraft | null => {
  if (!isRecord(value)) return null;

  const rawName = normalizeComponentValue(value.name)
    ?? normalizeComponentValue(value.model_name)
    ?? normalizeComponentValue(value.id);
  if (!rawName) return null;

  const components = normalizeComponents(value.components, modelsData);
  if (!components) return null;

  const componentList = getCustomCollectionComponentList({ components });
  if (Object.keys(modelsData).length > 0 && !componentList.every((component) => !!modelsData[component])) {
    return null;
  }

  const rawId = normalizeComponentValue(value.id) ?? normalizeComponentValue(value.model_name);

  return {
    id: rawId ? makeCollectionId(rawId) : undefined,
    name: cleanName(rawName),
    createdAt: normalizeComponentValue(value.createdAt),
    components,
  };
};

const extractImportRecords = (value: unknown): unknown[] => {
  if (Array.isArray(value)) return value;
  if (!isRecord(value)) return [];
  if (Array.isArray(value.collections)) return value.collections;
  return [value];
};

export const importCustomCollections = (value: unknown, modelsData: ModelsData = {}): CustomCollectionImportResult => {
  const entries = extractImportRecords(value);
  if (entries.length === 0) {
    throw new Error('No custom collections found in the selected file.');
  }

  const collections = entries
    .map((entry) => normalizeCustomCollection(entry, modelsData))
    .filter((collection): collection is CustomCollectionDraft => collection !== null);

  if (collections.length === 0) {
    throw new Error('The selected file does not contain valid custom collections.');
  }

  return {
    imported: collections.length,
    skipped: entries.length - collections.length,
    collections,
  };
};

export const buildCustomCollectionsExportPayload = (collections: CustomCollection[] = []): CustomCollectionsExportPayload => ({
  version: CUSTOM_COLLECTIONS_EXPORT_VERSION,
  exportedAt: new Date().toISOString(),
  collections,
});

export const buildCustomCollectionPullRequest = (collection: CustomCollectionDraft): CustomCollectionPullRequest => {
  const modelName = collection.id ? makeCollectionId(collection.id) : makeCollectionId(collection.name);
  const components = getCustomCollectionComponentList(collection);

  if (!collection.name.trim() || !collection.components.llm || components.length === 0) {
    throw new Error('Custom collection requires a name and a planner LLM.');
  }

  return {
    model_name: modelName,
    recipe: COLLECTION_OMNI_MODEL_RECIPE,
    components,
  };
};

const isCollectionEligibleModel = (info?: ModelInfo): boolean => {
  if (!info || isCollectionRecipe(info.recipe) || info.downloaded !== true) {
    return false;
  }
  return true;
};

export const getCollectionRoleOptions = (modelsData: ModelsData, role: CustomCollectionRole): Array<{ id: string; info: ModelInfo }> => {
  return Object.entries(modelsData)
    .filter(([, info]) => isCollectionEligibleModel(info))
    .filter(([, info]) => {
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
          return labels.includes('transcription') || labels.includes('audio');
        case 'speech':
          return labels.includes('tts') || labels.includes('speech');
        default:
          return false;
      }
    })
    .map(([id, info]) => ({ id, info }))
    .sort((a, b) => (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id));
};
