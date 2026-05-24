import type { ImageDefaults, ModelInfo, ModelsData } from './modelData';
import { USER_MODEL_PREFIX } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { COLLECTION_OMNI_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';

export const CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX;
const CUSTOM_COLLECTIONS_EXPORT_VERSION = 3;

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

export interface CustomCollectionPullRequest {
  model_name: string;
  recipe: typeof COLLECTION_OMNI_MODEL_RECIPE;
  components: string[];
}

export interface CustomModelExportEntry {
  model_name: string;
  recipe: string;
  checkpoint?: string;
  checkpoints?: Record<string, string>;
  labels?: string[];
  size?: number;
  mmproj?: string;
  image_defaults?: ImageDefaults;
  reasoning?: boolean;
  vision?: boolean;
}

export interface CustomCollectionsExportPayload {
  version: number;
  exportedAt: string;
  collections: CustomCollectionPullRequest[];
  models: CustomModelExportEntry[];
}

export interface CustomCollectionImportResult {
  imported: number;
  skipped: number;
  collections: CustomCollectionDraft[];
  models: CustomModelExportEntry[];
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
  if (!isCollectionRecipe(info?.recipe)) return false;
  if (modelId.startsWith(CUSTOM_COLLECTION_PREFIX)) return true;
  if ((info?.labels ?? []).includes('custom')) return true;
  if (info?.source === 'user' || info?.source === 'user_models' || info?.source === 'custom') return true;
  return info?.suggested !== true;
};

export const isCollectionEditableAsCustom = (info?: ModelInfo): boolean => isCollectionRecipe(info?.recipe);

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
  const ordered = [
    components.llm,
    components.vision,
    components.image,
    components.edit,
    components.transcription,
    components.speech,
  ].filter((value): value is string => typeof value === 'string' && value.length > 0);
  return Array.from(new Set(ordered));
};

