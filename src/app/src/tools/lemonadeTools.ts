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
      description: 'List all models known to the lemonade server — loaded, downloaded, and registry. Returns model names, status, recipes, and sizes.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_model_info',
      description: 'Get detailed info about a specific model including its available recipes, size, and labels.',
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
      description: 'Load a model into the server for inference. Optionally specify a recipe (e.g. "llamacpp-vulkan", "flm") and recipe options like context size.',
      parameters: {
        type: 'object',
        properties: {
          model_name: { type: 'string', description: 'The model name/ID to load.' },
          recipe: { type: 'string', description: 'The recipe to use (e.g. "llamacpp-vulkan", "llamacpp-cpu", "flm"). Optional — server picks the best if omitted.' },
          n_ctx: { type: 'number', description: 'Context window size. Optional.' },
          n_gpu_layers: { type: 'number', description: 'Number of GPU layers. Optional.' },
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
      description: 'List only the currently loaded models with their recipe, device, and resource usage.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_server_health',
      description: 'Get server health status including version, loaded models, and resource limits.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'pull_model',
      description: 'Download a model from the registry to local storage. This may take a while for large models.',
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
      description: 'Delete a downloaded model from local storage.',
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
      description: 'Get system hardware info including CPU, GPU, VRAM, and NPU capabilities.',
      parameters: { type: 'object', properties: {}, required: [] },
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

      default:
        result = { error: `Unknown tool: ${name}` };
    }

    return { tool_call_id: call.id, role: 'tool', content: JSON.stringify(result) };
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return { tool_call_id: call.id, role: 'tool', content: JSON.stringify({ error: msg }) };
  }
}

/* ── System prompt for tool-aware conversations ────────────────── */

export const TOOLS_SYSTEM_PROMPT = `You are a helpful assistant with access to the lemonade local LLM server. You can manage models, check server status, and configure the server using the available tools.

When the user asks you to load, unload, list, download, or delete models, or check server status, use the appropriate tool. After using a tool, summarize the result in a friendly way.

Key concepts:
- Models can be loaded with different "recipes" (e.g. llamacpp-vulkan for GPU, llamacpp-cpu for CPU, flm for NPU)
- Multiple models can be loaded simultaneously (within server limits)
- Models must be downloaded (pulled) before they can be loaded
- The server has resource limits (max loaded models per type)`;
