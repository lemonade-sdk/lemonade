import { isModelCollectionRecipe } from './recipeNames';

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
  // Cloud models only: USD per 1M tokens, when the provider reports it.
  cost_input_per_million?: number;
  cost_output_per_million?: number;
  mmproj?: string;
  source?: string;
  registry_source?: 'huggingface' | 'modelscope';
  model_name?: string;
  reasoning?: boolean;
  vision?: boolean;
  downloaded?: boolean;
  update_available?: boolean;
  image_defaults?: ImageDefaults;
  // Per-collection system prompt template (collection.omni only). Overrides the
  // global default in toolDefinitions.json when set. Keeps {tool_list} and
  // {tool_guidance} placeholders so runtime substitution still works.
  system_prompt?: string;
  // collection.router policies. Kept opaque in the general model catalog; router
  // authoring tools validate against the routing schema.
  routing?: unknown;
  [key: string]: unknown;
}

export interface ModelsData {
  [key: string]: ModelInfo;
}

export type TtsVoiceMode = 'fixed' | 'clone' | 'design';

export const getTtsVoiceMode = (info?: ModelInfo | null): TtsVoiceMode => {
  if (!info) return 'fixed';
  if ((info.labels || []).includes('voice-design')) return 'design';
  if (info.recipe === 'openmoss') return 'clone';
  return 'fixed';
};

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

  if (!recipe || (!checkpoint && !isModelCollectionRecipe(recipe))) {
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

  const registrySource = info['registry_source'];
  if (registrySource === 'huggingface' || registrySource === 'modelscope') {
    normalized.registry_source = registrySource;
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

  const systemPrompt = info['system_prompt'];
  if (typeof systemPrompt === 'string' && systemPrompt) {
    normalized.system_prompt = systemPrompt;
  }

  if (isRecord(info['routing'])) {
    normalized.routing = info['routing'];
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
        update_available: model.update_available === true,
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

      if (model.registry_source === 'huggingface' || model.registry_source === 'modelscope') {
        modelInfo.registry_source = model.registry_source;
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

      if (typeof model.system_prompt === 'string' && model.system_prompt) {
        modelInfo.system_prompt = model.system_prompt;
      }

      if (model.routing && typeof model.routing === 'object' && !Array.isArray(model.routing)) {
        modelInfo.routing = model.routing;
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

export const fetchSupportedModelsData = async (): Promise<ModelsData> => {
  // Cloud models are now served by lemond through /v1/models like every other
  // recipe — the server discovers them from each installed provider as soon
  // as an API key is resolvable (env var or POST /v1/cloud/auth). No
  // client-side discovery, no per-client mirroring.
  return fetchBuiltInModelsFromAPI();
};

// ---------------------------------------------------------------------------
// Model export — mirrors the CLI's validate_and_transform_model_json so GUI
// and CLI produce the same import-ready file from the live /models/{id} object.
// ---------------------------------------------------------------------------

// Keys allowed in exported model files. Keep in sync with kKnownKeys in
// src/cpp/cli/recipe_import.cpp. Notably excludes the user-specific runtime
// fields (suggested, created, downloaded) and wire decorations (id, object,
// owned_by) — the server regenerates those on import.
const EXPORT_KNOWN_KEYS = new Set([
  'checkpoint',
  'checkpoints',
  'components',
  'model_name',
  'models',
  'image_defaults',
  'labels',
  'recipe',
  'recipe_options',
  'routing',
  'source',
  'registry_source',
  'size',
  'system_prompt',
  // Router collections carry a root schema version the /pull parser requires;
  // dropping it would make the exported file un-importable.
  'version',
]);

const toExportEntry = (raw: Record<string, unknown>): Record<string, unknown> => {
  const entry = Object.fromEntries(
    Object.entries(raw).filter(([key]) => EXPORT_KNOWN_KEYS.has(key))
  );
  if (typeof entry.model_name !== 'string' && typeof raw.id === 'string') {
    entry.model_name = raw.id;
  }
  if (isRecord(entry.checkpoints) && 'checkpoint' in entry) {
    delete entry.checkpoint;
  }

  // Preserve only portable remote provenance. Local origins refer to paths on
  // the exporting machine and must not be replayed on import.
  const publicSource = typeof raw.source === 'string' ? raw.source.toLowerCase() : '';
  const explicitRegistry = typeof raw.registry_source === 'string'
    ? raw.registry_source.toLowerCase()
    : '';
  const isRemoteSource = (value: string): value is 'huggingface' | 'modelscope' =>
    value === 'huggingface' || value === 'modelscope';

  if (publicSource && !isRemoteSource(publicSource)) {
    delete entry.source;
    delete entry.registry_source;
  } else {
    const registrySource = isRemoteSource(publicSource)
      ? publicSource
      : isRemoteSource(explicitRegistry)
        ? explicitRegistry
        : '';
    if (registrySource) {
      entry.source = registrySource;
      entry.registry_source = registrySource;
    } else {
      delete entry.source;
      delete entry.registry_source;
    }
  }
  return entry;
};

/**
 * Normalize a live /models/{id} object into the import-ready file shape
 * (pure; mirrors the CLI transform). Collections keep `components` and a
 * `models` array with each component normalized by the same per-model
 * transform.
 */
export const normalizeModelExportPayload = (
  raw: Record<string, unknown>,
  fallbackId = '',
): { filename: string; payload: Record<string, unknown> } => {
  const payload = toExportEntry(raw);

  // The exported model itself is import-ready: registration uses the `user.`
  // namespace, exactly like the CLI export transform.
  const name = typeof payload.model_name === 'string' && payload.model_name ? payload.model_name : fallbackId;
  payload.model_name = name.startsWith(USER_MODEL_PREFIX) ? name : `${USER_MODEL_PREFIX}${name}`;

  if (isModelCollectionRecipe(typeof payload.recipe === 'string' ? payload.recipe : undefined)) {
    // Normalize each embedded component with the same transform. Components
    // are leaf models: drop their (empty) collection fields and keep bare
    // names — the server decides `user.` prefixing when registering them.
    const models = Array.isArray(raw.models) ? raw.models : [];
    payload.models = models.filter(isRecord).map((component) => {
      const entry = toExportEntry(component);
      if (Array.isArray(entry.components) && entry.components.length === 0) delete entry.components;
      if (Array.isArray(entry.models) && entry.models.length === 0) delete entry.models;
      return entry;
    });
  } else {
    // Regular-model files carry no collection fields (the live API emits an
    // empty `components` array on every model object).
    delete payload.components;
    delete payload.models;
  }

  const bareName = name.startsWith(USER_MODEL_PREFIX) ? name.slice(USER_MODEL_PREFIX.length) : name;
  return { filename: `${bareName}.json`, payload };
};

/**
 * Build an exportable model JSON the same way `lemonade export` does: fetch
 * the live /models/{id} object and normalize it into the import-ready file
 * shape.
 */
export const buildModelExportFile = async (
  modelId: string,
): Promise<{ filename: string; payload: Record<string, unknown> }> => {
  const { serverFetch } = await import('./serverConfig');
  const response = await serverFetch(`/models/${encodeURIComponent(modelId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch model info for '${modelId}' (HTTP ${response.status}).`);
  }
  const raw: unknown = await response.json();
  if (!isRecord(raw)) {
    throw new Error(`Unexpected /models response for '${modelId}'.`);
  }
  return normalizeModelExportPayload(raw, modelId);
};

/** Trigger a browser download of a JSON payload as a file. */
export const downloadJsonFile = (filename: string, payload: unknown): void => {
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url = window.URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  window.URL.revokeObjectURL(url);
};

/** Trigger a browser download of an exported model/collection JSON. */
export const downloadModelExportFile = async (modelId: string): Promise<void> => {
  const { filename, payload } = await buildModelExportFile(modelId);
  downloadJsonFile(filename, payload);
};
