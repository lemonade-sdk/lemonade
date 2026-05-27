/**
 * Lemonade tool definitions and executor.
 * Exposes lemonade server management as OpenAI-compatible function calling tools.
 */
import api from '../api';

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
      description: 'List all models known to the lemonade server (loaded, downloaded, and registry). Returns model names, status, available recipes, and sizes. Each model may support multiple recipes — a recipe defines how the model runs (e.g. "llamacpp" for GPU/CPU via llama.cpp, "flm" for NPU via FastFlowLM).',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_model_info',
      description: 'Get detailed info about a specific model including its available recipes, size, and labels. IMPORTANT: Call this before load_model to check which recipes a model supports. If multiple recipes are available (e.g. llamacpp and flm), present the options to the user and ask which they prefer before loading. Suggest llamacpp (GPU) as the recommended default.',
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
      description: 'Load a model into the server for inference. A "recipe" defines the inference backend: llamacpp (GPU/CPU via llama.cpp — backends: vulkan, rocm, metal, cpu), flm (NPU via FastFlowLM), ryzenai-llm (hybrid NPU). Combined forms like "llamacpp-vulkan" or "llamacpp-cpu" select a specific backend. Always specify a recipe — do not omit it. If the user did not specify one and the model has multiple recipes, ask them first (call get_model_info to check).',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to load.' },
          recipe: { type: 'string', description: 'The recipe to use. Examples: "llamacpp" (auto-selects best GPU/CPU backend), "llamacpp-vulkan" (AMD/NVIDIA GPU), "llamacpp-cpu" (CPU only), "flm" (NPU), "ryzenai-llm" (hybrid NPU). Always specify — check get_model_info for available options.' },
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
      description: 'Download a model from the registry to local storage. Models must be pulled before they can be loaded. This may take a while for large models.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to download.' },
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
      description: 'List all recipes and their backends with install status. A recipe (e.g. llamacpp, whispercpp, flm, kokoro, sd-cpp, vllm) defines the inference engine. Each recipe has backends (e.g. vulkan, rocm, cpu, metal, npu) that target specific hardware. Returns install state (installed, installable, update_available, update_required), version, and supported devices for each. Check this before recommending a recipe — if the user needs GPU but vulkan is not installed, suggest installing it first.',
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
          recipe: { type: 'string', description: 'The recipe name (e.g. "llamacpp", "whispercpp", "sd-cpp", "kokoro", "flm", "vllm").' },
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
    return { tool_call_id: call.id, role: 'tool', content: JSON.stringify({ error: 'Invalid arguments JSON' }) };
  }

  try {
    let result: unknown;

    switch (name) {
      case 'list_models': {
        const data = await api.models(true);
        // Summarize for the LLM — full data can be huge
        const models = data.data.map(m => ({
          id: m.id,
          name: m.display_name || m.name || m.id,
          labels: m.labels,
          size: m.size,
          recipes: m.recipes ? (m.recipes as Record<string, unknown>[]).map(r => (r as Record<string, unknown>).name || Object.keys(r)[0]) : undefined,
        }));
        result = { count: models.length, models };
        break;
      }

      case 'get_model_info': {
        const detail = await api.modelDetail(args.model_name as string);
        result = detail;
        break;
      }

      case 'load_model': {
        const opts: Record<string, unknown> = {};
        if (args.recipe) opts.recipe = args.recipe;
        if (args.n_ctx) opts.n_ctx = args.n_ctx;
        if (args.n_gpu_layers) opts.n_gpu_layers = args.n_gpu_layers;
        result = await api.loadModel(args.model_name as string, Object.keys(opts).length > 0 ? opts : undefined);
        break;
      }

      case 'unload_model': {
        result = await api.unloadModel(args.model_name as string | undefined);
        break;
      }

      case 'get_loaded_models': {
        const health = await api.health();
        result = {
          loaded: health.all_models_loaded.map(m => ({
            model: m.model_name,
            recipe: m.recipe,
            device: m.device,
            type: m.type,
          })),
          limits: health.max_models,
        };
        break;
      }

      case 'get_server_health': {
        const h = await api.health();
        result = {
          status: h.status,
          version: h.version,
          loaded_models: h.all_models_loaded.length,
          max_models: h.max_models,
        };
        break;
      }

      case 'pull_model': {
        // Pull is long-running with SSE progress. Execute and return final status.
        const pullResult = await new Promise<string>((resolve, reject) => {
          let lastPercent = 0;
          api.pullModel(args.model_name as string, {
            onProgress: (d) => { if (d.percent !== undefined) lastPercent = d.percent; },
            onComplete: () => resolve(`Download complete (${lastPercent}%)`),
            onError: (err) => reject(err),
          });
        });
        result = { status: pullResult, model: args.model_name };
        break;
      }

      case 'delete_model': {
        result = await api.deleteModel(args.model_name as string);
        break;
      }

      case 'get_system_info': {
        result = await api.systemInfo();
        break;
      }

      case 'list_backends': {
        const info = await api.systemInfo();
        const recipes = info.recipes as Record<string, { default_backend: string; backends: Record<string, { devices: string[]; state: string; version: string; message: string }> }> | undefined;
        if (!recipes) {
          result = { error: 'No recipe data available from server' };
        } else {
          const summary: Record<string, unknown> = {};
          for (const [recipe, rInfo] of Object.entries(recipes)) {
            const backends: Record<string, { state: string; version: string; devices: string[] }> = {};
            for (const [backend, bInfo] of Object.entries(rInfo.backends)) {
              backends[backend] = { state: bInfo.state, version: bInfo.version, devices: bInfo.devices };
            }
            summary[recipe] = { default_backend: rInfo.default_backend, backends };
          }
          result = summary;
        }
        break;
      }

      case 'install_backend': {
        const installResult = await new Promise<string>((resolve, reject) => {
          api.installBackend(args.recipe as string, args.backend as string, {
            onProgress: () => {},
            onComplete: () => resolve('Installation complete'),
            onError: (err) => reject(err),
          });
        });
        result = { status: installResult, recipe: args.recipe, backend: args.backend };
        break;
      }

      case 'ask_question': {
        // The UI renders the interactive buttons directly from the tool call data.
        // Just confirm to the LLM that choices were presented.
        const question = args.question as string;
        const choices = args.choices as string[];
        result = {
          status: 'presented',
          message: `Interactive choices presented to user: "${question}" with options: ${choices.join(', ')}. The user will click a button to respond. You may reference the question in your response but do NOT list the choices as text — they are already shown as interactive buttons.`,
        };
        break;
      }

      default:
        result = { error: `Unknown tool: ${name}` };
    }

    return { tool_call_id: call.id, role: 'tool', content: JSON.stringify(result) };
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return { tool_call_id: call.id, role: 'tool', content: JSON.stringify({ error: msg }) };
  }
}


