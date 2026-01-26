export const USER_MODEL_PREFIX = 'user.';

export interface ImageDefaults {
  steps?: number;
  cfg_scale?: number;
  width?: number;
  height?: number;
}

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
  downloaded?: boolean;
  image_defaults?: ImageDefaults;
  [key: string]: unknown;
}

export interface ModelsData {
  [key: string]: ModelInfo;
}

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

const fetchBuiltInModelsFromAPI = async (): Promise<ModelsData> => {
  const { serverFetch } = await import('./serverConfig');
  
  try {
    const response = await serverFetch('/models?show_all=true');
    if (!response.ok) {
      throw new Error(`Failed to fetch models: ${response.status} ${response.statusText}`);
    }
    
    const data = await response.json();
    const modelList = Array.isArray(data) ? data : data.data || [];
    
    return modelList.reduce((acc: ModelsData, model: any) => {
      if (!model.id || !model.checkpoint || !model.recipe) {
        return acc;
      }
      
      const modelInfo: ModelInfo = {
        checkpoint: model.checkpoint,
        recipe: model.recipe,
        // Use the suggested field from the API response
        suggested: model.suggested === true,
        downloaded: model.downloaded || false,
      };
      
      if (Array.isArray(model.labels)) {
        modelInfo.labels = model.labels;
      }
      
      if (typeof model.size === 'number' && Number.isFinite(model.size)) {
        modelInfo.size = model.size;
      }
      
      if (typeof model.max_prompt_length === 'number' && Number.isFinite(model.max_prompt_length)) {
        modelInfo.max_prompt_length = model.max_prompt_length;
      }
      
      if (typeof model.mmproj === 'string' && model.mmproj) {
        modelInfo.mmproj = model.mmproj;
      }
      
      if (typeof model.source === 'string' && model.source) {
        modelInfo.source = model.source;
      }
      
      if (typeof model.model_name === 'string' && model.model_name) {
        modelInfo.model_name = model.model_name;
      }
      
      if (typeof model.reasoning === 'boolean') {
        modelInfo.reasoning = model.reasoning;
      }
      
      if (typeof model.vision === 'boolean') {
        modelInfo.vision = model.vision;
      }

      // Parse image_defaults if present (for sd-cpp models)
      if (model.image_defaults && typeof model.image_defaults === 'object') {
        modelInfo.image_defaults = {
          steps: model.image_defaults.steps,
          cfg_scale: model.image_defaults.cfg_scale,
          width: model.image_defaults.width,
          height: model.image_defaults.height,
        };
      }

      acc[model.id] = modelInfo;
      return acc;
    }, {} as ModelsData);
  } catch (error) {
    console.error('Failed to fetch built-in models from API:', error);
    return {};
  }
};

export const fetchSupportedModelsData = async (): Promise<ModelsData> => {
  // Server is the source of truth for all models (including user models)
  // The /models?show_all=true endpoint returns both built-in and user models
  return fetchBuiltInModelsFromAPI();
};


