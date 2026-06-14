/**
 * Lemonade tool definitions and executor.
 * Exposes lemonade server management as OpenAI-compatible function calling tools.
 */
import api, { searchHuggingFace, HFModelResult, PullVariantsResult } from '../api';

/* ── Tool schemas (OpenAI function calling format) ─────────────── */

export interface ToolFunction {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, unknown>;
  };
}

export const LEMONADE_TOOLS: ToolFunction[] = [
  {
    type: 'function',
    function: {
      name: 'list_models',
      description: 'List Lemonade models. Use this for inventory questions only. By default it returns LOCAL models only (currently loaded + downloaded), not the full registry. For a specific model or load decision, call get_model_info after this. For backend/recipe installation questions, call list_backends instead of this.',
      parameters: {
        type: 'object',
        properties: {
          query: { type: 'string', description: 'Optional case-insensitive model-name filter. Use when the user mentions a model family/name such as Flux, Gemma, Qwen, Llama, Whisper, or SD.' },
          status: { type: 'string', enum: ['local', 'loaded', 'downloaded', 'registry', 'all'], description: 'Which models to return. Default: local. Use registry/all only when the user explicitly asks what can be downloaded.' },
          capability: { type: 'string', description: 'Optional capability filter: chat, image, audio, tts, embedding, reranking, omni.' },
          limit: { type: 'number', description: 'Maximum returned items per section. Default 30.' },
        },
        required: [],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_model_info',
      description: 'Get detailed info about one specific model: downloaded state, labels/capability, size, context window, checkpoint, and available recipes/backends. Use this for any user question about a named model and before load_model when recipe/device is unclear. Do not answer a named-model question from list_models alone.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to look up.' },
        },
        required: ['model_name'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'load_model',
      description: 'Load a downloaded/local model into the server for inference. Use this when the user asks to load/start/use a model. A recipe defines the inference backend: llamacpp (GPU/CPU via llama.cpp — backends: vulkan, rocm, metal, cpu), flm (NPU via FastFlowLM), ryzenai-llm (hybrid NPU). Combined forms like "llamacpp-vulkan" or "llamacpp-cpu" select a backend. If the user asks for CPU, pass backend/device=cpu. If multiple recipes are available and the user did not choose, call get_model_info and ask_question first.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to load.' },
          recipe: { type: 'string', description: 'The recipe to use. Examples: "llamacpp", "flm", "ryzenai-llm". Combined inputs like "llamacpp-cpu" are accepted and are normalized to recipe=llamacpp plus backend=cpu.' },
          backend: { type: 'string', description: 'Optional backend/device target for the recipe. Examples: "cpu", "vulkan", "rocm", "metal", "npu". Use "cpu" when the user asks to load on CPU.' },
          device: { type: 'string', description: 'Alias for backend. Examples: "cpu", "gpu", "npu".' },
          n_ctx: { type: 'number', description: 'Context window size (e.g. 4096, 8192, 32768). Higher uses more memory.' },
          n_gpu_layers: { type: 'number', description: 'Number of layers to offload to GPU. Higher = faster but uses more VRAM.' },
        },
        required: ['model_name'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'unload_model',
      description: 'Unload a model from the server, freeing its resources. If no model name is given, unloads the most recently used model.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model to unload. Optional — unloads MRU if omitted.' },
        },
        required: [],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_loaded_models',
      description: 'List only the currently loaded models with their recipe, device, and resource usage. Multiple models can be loaded simultaneously within server limits.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_server_health',
      description: 'Get server health status including version, loaded models count, and per-type resource limits (max loaded models for LLM, embedding, reranking, audio, image, TTS).',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'pull_model',
      description: 'Download a model to local storage. First try Lemonade registry names; if no registry match exists, this tool can search Hugging Face GGUF checkpoints and download one when a checkpoint/variant is specified or one clear option exists. Use ask_question if several registry or Hugging Face variants are returned as choices. Models must be pulled before load_model can run.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'Registry model name/ID to download, or desired local model name for a Hugging Face checkpoint. Can be a fuzzy query such as "gemma 4b".' },
          query: { type: 'string', description: 'Optional search query when model_name is not enough. Use this for Hugging Face or fuzzy registry search.' },
          source: { type: 'string', enum: ['auto', 'registry', 'huggingface'], description: 'Where to pull from. Default auto: registry first, then Hugging Face GGUF search.' },
          checkpoint: { type: 'string', description: 'Optional Hugging Face repo id, e.g. TheBloke/Llama-2-7B-GGUF. If set, the tool downloads from Hugging Face.' },
          variant: { type: 'string', description: 'Optional Hugging Face file/variant name, e.g. model.Q4_K_M.gguf. Required when several variants exist unless one clear option can be selected.' },
          recipe: { type: 'string', description: 'Recipe for Hugging Face pulls. Default: llamacpp for GGUF checkpoints.' },
        },
        required: ['model_name'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'delete_model',
      description: 'Delete a downloaded model from local storage, freeing disk space.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to delete.' },
        },
        required: ['model_name'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_system_info',
      description: 'Get system hardware info including CPU, GPU (AMD/NVIDIA), VRAM, and NPU capabilities, plus all available recipes and their backend install status.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'list_backends',
      description: 'List all Lemonade recipes/backends and their install status. Use this, not list_models, whenever the user asks about backends, recipes, CPU/GPU/NPU support, installed engines, or backend installation/update state.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'install_backend',
      description: 'Install or update a backend for a recipe (e.g. install vulkan for llamacpp to enable GPU inference). Use list_backends first to check what is installable.',
      parameters: {
        type: 'object',
        properties: {
          recipe: { type: 'string', description: 'The recipe name (e.g. "llamacpp", "whispercpp", "moonshine", "sd-cpp", "kokoro", "flm", "vllm").' },
          backend: { type: 'string', description: 'The backend to install (e.g. "vulkan", "rocm", "cpu", "metal", "npu").' },
        },
        required: ['recipe', 'backend'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'ask_question',
      description: 'Present an interactive question with clickable choices to the user. Use this whenever you need the user to choose between options (e.g. which recipe, which model, yes/no confirmations). The UI renders choices as clickable buttons. Set allowCustom to true if the user should also be able to type a custom answer.',
      parameters: {
        type: 'object',
        properties: {
          question: { type: 'string', description: 'The question to ask the user.' },
          choices: {
            type: 'array',
            items: { type: 'string' },
            description: 'Array of choice strings to present as clickable buttons.',
          },
          allowCustom: { type: 'boolean', description: 'Whether to show a text input for custom answers. Defaults to true.' },
        },
        required: ['question', 'choices'],
      },
    },
  },
];

