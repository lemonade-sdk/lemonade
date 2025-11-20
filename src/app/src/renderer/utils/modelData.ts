import serverModelsData from '../../../../lemonade_server/server_models.json';

export const USER_MODEL_PREFIX = 'user.';

export interface ModelInfo {
  checkpoint: string;
  recipe: string;
  suggested: boolean;
  size?: number;
  labels?: string[];
  max_prompt_length?: number;
  mmproj?: string;
  source?: string;
  model_name?: string;
  reasoning?: boolean;
  vision?: boolean;
  [key: string]: unknown;
}

export interface ModelsData {
  [key: string]: ModelInfo;
}

export const builtInModelsData: ModelsData = serverModelsData as ModelsData;

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const normalizeLabels = (info: Record<string, unknown>): string[] => {
  const rawLabels = info['labels'];
  const labels = Array.isArray(rawLabels)
    ? rawLabels.filter((label): label is string => typeof label === 'string')
    : [];

  if (info['reasoning'] === true && !labels.includes('reasoning')) {
    labels.push('reasoning');
  }

  if (info['vision'] === true && !labels.includes('vision')) {
    labels.push('vision');
  }

  if (!labels.includes('custom')) {
    labels.push('custom');
  }

  return labels;
};

const normalizeModelInfo = (info: unknown): ModelInfo | null => {
  if (!isRecord(info)) {
    return null;
  }

  const checkpoint = typeof info['checkpoint'] === 'string' ? info['checkpoint'] : '';
  const recipe = typeof info['recipe'] === 'string' ? info['recipe'] : '';

  if (!checkpoint || !recipe) {
    return null;
  }

  const normalized: ModelInfo = {
    checkpoint,
    recipe,
    suggested: info['suggested'] === false ? false : true,
    labels: normalizeLabels(info),
  };

  const size = info['size'];
  if (typeof size === 'number' && Number.isFinite(size)) {
    normalized.size = size;
  }

  const maxPromptLength = info['max_prompt_length'];
  if (typeof maxPromptLength === 'number' && Number.isFinite(maxPromptLength)) {
    normalized.max_prompt_length = maxPromptLength;
  }

  const mmproj = info['mmproj'];
  if (typeof mmproj === 'string' && mmproj) {
    normalized.mmproj = mmproj;
  }

  const source = info['source'];
  if (typeof source === 'string' && source) {
    normalized.source = source;
  }

  const modelName = info['model_name'];
  if (typeof modelName === 'string' && modelName) {
    normalized.model_name = modelName;
  }

  const reasoning = info['reasoning'];
  if (typeof reasoning === 'boolean') {
    normalized.reasoning = reasoning;
  }

  const vision = info['vision'];
  if (typeof vision === 'boolean') {
    normalized.vision = vision;
  }

  return normalized;
};

const loadUserModelsFromCache = async (): Promise<ModelsData> => {
  if (typeof window === 'undefined' || !window.api?.readUserModels) {
    return {};
  }

  try {
    const raw = await window.api.readUserModels();
    if (!isRecord(raw)) {
      return {};
    }

    return Object.entries(raw).reduce((acc, [modelName, modelInfo]) => {
      const normalized = normalizeModelInfo(modelInfo);
      if (normalized) {
        acc[`${USER_MODEL_PREFIX}${modelName}`] = normalized;
      }
      return acc;
    }, {} as ModelsData);
  } catch (error) {
    console.error('Failed to load user models from cache:', error);
    return {};
  }
};

export const fetchSupportedModelsData = async (): Promise<ModelsData> => {
  const userModels = await loadUserModelsFromCache();
  return {
    ...builtInModelsData,
    ...userModels,
  };
};