export const modelEntryToCustomCollection = (
  modelId: string,
  info: ModelInfo | undefined,
  modelsData: ModelsData,
): CustomCollection | null => {
  if (!isCollectionEditableAsCustom(info)) return null;

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

const extractModelRecords = (value: unknown): unknown[] => {
  if (!isRecord(value)) return [];
  return Array.isArray(value.models) ? value.models : [];
};

const normalizeStringArray = (value: unknown): string[] | undefined => {
  if (!Array.isArray(value)) return undefined;
  const items = value.filter((item): item is string => typeof item === 'string' && item.length > 0);
  return items.length > 0 ? Array.from(new Set(items)) : undefined;
};

const normalizeCheckpoints = (value: unknown): Record<string, string> | undefined => {
  if (!isRecord(value)) return undefined;
  const checkpoints = Object.fromEntries(
    Object.entries(value).filter((entry): entry is [string, string] => typeof entry[1] === 'string' && entry[1].length > 0)
  );
  return Object.keys(checkpoints).length > 0 ? checkpoints : undefined;
};

const normalizeCustomModelExportEntry = (value: unknown): CustomModelExportEntry | null => {
  if (!isRecord(value)) return null;
  const modelName = normalizeComponentValue(value.model_name) ?? normalizeComponentValue(value.id);
  const recipe = normalizeComponentValue(value.recipe);
  if (!modelName || !recipe || isCollectionRecipe(recipe)) return null;

  const checkpoint = normalizeComponentValue(value.checkpoint);
  const checkpoints = normalizeCheckpoints(value.checkpoints);
  if (!checkpoint && !checkpoints) return null;

  const entry: CustomModelExportEntry = {
    model_name: modelName,
    recipe,
  };
  if (checkpoint) entry.checkpoint = checkpoint;
  if (checkpoints) entry.checkpoints = checkpoints;

  const labels = normalizeStringArray(value.labels);
  if (labels) entry.labels = labels;

  const size = value.size;
  if (typeof size === 'number' && Number.isFinite(size)) entry.size = size;

  const mmproj = normalizeComponentValue(value.mmproj);
  if (mmproj) entry.mmproj = mmproj;

  if (isRecord(value.image_defaults)) entry.image_defaults = value.image_defaults as ImageDefaults;
  if (typeof value.reasoning === 'boolean') entry.reasoning = value.reasoning;
  if (typeof value.vision === 'boolean') entry.vision = value.vision;

  return entry;
};

const modelExportEntryToModelInfo = (entry: CustomModelExportEntry): ModelInfo => ({
  checkpoint: entry.checkpoint ?? entry.checkpoints?.main ?? '',
  checkpoints: entry.checkpoints,
  recipe: entry.recipe,
  suggested: false,
  labels: entry.labels ?? [],
  downloaded: true,
  size: entry.size,
  mmproj: entry.mmproj,
  image_defaults: entry.image_defaults,
  model_name: entry.model_name,
  reasoning: entry.reasoning,
  vision: entry.vision,
});

export const importCustomCollections = (value: unknown, modelsData: ModelsData = {}): CustomCollectionImportResult => {
  const entries = extractImportRecords(value);
  const models = extractModelRecords(value)
    .map(normalizeCustomModelExportEntry)
    .filter((model): model is CustomModelExportEntry => model !== null);

  if (entries.length === 0) {
    throw new Error('No Omni Models found in the selected file.');
  }

  const importModelsData: ModelsData = { ...modelsData };
  for (const model of models) {
    importModelsData[model.model_name] = importModelsData[model.model_name] ?? modelExportEntryToModelInfo(model);
  }

  const collections = entries
    .map((entry) => normalizeCustomCollection(entry, importModelsData))
    .filter((collection): collection is CustomCollectionDraft => collection !== null);

  if (collections.length === 0) {
    throw new Error('The selected file does not contain valid Omni Models.');
  }

  return {
    imported: collections.length,
    skipped: entries.length - collections.length,
    collections,
    models,
  };
};

const modelInfoToExportEntry = (modelName: string, info?: ModelInfo): CustomModelExportEntry | null => {
  if (!info || isCollectionRecipe(info.recipe)) return null;

  const checkpoint = typeof info.checkpoint === 'string' && info.checkpoint.length > 0 ? info.checkpoint : undefined;
  const checkpoints = normalizeCheckpoints(info.checkpoints);
  if (!checkpoint && !checkpoints) return null;

  const entry: CustomModelExportEntry = {
    model_name: modelName,
    recipe: info.recipe,
  };
  if (checkpoint) entry.checkpoint = checkpoint;
  if (checkpoints) entry.checkpoints = checkpoints;

  const labels = normalizeStringArray(info.labels);
  if (labels) entry.labels = labels;
  if (typeof info.size === 'number' && Number.isFinite(info.size)) entry.size = info.size;
  if (typeof info.mmproj === 'string' && info.mmproj.length > 0) entry.mmproj = info.mmproj;
  if (info.image_defaults) entry.image_defaults = info.image_defaults;
  if (typeof info.reasoning === 'boolean') entry.reasoning = info.reasoning;
  if (typeof info.vision === 'boolean') entry.vision = info.vision;

  return entry;
};

export const buildCustomCollectionsExportPayload = (
  collections: Array<CustomCollection | CustomCollectionDraft>,
  modelsData: ModelsData = {},
): CustomCollectionsExportPayload => {
  const modelEntries = new Map<string, CustomModelExportEntry>();

  for (const collection of collections) {
    for (const component of getCustomCollectionComponentList(collection)) {
      const entry = modelInfoToExportEntry(component, modelsData[component]);
      if (entry) modelEntries.set(component, entry);
    }
  }

  return {
    version: CUSTOM_COLLECTIONS_EXPORT_VERSION,
    exportedAt: new Date().toISOString(),
    collections: collections.map(buildCustomCollectionPullRequest),
    models: Array.from(modelEntries.values()),
  };
};

export const buildCustomCollectionPullRequest = (draft: CustomCollectionDraft): CustomCollectionPullRequest => {
  const modelName = makeCollectionId(draft.id ?? draft.name);
  const components = getCustomCollectionComponentList(draft);

  if (components.length === 0 || !draft.components.llm) {
    throw new Error('Omni Model requires a name and a planner LLM.');
  }

  const request: CustomCollectionPullRequest = {
    model_name: modelName,
    recipe: COLLECTION_OMNI_MODEL_RECIPE,
    components,
  };
  return request;
};

export const buildCustomModelPullRequest = (model: CustomModelExportEntry): CustomModelExportEntry => ({ ...model });

const isCollectionEligibleModel = (info?: ModelInfo): boolean => {
  if (!info || isCollectionRecipe(info.recipe)) {
    return false;
  }
  return true;
};

export const getCollectionRoleOptions = (modelsData: ModelsData, role: CustomCollectionRole) => {
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
    .sort((a, b) => {
      const downloadedDiff = Number(b.info.downloaded === true) - Number(a.info.downloaded === true);
      if (downloadedDiff !== 0) return downloadedDiff;
      return (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id);
    });
};