/* ── Tool executor ─────────────────────────────────────────────── */

export interface ToolCall {
  id: string;
  type: 'function';
  function: {
    name: string;
    arguments: string; // JSON string
  };
}

export interface ToolResult {
  tool_call_id: string;
  role: 'tool';
  content: string;
  displayResult?: string;
  artifacts?: Array<{ type: 'image' | 'audio'; url: string; name?: string; mime?: string }>;
  error?: boolean;
}

type AnyModel = Record<string, any>;
type AnyLoadedModel = Record<string, any>;

function asString(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

function asNumber(value: unknown): number | undefined {
  const number = Number(value);
  return Number.isFinite(number) ? number : undefined;
}

function modelName(model: AnyModel | null | undefined): string {
  return asString(model?.model_name) || asString(model?.name) || asString(model?.id);
}

function displayName(model: AnyModel | null | undefined): string {
  return asString(model?.display_name) || modelName(model);
}

function lowerLabels(model: AnyModel | null | undefined): string[] {
  return Array.isArray(model?.labels) ? model.labels.map((label: unknown) => String(label).toLowerCase()) : [];
}

function isDownloaded(model: AnyModel | null | undefined): boolean {
  if (!model) return false;
  if (model.downloaded === true) return true;
  return lowerLabels(model).some(label => ['downloaded', 'local', 'installed', 'ready'].includes(label));
}

function recipeNames(model: AnyModel | null | undefined): string[] {
  const directRecipe = asString(model?.recipe);
  const recipes = Array.isArray(model?.recipes) ? model.recipes : [];
  const names = recipes
    .map((recipe: unknown) => {
      if (typeof recipe === 'string') return recipe;
      if (recipe && typeof recipe === 'object') {
        const record = recipe as Record<string, unknown>;
        return asString(record.name) || Object.keys(record)[0] || '';
      }
      return '';
    })
    .filter(Boolean);
  if (directRecipe && !names.includes(directRecipe)) names.unshift(directRecipe);
  return Array.from(new Set(names));
}

function capabilityForModel(model: AnyModel | null | undefined): string {
  const labels = lowerLabels(model);
  const type = asString(model?.type).toLowerCase();
  const haystack = `${type} ${labels.join(' ')} ${asString(model?.recipe).toLowerCase()} ${modelName(model).toLowerCase()}`;
  if (haystack.includes('omni') || haystack.includes('collection')) return 'omni';
  if (labels.some(label => ['image', 'image-generation', 'diffusion', 'image-edit', 'upscaling'].includes(label)) || type === 'image') return 'image';
  if (labels.some(label => ['audio', 'transcription', 'stt', 'speech-to-text', 'realtime-transcription'].includes(label)) || type === 'audio') return 'audio';
  if (labels.some(label => ['tts', 'speech', 'text-to-speech'].includes(label)) || type === 'tts') return 'tts';
  if (labels.includes('embedding') || type === 'embedding') return 'embedding';
  if (labels.includes('reranking') || type === 'reranking' || type === 'rerank') return 'reranking';
  return 'chat';
}

function formatSize(gb: unknown): string | undefined {
  const size = asNumber(gb);
  if (!size || size <= 0) return undefined;
  if (size >= 1) return `${size.toFixed(1)} GB`;
  if (size >= 0.01) return `${(size * 1000).toFixed(0)} MB`;
  return '< 1 MB';
}

function modelSummary(model: AnyModel, loaded?: AnyLoadedModel | null): Record<string, unknown> {
  return {
    id: modelName(model),
    name: displayName(model),
    status: loaded ? 'loaded' : (isDownloaded(model) ? 'downloaded' : 'registry'),
    capability: capabilityForModel(model),
    size: formatSize(model.size) || model.size,
    recipe: asString(model.recipe) || undefined,
    recipes: recipeNames(model),
    backend: loaded?.recipe || undefined,
    device: loaded?.device || undefined,
    context_window: model.max_context_window || model.context_length || model.n_ctx || undefined,
    labels: Array.isArray(model.labels) ? model.labels.slice(0, 8) : undefined,
  };
}

function includesQuery(model: AnyModel, query: string): boolean {
  if (!query) return true;
  const haystack = [
    modelName(model),
    displayName(model),
    asString(model.checkpoint),
    asString(model.recipe),
    lowerLabels(model).join(' '),
  ].join(' ').toLowerCase();
  return query.toLowerCase().split(/\s+/).every(part => haystack.includes(part));
}

function resolveModel(models: AnyModel[], loaded: AnyLoadedModel[], requested: unknown): AnyModel | null {
  const raw = asString(requested);
  if (!raw) return null;
  const needle = raw.toLowerCase();
  const loadedNames = new Set(loaded.map(model => asString(model.model_name).toLowerCase()).filter(Boolean));
  return models.find(model => modelName(model).toLowerCase() === needle)
    || models.find(model => displayName(model).toLowerCase() === needle)
    || models.find(model => modelName(model).toLowerCase().includes(needle) && (isDownloaded(model) || loadedNames.has(modelName(model).toLowerCase())))
    || models.find(model => modelName(model).toLowerCase().includes(needle))
    || null;
}

function loadedFor(model: AnyModel | null, loaded: AnyLoadedModel[]): AnyLoadedModel | null {
  if (!model) return null;
  const name = modelName(model).toLowerCase();
  return loaded.find(item => asString(item.model_name).toLowerCase() === name) || null;
}

function recipeOptionsFromDetail(detail: AnyModel): Record<string, unknown>[] {
  return Array.isArray(detail.recipes) ? detail.recipes : [];
}

function extractRecipeBackendOptions(detail: AnyModel): Record<string, unknown>[] {
  return recipeOptionsFromDetail(detail).map(recipe => {
    const record = recipe && typeof recipe === 'object' ? recipe as Record<string, any> : { name: String(recipe) };
    const name = asString(record.name) || Object.keys(record)[0] || '';
    const backends = record.backends || record.available_backends || record.backend_options || record[name]?.backends;
    return { name, backends };
  }).filter(option => option.name);
}


function uniqueBy<T>(items: T[], keyFn: (item: T) => string): T[] {
  const seen = new Set<string>();
  const out: T[] = [];
  for (const item of items) {
    const key = keyFn(item);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push(item);
  }
  return out;
}

function shortJson(value: unknown, maxChars = 220): string {
  try {
    const text = JSON.stringify(value);
    return text.length > maxChars ? `${text.slice(0, maxChars - 1)}…` : text;
  } catch {
    return String(value).slice(0, maxChars);
  }
}

function cleanValue(value: unknown): string {
  const text = String(value ?? '').trim();
  return text && text.toLowerCase() !== 'unknown' ? text : '';
}

function summarizeDevice(name: string, value: unknown): Record<string, unknown> | null {
  if (!value || typeof value !== 'object') return null;
  const record = value as Record<string, unknown>;
  const label = cleanValue(record.name) || cleanValue(record.family) || name;
  const available = record.available !== false;
  const details: string[] = [];
  if (record.cores) details.push(`${record.cores} cores`);
  if (record.threads) details.push(`${record.threads} threads`);
  if (record.vram_gb) details.push(`${record.vram_gb} GB VRAM`);
  if (record.tops_max_int) details.push(`${record.tops_max_int} TOPS`);
  return { type: name, name: label, available, details };
}

function summarizeSystemInfo(info: Record<string, unknown>): Record<string, unknown> {
  const devices = (info.devices && typeof info.devices === 'object' ? info.devices : {}) as Record<string, unknown>;
  const deviceSummary: Record<string, unknown[]> = {};
  for (const [key, raw] of Object.entries(devices)) {
    const values = Array.isArray(raw) ? raw : raw ? [raw] : [];
    const summarized = values.map(item => summarizeDevice(key, item)).filter(Boolean) as Record<string, unknown>[];
    if (summarized.length > 0) deviceSummary[key] = summarized;
  }

  const recipes = (info.recipes && typeof info.recipes === 'object' ? info.recipes : {}) as Record<string, any>;
  let installedBackends = 0;
  let installableBackends = 0;
  const recipeSummary: Record<string, unknown> = {};
  for (const [recipe, recipeInfo] of Object.entries(recipes)) {
    const backends = recipeInfo?.backends && typeof recipeInfo.backends === 'object' ? recipeInfo.backends : {};
    const backendSummary: Record<string, unknown> = {};
    for (const [backend, backendInfo] of Object.entries(backends as Record<string, any>)) {
      const state = cleanValue(backendInfo?.state) || 'unknown';
      if (state === 'installed') installedBackends++;
      if (state === 'installable' || state === 'update_available' || state === 'update_required') installableBackends++;
      backendSummary[backend] = {
        state,
        version: cleanValue(backendInfo?.version) || undefined,
        devices: Array.isArray(backendInfo?.devices) ? backendInfo.devices : undefined,
        message: cleanValue(backendInfo?.message) || undefined,
      };
    }
    recipeSummary[recipe] = {
      default_backend: cleanValue(recipeInfo?.default_backend) || undefined,
      backends: backendSummary,
    };
  }

  return {
    os: cleanValue(info['OS Version']) || cleanValue(info.os_version) || undefined,
    lemonade_version: cleanValue(info.lemonade_version) || cleanValue(info.version) || undefined,
    devices: deviceSummary,
    recipes: recipeSummary,
    counts: {
      device_groups: Object.keys(deviceSummary).length,
      recipes: Object.keys(recipeSummary).length,
      installed_backends: installedBackends,
      installable_or_update_backends: installableBackends,
    },
    answer_instruction: 'Summarize the OS/version, detected CPU/GPU/NPU devices, and installed/installable backend status for the user. Do not just say the call succeeded.',
  };
}

function linesForSystemInfo(summary: Record<string, any>): string {
  const lines: string[] = [];
  if (summary.os) lines.push(`OS: ${summary.os}`);
  if (summary.lemonade_version) lines.push(`Lemonade: ${summary.lemonade_version}`);
  const devices = summary.devices || {};
  for (const [kind, items] of Object.entries(devices)) {
    const labels = Array.isArray(items)
      ? items.map((item: any) => `${item.name}${item.available === false ? ' (unavailable)' : ''}${Array.isArray(item.details) && item.details.length ? ` — ${item.details.join(', ')}` : ''}`)
      : [];
    if (labels.length) lines.push(`${kind}: ${labels.join('; ')}`);
  }
  const counts = summary.counts || {};
  lines.push(`Backends: ${counts.installed_backends || 0} installed, ${counts.installable_or_update_backends || 0} installable/update, across ${counts.recipes || 0} recipes`);
  return lines.join('\n');
}

function summarizeBackends(info: Record<string, unknown>): Record<string, unknown> {
  const recipes = (info.recipes && typeof info.recipes === 'object' ? info.recipes : {}) as Record<string, any>;
  const summary: Record<string, unknown> = {};
  for (const [recipe, rInfo] of Object.entries(recipes)) {
    const backends: Record<string, { state: string; version?: string; devices?: string[]; message?: string }> = {};
    const rawBackends = rInfo?.backends && typeof rInfo.backends === 'object' ? rInfo.backends : {};
    for (const [backend, bInfo] of Object.entries(rawBackends as Record<string, any>)) {
      backends[backend] = {
        state: cleanValue(bInfo?.state) || 'unknown',
        version: cleanValue(bInfo?.version) || undefined,
        devices: Array.isArray(bInfo?.devices) ? bInfo.devices : undefined,
        message: cleanValue(bInfo?.message) || undefined,
      };
    }
    summary[recipe] = { default_backend: cleanValue(rInfo?.default_backend) || undefined, backends };
  }
  return summary;
}

function backendDisplayLines(summary: Record<string, any>): string {
  const lines: string[] = [];
  for (const [recipe, rInfo] of Object.entries(summary)) {
    const backends = Object.entries((rInfo as any).backends || {})
      .map(([backend, bInfo]: [string, any]) => `${backend}: ${bInfo.state}${bInfo.version ? ` ${bInfo.version}` : ''}`)
      .join(', ');
    lines.push(`${recipe}${(rInfo as any).default_backend ? ` (default ${(rInfo as any).default_backend})` : ''}: ${backends || 'no backends'}`);
  }
  return lines.join('\n');
}

function modelChoiceLines(models: AnyModel[], loaded: AnyLoadedModel[], limit = 8): string[] {
  return models.slice(0, limit).map(model => {
    const loadedItem = loadedFor(model, loaded);
    const summary = modelSummary(model, loadedItem);
    const bits = [String(summary.name || summary.id), String(summary.status || ''), String(summary.capability || '')].filter(Boolean);
    if (summary.size) bits.push(String(summary.size));
    if (Array.isArray(summary.recipes) && summary.recipes.length) bits.push(`recipes: ${summary.recipes.join(', ')}`);
    return bits.join(' · ');
  });
}

function registryMatches(models: AnyModel[], loaded: AnyLoadedModel[], requested: string): AnyModel[] {
  if (!requested) return [];
  const needle = requested.toLowerCase();
  const exact = models.filter(model => modelName(model).toLowerCase() === needle || displayName(model).toLowerCase() === needle);
  if (exact.length > 0) return exact;
  const queryMatches = models.filter(model => includesQuery(model, requested));
  if (queryMatches.length > 0) return uniqueBy(queryMatches, model => modelName(model).toLowerCase());
  const fuzzy = models.filter(model => {
    const haystack = `${modelName(model)} ${displayName(model)} ${asString(model.checkpoint)} ${lowerLabels(model).join(' ')}`.toLowerCase();
    return requested.toLowerCase().split(/\s+/).filter(Boolean).some(part => haystack.includes(part));
  });
  return uniqueBy(fuzzy, model => modelName(model).toLowerCase());
}

function chooseHfVariant(variants: PullVariantsResult, requestedVariant = '') {
  const options = Array.isArray(variants.variants) ? variants.variants : [];
  if (options.length === 0) return null;
  const wanted = requestedVariant.trim().toLowerCase();
  if (wanted) {
    return options.find(option => option.name.toLowerCase() === wanted || option.primary_file.toLowerCase() === wanted || option.name.toLowerCase().includes(wanted) || option.primary_file.toLowerCase().includes(wanted)) || null;
  }
  const preferredTokens = ['q4_k_m', 'q4-k-m', 'q4_k_s', 'q5_k_m', 'q5-k-m', 'q8_0'];
  return options.find(option => preferredTokens.some(token => `${option.name} ${option.primary_file}`.toLowerCase().includes(token))) || (options.length === 1 ? options[0] : null);
}

function defaultHfLocalName(variants: PullVariantsResult, checkpoint: string): string {
  const suggested = cleanValue(variants.suggested_name) || checkpoint.split('/').pop() || checkpoint;
  return suggested.startsWith('user.') ? suggested : `user.${suggested}`;
}

async function pullWithProgress(modelName: string, opts?: Record<string, unknown>): Promise<Record<string, unknown>> {
  return new Promise<Record<string, unknown>>((resolve, reject) => {
    let lastPercent = 0;
    let lastEvent: Record<string, unknown> = {};
    api.pullModel(modelName, {
      onProgress: (d) => {
        lastEvent = { ...lastEvent, ...d };
        if (typeof d.percent === 'number') lastPercent = d.percent;
      },
      onComplete: (d) => resolve({ status: 'complete', percent: lastPercent || (typeof d.percent === 'number' ? d.percent : 100), ...lastEvent, ...d }),
      onError: (err) => reject(err),
    }, opts);
  });
}

function toolPayload(call: ToolCall, result: unknown, displayResult?: string, error = false): ToolResult {
  return {
    tool_call_id: call.id,
    role: 'tool',
    content: JSON.stringify(result),
    displayResult,
    error,
  };
}

/**
 * Execute a tool call against the lemonade API.
 * Returns a serialized result suitable for sending back to the LLM.
 */
export async function executeTool(call: ToolCall): Promise<ToolResult> {
  const name = call.function.name;
  let args: Record<string, unknown> = {};
  try {
    args = JSON.parse(call.function.arguments || '{}');
  } catch {
    return toolPayload(call, { error: 'Invalid arguments JSON', received: call.function.arguments || '' }, 'Error: invalid tool arguments', true);
  }

  try {
    switch (name) {
      case 'list_models': {
        const [data, health] = await Promise.all([
          api.models(true),
          api.health().catch(() => null),
        ]);
        const loaded = health?.all_models_loaded || [];
        const loadedNames = new Set(loaded.map(model => asString(model.model_name).toLowerCase()).filter(Boolean));
        const query = asString(args.query);
        const status = (asString(args.status) || 'local').toLowerCase();
        const capability = asString(args.capability).toLowerCase();
        const limit = Math.max(1, Math.min(100, Math.round(asNumber(args.limit) || 30)));
        const all = (data.data as AnyModel[]).filter(model => {
          if (!includesQuery(model, query)) return false;
          if (capability && capabilityForModel(model) !== capability) return false;
          return true;
        });
        const loadedItems = loaded
          .filter(model => !query || asString(model.model_name).toLowerCase().includes(query.toLowerCase()))
          .map(model => {
            const registry = all.find(info => modelName(info).toLowerCase() === asString(model.model_name).toLowerCase());
            return modelSummary(registry || { id: model.model_name, name: model.model_name, recipe: model.recipe, type: model.type }, model);
          });
        const downloaded = all.filter(model => !loadedNames.has(modelName(model).toLowerCase()) && isDownloaded(model)).map(model => modelSummary(model));
        const registry = all.filter(model => !loadedNames.has(modelName(model).toLowerCase()) && !isDownloaded(model)).map(model => modelSummary(model));
        const wanted = status === 'loaded' ? { loaded: loadedItems.slice(0, limit) }
          : status === 'downloaded' ? { downloaded: downloaded.slice(0, limit) }
            : status === 'registry' ? { registry: registry.slice(0, limit) }
              : status === 'all' ? { loaded: loadedItems.slice(0, limit), downloaded: downloaded.slice(0, limit), registry: registry.slice(0, limit) }
                : { loaded: loadedItems.slice(0, limit), downloaded: downloaded.slice(0, limit) };
        const result = {
          scope: status,
          query: query || undefined,
          capability: capability || undefined,
          counts: {
            total_registry_known: data.data.length,
            matching_total: all.length,
            loaded: loadedItems.length,
            downloaded: downloaded.length,
            registry: registry.length,
          },
          ...wanted,
          answer_instruction: status === 'local'
            ? 'Answer using the loaded/downloaded model names. Do not mention registry-only models unless asked.'
            : 'Answer using the returned concrete model names and their statuses. Do not stop at a raw count.',
          next_step: 'For a named-model question call get_model_info. For backend/recipe/hardware questions call list_backends or get_system_info. For a load request call load_model. For a download request call pull_model.',
        };
        const lines = [
          `${loadedItems.length} loaded, ${downloaded.length} downloaded${status === 'registry' || status === 'all' ? `, ${registry.length} registry-only` : ''}`,
          ...modelChoiceLines(all.filter(model => status === 'registry' ? !isDownloaded(model) : true), loaded, 6),
        ].filter(Boolean).join('\n');
        return toolPayload(call, result, lines || 'Model inventory retrieved');
      }

      case 'get_model_info': {
        const data = await api.models(true);
        const health = await api.health().catch(() => null);
        const loaded = health?.all_models_loaded || [];
        const resolved = resolveModel(data.data as AnyModel[], loaded, args.model_name);
        if (!resolved) {
          const requested = asString(args.model_name);
          const matches = registryMatches(data.data as AnyModel[], loaded, requested).slice(0, 8);
          const result = {
            error: `Model not found: ${requested || '(missing model_name)'}`,
            suggestions: matches.map(model => modelSummary(model, loadedFor(model, loaded))),
            answer_instruction: matches.length > 0 ? 'Tell the user the model name was ambiguous and ask which suggestion they meant.' : 'Tell the user no matching local/registry model was found. Suggest pull_model with source=huggingface if they want to search Hugging Face.',
          };
          return toolPayload(call, result, matches.length ? `Model not found exactly. Suggestions:\n${modelChoiceLines(matches, loaded).join('\n')}` : 'Model not found', true);
        }
        const detail = await api.modelDetail(modelName(resolved)).catch(() => resolved);
        const loadedItem = loadedFor(resolved, loaded);
        const summary = modelSummary(detail as AnyModel, loadedItem);
        const result = {
          ...summary,
          raw_name: modelName(detail as AnyModel),
          downloaded: isDownloaded(detail as AnyModel),
          loaded: Boolean(loadedItem),
          checkpoint: (detail as AnyModel).checkpoint,
          context_window: (detail as AnyModel).max_context_window || (detail as AnyModel).context_length || (detail as AnyModel).n_ctx,
          recipe_options: extractRecipeBackendOptions(detail as AnyModel),
          answer_instruction: 'Answer the user with the model status, capability, size/context, checkpoint, and recipe/backend options. If they asked to load it, call load_model next.',
        };
        const display = [
          `${summary.name || summary.id} — ${summary.status}, ${summary.capability}`,
          summary.size ? `Size: ${summary.size}` : '',
          result.context_window ? `Context: ${result.context_window}` : '',
          result.checkpoint ? `Checkpoint: ${result.checkpoint}` : '',
          Array.isArray(summary.recipes) && summary.recipes.length ? `Recipes: ${summary.recipes.join(', ')}` : '',
        ].filter(Boolean).join('\n');
        return toolPayload(call, result, display || 'Model info retrieved');
      }

      case 'load_model': {
        const data = await api.models(true).catch(() => ({ data: [] as AnyModel[] }));
        const health = await api.health().catch(() => null);
        const loaded = health?.all_models_loaded || [];
        const resolved = resolveModel(data.data as AnyModel[], loaded, args.model_name);
        const targetModelName = resolved ? modelName(resolved) : asString(args.model_name);
        if (!targetModelName) {
          return toolPayload(call, { error: 'Missing model_name. Use list_models with a query to resolve a downloaded model first.' }, 'Error: missing model name', true);
        }
        const opts: Record<string, unknown> = {};
        const recipeArg = typeof args.recipe === 'string' ? args.recipe.trim().toLowerCase() : '';
        const backendArg = typeof args.backend === 'string' ? args.backend.trim().toLowerCase()
          : (typeof args.device === 'string' ? args.device.trim().toLowerCase() : '');
        let recipe = recipeArg;
        let backend = backendArg;

        const llamacppMatch = /^llamacpp[-_:](cpu|vulkan|rocm|metal|cuda|gpu)$/i.exec(recipeArg);
        if (llamacppMatch) {
          recipe = 'llamacpp';
          backend = llamacppMatch[1].toLowerCase();
        }
        if (backend === 'gpu' && (!recipe || recipe === 'llamacpp')) backend = 'vulkan';

        if (recipe) opts.recipe = recipe;
        if (backend) {
          if (recipe === 'llamacpp' || !recipe) {
            opts.llamacpp_backend = backend;
            if (backend === 'cpu') opts.llamacpp_device = 'cpu';
          } else if (recipe.includes('whisper')) {
            opts.whispercpp_backend = backend;
          } else if (recipe.includes('moonshine')) {
            opts.moonshine_backend = backend;
          } else if (recipe.includes('vllm')) {
            opts.vllm_backend = backend;
          } else if (recipe.includes('sd-cpp')) {
            opts.sd_cpp_backend = backend;
          }
        }
        if (args.n_ctx) opts.n_ctx = args.n_ctx;
        if (args.n_gpu_layers) opts.n_gpu_layers = args.n_gpu_layers;
        const response = await api.loadModel(targetModelName, Object.keys(opts).length > 0 ? opts : undefined, resolved as any || null);
        const result = {
          status: 'loaded',
          model: targetModelName,
          options: opts,
          response,
          answer_instruction: 'Tell the user the model was loaded, including the model name and selected recipe/backend if any.',
        };
        return toolPayload(call, result, `Loaded ${targetModelName}${Object.keys(opts).length ? ` with ${shortJson(opts, 120)}` : ''}`);
      }

      case 'unload_model': {
        const requested = asString(args.model_name) || undefined;
        const response = await api.unloadModel(requested);
        const result = { status: 'unloaded', model: requested || 'most recently used model', response, answer_instruction: 'Tell the user which model was unloaded.' };
        return toolPayload(call, result, `Unloaded ${requested || 'most recently used model'}`);
      }

      case 'get_loaded_models': {
        const health = await api.health();
        const loaded = health.all_models_loaded.map(m => ({
          model: m.model_name,
          recipe: m.recipe,
          device: m.device,
          type: m.type,
        }));
        const result = {
          loaded,
          limits: health.max_models,
          answer_instruction: 'List the currently loaded model names with recipe/device/type. If none are loaded, say none are loaded.',
        };
        const display = loaded.length
          ? loaded.map(m => `${m.model} — ${m.recipe || 'recipe unknown'}${m.device ? ` on ${m.device}` : ''} (${m.type || 'type unknown'})`).join('\n')
          : 'No models are currently loaded';
        return toolPayload(call, result, display);
      }

      case 'get_server_health': {
        const h = await api.health();
        const result = {
          status: h.status,
          version: h.version,
          loaded_models: h.all_models_loaded.length,
          loaded: h.all_models_loaded.map(m => ({ model: m.model_name, recipe: m.recipe, device: m.device, type: m.type })),
          max_models: h.max_models,
          answer_instruction: 'Summarize server status/version, loaded model count/names, and resource limits.',
        };
        const display = [
          `Server: ${h.status} (${h.version})`,
          `Loaded: ${h.all_models_loaded.length}`,
          ...h.all_models_loaded.slice(0, 6).map(m => `${m.model_name} — ${m.recipe || 'recipe unknown'}${m.device ? ` on ${m.device}` : ''}`),
        ].join('\n');
        return toolPayload(call, result, display);
      }

      case 'pull_model': {
        const requested = asString(args.model_name);
        const query = asString(args.query) || requested;
        const source = (asString(args.source) || 'auto').toLowerCase();
        const checkpoint = asString(args.checkpoint);
        const variant = asString(args.variant);
        const recipe = asString(args.recipe) || 'llamacpp';
        if (!requested && !query && !checkpoint) {
          return toolPayload(call, { error: 'Missing model_name/query/checkpoint for pull_model.' }, 'Error: missing model/checkpoint for download', true);
        }

        if (checkpoint || source === 'huggingface') {
          const hfCheckpoint = checkpoint || (query.includes('/') ? query : '');
          if (!hfCheckpoint) {
            const results = await searchHuggingFace(query);
            const candidates = results.slice(0, 6).map((item: HFModelResult) => ({ id: item.id || item.modelId, downloads: item.downloads, likes: item.likes, tags: item.tags?.slice(0, 8) }));
            const result = {
              status: 'needs_choice',
              source: 'huggingface',
              query,
              candidates,
              answer_instruction: 'Ask the user which Hugging Face checkpoint to download. Use ask_question with the candidate ids as choices.',
            };
            return toolPayload(call, result, candidates.length ? `Choose a Hugging Face checkpoint:\n${candidates.map(c => `${c.id} — ${c.downloads || 0} downloads`).join('\n')}` : `No Hugging Face GGUF checkpoints found for "${query}"`, candidates.length === 0);
          }
          const variants = await api.pullVariants(hfCheckpoint);
          const chosen = chooseHfVariant(variants, variant);
          if (!chosen) {
            const choices = (variants.variants || []).slice(0, 8).map(v => ({ name: v.name, file: v.primary_file, size_bytes: v.size_bytes }));
            const result = {
              status: 'needs_choice',
              source: 'huggingface',
              checkpoint: hfCheckpoint,
              suggested_local_name: defaultHfLocalName(variants, hfCheckpoint),
              recipe,
              variants: choices,
              answer_instruction: 'Ask the user which GGUF variant/file to download. Use ask_question with the variant names as choices.',
            };
            return toolPayload(call, result, choices.length ? `Choose a GGUF variant for ${hfCheckpoint}:\n${choices.map(c => `${c.name} — ${c.file}`).join('\n')}` : `No downloadable variants found for ${hfCheckpoint}`, choices.length === 0);
          }
          const localName = requested && !requested.includes('/') ? requested : defaultHfLocalName(variants, hfCheckpoint);
          const pullResult = await pullWithProgress(localName, { checkpoint: `${hfCheckpoint}:${chosen.name}`, recipe });
          const result = {
            status: 'downloaded',
            source: 'huggingface',
            model: localName,
            checkpoint: hfCheckpoint,
            variant: chosen.name,
            recipe,
            response: pullResult,
            answer_instruction: 'Tell the user the Hugging Face checkpoint/variant was downloaded and name the local model.',
          };
          return toolPayload(call, result, `Downloaded ${localName}\nHF: ${hfCheckpoint}\nVariant: ${chosen.name}`);
        }

        const data = await api.models(true);
        const health = await api.health().catch(() => null);
        const loaded = health?.all_models_loaded || [];
        const matches = registryMatches(data.data as AnyModel[], loaded, requested || query);
        if (matches.length > 1) {
          const choices = matches.slice(0, 8).map(model => modelName(model));
          const result = {
            status: 'needs_choice',
            source: 'registry',
            query: requested || query,
            choices,
            matches: matches.slice(0, 8).map(model => modelSummary(model, loadedFor(model, loaded))),
            answer_instruction: 'Ask the user which registry model to download. Use ask_question with choices.',
          };
          return toolPayload(call, result, `Several registry models match:\n${modelChoiceLines(matches, loaded).join('\n')}`);
        }
        if (matches.length === 1) {
          const target = modelName(matches[0]);
          const pullResult = await pullWithProgress(target);
          const result = {
            status: 'downloaded',
            source: 'registry',
            model: target,
            response: pullResult,
            answer_instruction: 'Tell the user the registry model was downloaded and can now be loaded.',
          };
          return toolPayload(call, result, `Downloaded ${target}`);
        }

        if (source === 'registry') {
          return toolPayload(call, { error: `No registry model found for ${requested || query}` }, `No registry model found for ${requested || query}`, true);
        }

        const results = await searchHuggingFace(query);
        const candidates = results.slice(0, 6).map((item: HFModelResult) => ({ id: item.id || item.modelId, downloads: item.downloads, likes: item.likes, tags: item.tags?.slice(0, 8) }));
        const result = {
          status: 'needs_choice',
          source: 'huggingface',
          query,
          registry_matches: [],
          candidates,
          answer_instruction: 'No registry match was found. Ask the user which Hugging Face GGUF checkpoint to download. Use ask_question with the candidate ids as choices.',
        };
        return toolPayload(call, result, candidates.length ? `No registry match. Hugging Face candidates:\n${candidates.map(c => `${c.id} — ${c.downloads || 0} downloads`).join('\n')}` : `No registry or Hugging Face GGUF candidates found for "${query}"`, candidates.length === 0);
      }

      case 'delete_model': {
        const requested = asString(args.model_name);
        if (!requested) return toolPayload(call, { error: 'Missing model_name.' }, 'Error: missing model name', true);
        const response = await api.deleteModel(requested);
        const result = { status: 'deleted', model: requested, response, answer_instruction: 'Tell the user the model was deleted from local storage.' };
        return toolPayload(call, result, `Deleted ${requested}`);
      }

      case 'get_system_info': {
        const info = await api.systemInfo();
        const summary = summarizeSystemInfo(info);
        return toolPayload(call, summary, linesForSystemInfo(summary));
      }

      case 'list_backends': {
        const info = await api.systemInfo();
        const summary = summarizeBackends(info);
        if (Object.keys(summary).length === 0) {
          return toolPayload(call, { error: 'No recipe/backend data available from server', answer_instruction: 'Tell the user recipe/backend data was not available from the server.' }, 'No recipe/backend data available', true);
        }
        const result = {
          recipes: summary,
          answer_instruction: 'Summarize installed/installable/unsupported backends by recipe. Mention CPU/GPU/NPU support where devices are present.',
        };
        return toolPayload(call, result, backendDisplayLines(summary as Record<string, any>));
      }

      case 'install_backend': {
        const recipe = asString(args.recipe);
        const backend = asString(args.backend);
        if (!recipe || !backend) {
          return toolPayload(call, { error: 'recipe and backend are required.' }, 'Error: recipe and backend are required', true);
        }
        const installResult = await new Promise<string>((resolve, reject) => {
          api.installBackend(recipe, backend, {
            onProgress: () => {},
            onComplete: () => resolve('installation complete'),
            onError: (err) => reject(err),
          });
        });
        const fresh = await api.systemInfo().catch(() => null);
        const backendState = fresh ? (fresh as any).recipes?.[recipe]?.backends?.[backend]?.state : undefined;
        const result = {
          status: installResult,
          recipe,
          backend,
          backend_state: backendState,
          answer_instruction: 'Tell the user installation/update completed and include the final backend state if available.',
        };
        return toolPayload(call, result, `${recipe}/${backend}: ${backendState || installResult}`);
      }

      case 'ask_question': {
        const question = asString(args.question);
        const choices = Array.isArray(args.choices) ? args.choices.map(choice => String(choice)) : [];
        const result = {
          status: 'presented',
          question,
          choices,
          allowCustom: args.allowCustom !== false,
          answer_instruction: 'Do not repeat all choices; the UI renders them as buttons. Briefly tell the user to choose an option.',
        };
        return toolPayload(call, result, `Question presented: ${question}`);
      }

      default:
        return toolPayload(call, { error: `Unknown tool: ${name}` }, `Error: unknown tool ${name}`, true);
    }
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return toolPayload(call, { error: msg, tool: name, args }, `Error: ${msg}`, true);
  }
}

