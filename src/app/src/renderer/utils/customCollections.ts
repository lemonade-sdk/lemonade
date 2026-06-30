import type { ModelInfo, ModelsData } from './modelData';
import { USER_MODEL_PREFIX } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { COLLECTION_OMNI_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';
import toolDefinitions from './toolDefinitions.json';

export const CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX;

// The shipped OmniRouter system prompt, exposed so the Omni Model editor can
// show authors the text they're (potentially) overriding. Matched verbatim on
// save: when the textarea content equals this string, the editor stores no
// override and the collection stays on whatever the global default is at
// runtime — so a future tweak to toolDefinitions.json propagates automatically.
export const DEFAULT_OMNI_SYSTEM_PROMPT: string = toolDefinitions.system_prompt;

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
  // Optional per-collection system prompt template. Overrides the global
  // default in toolDefinitions.json when set; still uses {tool_list} and
  // {tool_guidance} placeholders for runtime substitution.
  systemPrompt?: string;
}

export interface CustomCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  components: CustomCollectionComponents;
  systemPrompt?: string;
}

export interface CustomCollectionPullRequest {
  model_name: string;
  recipe: typeof COLLECTION_OMNI_MODEL_RECIPE;
  components: string[];
  // Optional template (matches the registry's system_prompt field).
  system_prompt?: string;
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

  const systemPrompt = typeof info?.system_prompt === 'string' && info.system_prompt
    ? info.system_prompt
    : undefined;

  return {
    id: modelId,
    name: getCollectionDisplayName(modelId),
    components,
    systemPrompt,
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
  const systemPrompt = typeof draft.systemPrompt === 'string' ? draft.systemPrompt.trim() : '';
  if (systemPrompt) {
    request.system_prompt = systemPrompt;
  }
  return request;
};

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
