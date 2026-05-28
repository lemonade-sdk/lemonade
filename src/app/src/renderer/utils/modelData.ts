import { isCollectionRecipe } from './recipeNames';

export const USER_MODEL_PREFIX = 'user.';

export interface ImageDefaults {
  steps?: number;
  cfg_scale?: number;
  width?: number;
  height?: number;
  sampling_method?: string;
  flow_shift?: number;
}

export interface ModelInfo {
  checkpoint: string;
  checkpoints?: Record<string, string>;
  recipe: string;
  suggested: boolean;
  size?: number;
  labels?: string[];
  components?: string[];
  max_prompt_length?: number;
  max_context_window?: number;
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

  if (!recipe || (!checkpoint && !isCollectionRecipe(recipe))) {
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

  const maxContextWindow = info['max_context_window'];
  if (typeof maxContextWindow === 'number' && Number.isFinite(maxContextWindow)) {
    normalized.max_context_window = maxContextWindow;
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

  const components = info['components'];
  if (Array.isArray(components)) {
    normalized.components = components.filter((model): model is string => typeof model === 'string');
  }

  const checkpoints = info['checkpoints'];
  if (isRecord(checkpoints)) {
    const normalizedCheckpoints = Object.fromEntries(
      Object.entries(checkpoints).filter((entry): entry is [string, string] => typeof entry[1] === 'string')
    );
    if (Object.keys(normalizedCheckpoints).length > 0) {
      normalized.checkpoints = normalizedCheckpoints;
    }
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
      if (!model.id || !model.recipe) {
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

      if (typeof model.max_context_window === 'number' && Number.isFinite(model.max_context_window)) {
        modelInfo.max_context_window = model.max_context_window;
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

      const components = model.components;
      if (Array.isArray(components)) {
        modelInfo.components = components.filter((component: unknown): component is string => typeof component === 'string');
      }

      if (model.checkpoints && typeof model.checkpoints === 'object' && !Array.isArray(model.checkpoints)) {
        const checkpoints = Object.fromEntries(
          Object.entries(model.checkpoints).filter((entry): entry is [string, string] => typeof entry[1] === 'string')
        );
        if (Object.keys(checkpoints).length > 0) {
          modelInfo.checkpoints = checkpoints;
        }
      }

      if (model.recipe_options && typeof model.recipe_options === 'object') {
        modelInfo.recipe_options = model.recipe_options;
      }

      if (typeof model.reasoning === 'boolean') {
        modelInfo.reasoning = model.reasoning;
      }

      if (typeof model.vision === 'boolean') {
        modelInfo.vision = model.vision;
      }

      // cloud_provider distinguishes per-provider buckets in the Model
      // Manager grouping (recipe="cloud" alone collapses all providers
      // into a single sub-heading).
      if (typeof model.cloud_provider === 'string' && model.cloud_provider) {
        modelInfo.cloud_provider = model.cloud_provider;
      }

      // Parse image_defaults if present (for sd-cpp models)
      if (model.image_defaults && typeof model.image_defaults === 'object') {
        modelInfo.image_defaults = {
          steps: model.image_defaults.steps,
          cfg_scale: model.image_defaults.cfg_scale,
          width: model.image_defaults.width,
          height: model.image_defaults.height,
          sampling_method: model.image_defaults.sampling_method,
          flow_shift: model.image_defaults.flow_shift,
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

// Client-driven cloud model discovery. Reads cloud providers out of the
// local app settings and asks lemond to proxy a /v1/models call for each.
// Failures are swallowed so a dead provider doesn't sink the whole model
// list — the user just sees fewer entries. This is the client-side
// equivalent of the server-side Step 1.7 that used to live in
// ModelManager::build_cache before cloud creds moved client-side.
const fetchCloudModelsFromProviders = async (): Promise<ModelsData> => {
  if (typeof window === 'undefined' || !window.api?.getSettings) return {};
  let providers: Record<string, { baseUrl: string; apiKey: string }> = {};
  try {
    const stored = await window.api.getSettings();
    const raw = (stored as any)?.cloudProviders;
    if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
      for (const [name, cfg] of Object.entries(raw)) {
        if (!cfg || typeof cfg !== 'object') continue;
        const c = cfg as any;
        if (typeof c.baseUrl === 'string' && typeof c.apiKey === 'string' && c.apiKey.length > 0) {
          providers[name] = { baseUrl: c.baseUrl, apiKey: c.apiKey };
        }
      }
    }
  } catch (err) {
    console.warn('Failed to read cloud providers from settings:', err);
    return {};
  }
  if (Object.keys(providers).length === 0) return {};

  const { serverConfig, getServerBaseUrl } = await import('./serverConfig');

  const results = await Promise.all(
    Object.entries(providers).map(async ([name, cfg]) => {
      try {
        const url = `${getServerBaseUrl()}/internal/cloud/discover`;
        const response = await serverConfig.fetch(url, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ provider: name, base_url: cfg.baseUrl, api_key: cfg.apiKey }),
        });
        if (!response.ok) return {} as ModelsData;
        const body = await response.json();
        const list = Array.isArray(body?.data) ? body.data : [];
        const out: ModelsData = {};
        // Feed the model_name->upstream_id mapping back into serverConfig so
        // future cloud chat requests can attach X-Lemonade-Cloud-Upstream-Model.
        // Providers like Fireworks expose cleaned public ids that don't
        // round-trip server-side; without this the upstream API call uses
        // the wrong model id and 404s.
        const checkpointEntries: Array<{ id: string; checkpoint: string }> = [];
        for (const entry of list) {
          if (!entry?.id || typeof entry.id !== 'string') continue;
          const info: ModelInfo = {
            checkpoint: typeof entry.checkpoint === 'string' ? entry.checkpoint : '',
            recipe: 'cloud',
            // useModels.suggestedModels (which feeds the Model Manager
            // picker) filters to `info.suggested || name.startsWith(USER_MODEL_PREFIX)`.
            // Cloud models are added through the UI like user models, so
            // mark them suggested so they surface in the picker.
            suggested: true,
            downloaded: true, // cloud models have no local artifact
          };
          if (typeof entry.cloud_provider === 'string' && entry.cloud_provider) {
            info.cloud_provider = entry.cloud_provider;
          } else {
            info.cloud_provider = name;
          }
          if (Array.isArray(entry.labels)) {
            info.labels = entry.labels.filter((l: unknown): l is string => typeof l === 'string');
          }
          out[entry.id] = info;
          if (typeof entry.checkpoint === 'string' && entry.checkpoint.length > 0) {
            checkpointEntries.push({ id: entry.id, checkpoint: entry.checkpoint });
          }
        }
        serverConfig.setCloudModelCheckpoints(name, checkpointEntries);
        return out;
      } catch (err) {
        console.warn(`Cloud discovery failed for provider '${name}':`, err);
        return {} as ModelsData;
      }
    })
  );

  return results.reduce<ModelsData>((acc, partial) => ({ ...acc, ...partial }), {});
};

export const fetchSupportedModelsData = async (): Promise<ModelsData> => {
  // Server is the source of truth for built-in + user models.
  // Cloud models live client-side now (each client manages its own
  // credentials per AGENTS.md Invariant #11), so we discover them via
  // /internal/cloud/discover and merge into the same map. Built-ins
  // win on key collision — the merge order matters.
  const [builtIns, cloud] = await Promise.all([
    fetchBuiltInModelsFromAPI(),
    fetchCloudModelsFromProviders(),
  ]);
  return { ...cloud, ...builtIns };
};
