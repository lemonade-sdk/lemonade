import api, { ChatMessage, ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import {
  findModelInfoByName,
  getAudioTranscriptionComponent,
  getCollectionComponents,
  getPrimaryChatComponent,
  getVisionChatComponent,
} from '../features/collections/collectionModels';
import type { ChatToolRuntime, ToolCall, ToolExecutionPayload, ToolArtifact } from '../hooks/useChatStreaming';
import { COLLECTION_IMAGE_SIZE } from '../features/collections/collectionImageConfig';

interface ToolDefinitionEntry {
  requires_labels?: string[];
  requires_llm_labels?: string[];
  prompt_guidance?: string;
  function: { name: string; description: string; parameters: Record<string, unknown> };
}

const DEFAULT_IMAGE_SIZE = COLLECTION_IMAGE_SIZE;
const MAX_IMAGE_DIMENSION = 2048;
const IMAGE_DIMENSION_STEP = 8;

// Keep these built-in schemas aligned with GUI2/main's Omni tool definitions.
// GUI3 currently owns Omni runtime wiring in omniTools.ts; when a shared
// definition file exists in the target branch, only this data block should move.
const DEFAULT_OMNI_TOOLS: ToolDefinitionEntry[] = [
  {
    "requires_labels": [
      "image"
    ],
    "function": {
      "name": "generate_image",
      "description": "Generate a NEW image from scratch based on a text description. Use this only for a new image, not for modifying an existing image.",
      "parameters": {
        "type": "object",
        "properties": {
          "prompt": {
            "type": "string",
            "description": "A detailed description of the image to generate."
          },
          "size": {
            "type": "string",
            "description": "Optional output canvas size as WIDTHxHEIGHT pixels. Omit by default to use {image_size}. Use only when the user explicitly asks for the final output image/canvas dimensions or a specific aspect ratio. For aspect-ratio requests, choose reasonable dimensions and round dimensions to the nearest multiple of 8."
          },
          "steps": {
            "type": "integer",
            "description": "Optional sampling/denoising step count. Use only when the user asks for speed, quality, detail, or a specific step count.",
            "minimum": 1,
            "maximum": 100
          },
          "cfg_scale": {
            "type": "number",
            "description": "Optional text guidance scale. Higher values follow the prompt more strongly; lower values allow more variation. Use only when requested or clearly implied.",
            "minimum": 0
          },
          "seed": {
            "type": "integer",
            "description": "Optional random seed for reproducible generation. Use when the user provides a seed or asks for reproducibility."
          },
          "sample_method": {
            "type": "string",
            "description": "Optional sampler name/method. Use only when the user explicitly requests a sampler."
          },
          "flow_shift": {
            "type": "number",
            "description": "Optional flow shift value for models/backends that support it. Use only when requested or model guidance requires it.",
            "minimum": 0
          }
        },
        "required": [
          "prompt"
        ],
        "additionalProperties": false
      }
    }
  },
  {
    "requires_labels": [
      "edit"
    ],
    "prompt_guidance": "When an image already exists in the conversation and the user asks to add, remove, change, modify, or adjust it, use edit_image rather than generate_image. Use generate_image for creating a brand new image from scratch. The edit_image tool automatically uses the most recent image as its source. If the user explicitly asks for final output canvas dimensions or aspect ratio for the edited image, set size with dimensions rounded to the nearest multiple of 8; otherwise omit size.",
    "function": {
      "name": "edit_image",
      "description": "Edit or modify the most recent generated image from this conversation. Use this for requested changes to an existing image, not for creating a brand new image.",
      "parameters": {
        "type": "object",
        "properties": {
          "prompt": {
            "type": "string",
            "description": "A description of the desired edit or modification."
          },
          "size": {
            "type": "string",
            "description": "Optional output canvas size as WIDTHxHEIGHT pixels for the edited image. Omit by default to preserve the edit source/default canvas. Use only when the user explicitly asks for final output image/canvas dimensions or aspect ratio. For aspect-ratio requests, choose reasonable dimensions and round dimensions to the nearest multiple of 8."
          },
          "steps": {
            "type": "integer",
            "description": "Optional sampling/denoising step count. Use only when the user asks for speed, quality, detail, or a specific step count.",
            "minimum": 1,
            "maximum": 100
          },
          "cfg_scale": {
            "type": "number",
            "description": "Optional text guidance scale. Higher values follow the prompt more strongly; lower values allow more variation. Use only when requested or clearly implied.",
            "minimum": 0
          },
          "seed": {
            "type": "integer",
            "description": "Optional random seed for reproducible generation. Use when the user provides a seed or asks for reproducibility."
          },
          "sample_method": {
            "type": "string",
            "description": "Optional sampler name/method. Use only when the user explicitly requests a sampler."
          },
          "flow_shift": {
            "type": "number",
            "description": "Optional flow shift value for models/backends that support it. Use only when requested or model guidance requires it.",
            "minimum": 0
          }
        },
        "required": [
          "prompt"
        ],
        "additionalProperties": false
      }
    }
  },
  {
    "requires_labels": [
      "tts"
    ],
    "function": {
      "name": "text_to_speech",
      "description": "Convert text to spoken audio. Use this when the user asks you to speak, say, read aloud, or convert text to speech.",
      "parameters": {
        "type": "object",
        "properties": {
          "input": {
            "type": "string",
            "description": "The text to convert to speech"
          },
          "voice": {
            "type": "string",
            "description": "Voice to use for speech synthesis",
            "default": "af_heart"
          }
        },
        "required": [
          "input"
        ],
        "additionalProperties": false
      }
    }
  },
  {
    "requires_labels": [
      "transcription"
    ],
    "function": {
      "name": "transcribe_audio",
      "description": "Transcribe audio to text (speech-to-text). Use this when the user provides an audio file or when you see '[User provided audio file #N]' placeholders in the conversation. The audio data is automatically provided by the system, just call this tool with the language parameter.",
      "parameters": {
        "type": "object",
        "properties": {
          "language": {
            "type": "string",
            "description": "Language of the audio (ISO 639-1 code, e.g. 'en', 'es', 'fr')",
            "default": "en"
          }
        },
        "required": [],
        "additionalProperties": false
      }
    }
  },
  {
    "requires_llm_labels": [
      "vision"
    ],
    "prompt_guidance": "When the user sends an image (as an image_url in their message), use the analyze_image tool to look at the image before responding about it.",
    "function": {
      "name": "analyze_image",
      "description": "Analyze, describe, or answer questions about an image. Use this when the user shares an image and asks you to look at it, describe it, read text from it, identify objects, or answer any question about what's in the image.",
      "parameters": {
        "type": "object",
        "properties": {
          "image_url": {
            "type": "string",
            "description": "The URL or base64 data URI of the image to analyze"
          },
          "question": {
            "type": "string",
            "description": "The question to answer about the image, or 'describe' for a general description"
          }
        },
        "required": [
          "image_url",
          "question"
        ],
        "additionalProperties": false
      }
    }
  }
] as ToolDefinitionEntry[];

export const DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE = [
  'You are a helpful multimodal AI assistant with access to the following tools:',
  '',
  '{tool_list}',
  '',
  'When the user asks you to perform an action that matches one of these tools, use the appropriate tool.',
  'You may call multiple tools if the request requires it.',
  'After using a tool, describe what you did to the user in a brief, friendly response.',
  "If the user's request does not require any tool, respond normally with text.{tool_guidance}",
].join('\n');
type ComponentRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';

interface OmniFunctionTool extends Record<string, unknown> {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, unknown>;
  };
}

function labelsFor(model: ModelInfo | null | undefined): string[] {
  return (model?.labels || []).map(label => label.toLowerCase().trim()).filter(Boolean);
}

function componentRole(model: ModelInfo, role: ComponentRole): string | null {
  const raw = (model as any).component_roles?.[role];
  return typeof raw === 'string' && raw.trim() ? raw.trim() : null;
}

function componentHasAnyLabel(componentName: string | null | undefined, allModels: ModelInfo[], labels: string[]): boolean {
  const info = findModelInfoByName(allModels, componentName || '');
  if (!info) return false;
  const wanted = new Set(labels.map(label => label.toLowerCase()));
  return labelsFor(info).some(label => wanted.has(label));
}

function componentByLabels(model: ModelInfo, allModels: ModelInfo[], labels: string[]): string | null {
  const wanted = new Set(labels.map(label => label.toLowerCase()));
  return getCollectionComponents(model).find(component => {
    const info = findModelInfoByName(allModels, component);
    return labelsFor(info).some(label => wanted.has(label));
  }) || null;
}

function componentByCapability(model: ModelInfo, allModels: ModelInfo[], capability: 'image' | 'tts'): string | null {
  const components = getCollectionComponents(model);
  return components.find(component => {
    const info = findModelInfoByName(allModels, component);
    if (!info) return false;
    const cap = capabilityFromModelInfo(info);
    const labels = labelsFor(info);
    if (cap === capability) return true;
    if (capability === 'image') return labels.some(label => ['image', 'image-generation', 'diffusion', 'upscaling', 'edit'].includes(label));
    return labels.some(label => ['tts', 'speech', 'text-to-speech'].includes(label));
  }) || null;
}

function makeTool(name: string, description: string, parameters: Record<string, unknown>): OmniFunctionTool {
  return { type: 'function', function: { name, description, parameters } };
}

function substituteImageSize(text: string): string {
  return text.split('{image_size}').join(COLLECTION_IMAGE_SIZE);
}

function substituteImageSizeDeep<T>(value: T): T {
  if (typeof value === 'string') return substituteImageSize(value) as T;
  if (Array.isArray(value)) return value.map(item => substituteImageSizeDeep(item)) as T;
  if (isPlainObject(value)) {
    const out: Record<string, unknown> = {};
    for (const [key, nested] of Object.entries(value)) out[key] = substituteImageSizeDeep(nested);
    return out as T;
  }
  return value;
}

function materializeDefaultTool(def: ToolDefinitionEntry): OmniFunctionTool {
  return {
    type: 'function',
    function: {
      name: def.function.name,
      description: substituteImageSize(def.function.description),
      parameters: substituteImageSizeDeep(def.function.parameters),
    },
  };
}

function defaultToolDefinition(name: string): ToolDefinitionEntry | null {
  return DEFAULT_OMNI_TOOLS.find(def => def.function?.name === name) || null;
}

function defaultToolOrFallback(name: string, description: string, parameters: Record<string, unknown>): OmniFunctionTool {
  const def = defaultToolDefinition(name);
  return def ? materializeDefaultTool(def) : makeTool(name, description, parameters);
}

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

function customSystemPromptTemplate(model: ModelInfo): string {
  const direct = stringValue((model as any).system_prompt);
  if (direct) return direct;
  const options = (model as any).recipe_options;
  if (options && typeof options === 'object' && !Array.isArray(options)) {
    const fromOptions = stringValue((options as Record<string, unknown>).system_prompt)
      || stringValue((options as Record<string, unknown>).omni_system_prompt);
    if (fromOptions) return fromOptions;
  }
  return DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE;
}

function replaceToken(value: string, token: string, replacement: string): string {
  return value.split(token).join(replacement);
}

function renderOmniSystemPrompt(template: string, toolList: string, toolGuidance: string): string {
  const source = template.trim() || DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE;
  const hasToolList = source.includes('{tool_list}');
  const hasToolGuidance = source.includes('{tool_guidance}');
  const guidanceBlock = toolGuidance.trim() ? `\n\nAdditional tool guidance:\n${toolGuidance.trim()}` : '';

  let rendered = replaceToken(source, '{tool_list}', toolList || 'No Omni tools available.');
  rendered = replaceToken(rendered, '{tool_guidance}', guidanceBlock);

  // Be forgiving for hand-written prompts: keep the collection usable even if a
  // user removes one of the placeholders from the template.
  if (!hasToolList && toolList.trim()) rendered = `${rendered}\n\nAvailable Omni tools:\n${toolList}`;
  if (!hasToolGuidance && guidanceBlock) rendered = `${rendered}${guidanceBlock}`;
  return rendered.trim();
}

function artifactFromDataUrl(url: string, type: 'image' | 'audio', name?: string): ToolArtifact {
  const mimeMatch = /^data:([^;,]+)[;,]/.exec(url);
  return { type, url, mime: mimeMatch?.[1], name };
}

function latestImage(context: OmniToolContext): string | null {
  const candidates = [...context.previousImages, ...context.attachedImages].filter(Boolean);
  return candidates.length > 0 ? candidates[candidates.length - 1] : null;
}

export interface OmniToolContext {
  attachedImages: string[];
  attachedAudioFiles: File[];
  previousImages: string[];
}

interface OmniComponentMap {
  generate_image?: string;
  edit_image?: string;
  text_to_speech?: string;
  transcribe_audio?: string;
  analyze_image?: string;
}

interface CustomOmniLlmTool {
  id?: string;
  name: string;
  description: string;
  target_model: string;
  system_prompt?: string;
  prompt_template?: string;
  parameters?: Record<string, unknown>;
  max_tokens?: number;
}

const DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS: Record<string, unknown> = {
  type: 'object',
  properties: {
    task: { type: 'string', description: 'The focused task to delegate to the target model.' },
    context: { type: 'string', description: 'Optional context, constraints, code, or review material for the target model.' },
  },
  required: ['task'],
  additionalProperties: false,
};

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function normalizeToolName(value: unknown): string {
  return String(value || '')
    .trim()
    .replace(/\s+/g, '_')
    .replace(/[^a-zA-Z0-9_-]/g, '_')
    .replace(/_+/g, '_')
    .replace(/^([^a-zA-Z_])/, '_$1')
    .slice(0, 64);
}

function normalizeCustomLlmTools(model: ModelInfo): CustomOmniLlmTool[] {
  const direct = Array.isArray((model as any).custom_tools) ? (model as any).custom_tools : [];
  const options = (model as any).recipe_options;
  const fromOptions = options && typeof options === 'object' && !Array.isArray(options)
    ? ((Array.isArray((options as Record<string, unknown>).custom_tools)
      ? (options as Record<string, unknown>).custom_tools
      : (Array.isArray((options as Record<string, unknown>).customTools) ? (options as Record<string, unknown>).customTools : [])) as unknown[])
    : [];
  const seen = new Set<string>();
  const tools: CustomOmniLlmTool[] = [];
  for (const raw of [...direct, ...fromOptions]) {
    if (!isPlainObject(raw)) continue;
    const name = normalizeToolName(raw.name);
    const targetModel = stringValue(raw.target_model) || stringValue(raw.targetModel) || stringValue(raw.model);
    if (!name || !targetModel || seen.has(name.toLowerCase())) continue;
    seen.add(name.toLowerCase());
    const maxTokens = Number(raw.max_tokens ?? raw.maxTokens);
    tools.push({
      id: stringValue(raw.id) || name,
      name,
      description: stringValue(raw.description) || `Delegate a focused task to ${targetModel}.`,
      target_model: targetModel,
      system_prompt: stringValue(raw.system_prompt) || stringValue(raw.systemPrompt) || undefined,
      prompt_template: stringValue(raw.prompt_template) || stringValue(raw.promptTemplate) || undefined,
      parameters: isPlainObject(raw.parameters) ? raw.parameters : DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS,
      max_tokens: Number.isFinite(maxTokens) && maxTokens > 0 ? Math.floor(maxTokens) : undefined,
    });
  }
  return tools;
}

function stringifyArgs(args: Record<string, unknown>): string {
  try { return JSON.stringify(args, null, 2); } catch { return String(args); }
}

function renderCustomToolPrompt(template: string | undefined, args: Record<string, unknown>): string {
  const argJson = stringifyArgs(args);
  const fallback = [
    'You are being called as an internal tool by the planner model.',
    'Use the provided arguments to complete the delegated task.',
    'Return a concise, concrete result that the planner can use in its final answer.',
    '',
    'Tool arguments:',
    '{arguments}',
  ].join('\n');
  let rendered = (template && template.trim()) || fallback;
  rendered = rendered.split('{arguments}').join(argJson);
  for (const [key, value] of Object.entries(args)) {
    rendered = rendered.split(`{${key}}`).join(typeof value === 'string' ? value : stringifyArgs({ [key]: value }));
  }
  if (!rendered.includes(argJson) && !template?.includes('{arguments}')) {
    rendered = `${rendered}\n\nTool arguments:\n${argJson}`;
  }
  return rendered.trim();
}

export function buildOmniToolRuntime(
  collectionModel: ModelInfo,
  allModels: ModelInfo[],
  context: OmniToolContext,
): ChatToolRuntime | null {
  const plannerModel = componentRole(collectionModel, 'llm') || getPrimaryChatComponent(collectionModel, allModels) || '';
  const imageModel = componentRole(collectionModel, 'image') || componentByLabels(collectionModel, allModels, ['image', 'image-generation']) || componentByCapability(collectionModel, allModels, 'image');
  const editModel = componentRole(collectionModel, 'edit') || componentByLabels(collectionModel, allModels, ['edit']) || (componentHasAnyLabel(imageModel, allModels, ['edit']) ? imageModel : null);
  const ttsModel = componentRole(collectionModel, 'speech') || componentByLabels(collectionModel, allModels, ['tts', 'text-to-speech']) || componentByCapability(collectionModel, allModels, 'tts');
  const transcriptionModel = componentRole(collectionModel, 'transcription') || componentByLabels(collectionModel, allModels, ['transcription', 'realtime-transcription', 'asr']) || getAudioTranscriptionComponent(collectionModel, allModels);
  const visionModel = componentRole(collectionModel, 'vision')
    || (componentHasAnyLabel(plannerModel, allModels, ['vision', 'vision-language', 'vlm', 'image-input']) ? plannerModel : null)
    || getVisionChatComponent(collectionModel, allModels);

  const tools: OmniFunctionTool[] = [];
  const models: OmniComponentMap = {};
  const customLlmTools = normalizeCustomLlmTools(collectionModel);
  const customToolMap = new Map<string, CustomOmniLlmTool>();

  const builtInGuidance: string[] = [];
  const includeDefaultTool = (toolName: keyof OmniComponentMap, modelName: string | null | undefined) => {
    if (!modelName) return;
    const def = defaultToolDefinition(toolName);
    if (def?.prompt_guidance) builtInGuidance.push(substituteImageSize(def.prompt_guidance));
    tools.push(defaultToolOrFallback(toolName, `Run ${toolName}.`, { type: 'object', properties: {}, required: [] }));
    models[toolName] = modelName;
  };

  includeDefaultTool('generate_image', imageModel);
  includeDefaultTool('edit_image', editModel);
  includeDefaultTool('text_to_speech', ttsModel);
  includeDefaultTool('transcribe_audio', transcriptionModel);
  includeDefaultTool('analyze_image', visionModel);

  const reservedToolNames = new Set(tools.map(tool => tool.function.name));
  for (const tool of customLlmTools) {
    if (reservedToolNames.has(tool.name)) continue;
    tools.push(makeTool(tool.name, tool.description, tool.parameters || DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS));
    customToolMap.set(tool.name, tool);
    reservedToolNames.add(tool.name);
  }

  if (tools.length === 0) return null;

  const toolList = tools.map(tool => `- ${tool.function.name}: ${tool.function.description}`).join('\n');
  const toolGuidance = [
    ...builtInGuidance,
    models.text_to_speech ? 'Use text_to_speech when the user asks you to speak, read aloud, narrate, or create audio.' : '',
    models.analyze_image ? "If the model cannot provide a real image_url for analyze_image, it may pass the visible placeholder; the UI will safely use the latest attached/generated image." : '',
    models.transcribe_audio ? "When the user message contains '[User provided audio file #N]', call transcribe_audio before answering audio-specific questions. Include language only when the user specified it or it is obvious." : '',
    customToolMap.size > 0 ? `Custom LLM tools delegate work to other local models: ${Array.from(customToolMap.values()).map(tool => `${tool.name} -> ${tool.target_model}`).join(', ')}.` : '',
    customToolMap.size > 0 ? 'Do not recursively ask custom LLM tools to call tools. They return plain text for you to use.' : '',
    'After a tool succeeds, give a brief friendly response. Do not include base64 data in your message; the UI renders media artifacts automatically.',
    `Planner model: ${plannerModel || String((collectionModel as any).name || collectionModel.id || 'planner')}`,
  ].filter(Boolean).join('\n');
  const systemPrompt = renderOmniSystemPrompt(customSystemPromptTemplate(collectionModel), toolList, toolGuidance);

  const execute = async (call: ToolCall): Promise<ToolExecutionPayload> => {
    const name = call.function.name;
    let args: Record<string, unknown> = {};
    try { args = JSON.parse(call.function.arguments || '{}'); } catch { /* keep empty args */ }

    const customTool = customToolMap.get(name);
    if (customTool) {
      try {
        const messages: ChatMessage[] = [];
        messages.push({
          role: 'system',
          content: customTool.system_prompt || `You are ${customTool.name}, a focused internal assistant tool. Complete delegated tasks and return concise, actionable results.`,
        });
        messages.push({ role: 'user', content: renderCustomToolPrompt(customTool.prompt_template, args) });
        const params = customTool.max_tokens ? { max_tokens: customTool.max_tokens } : {};
        const content = await api.chatCompletionOnce(customTool.target_model, messages, params);
        return textResult(call.id, content || `${customTool.name} completed without text output.`, `${customTool.name} completed`);
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        return textResult(call.id, JSON.stringify({ error: message }), `Error: ${message}`, true);
      }
    }

    const builtInName = name as keyof OmniComponentMap;
    const requestedModel = models[builtInName];
    const sourceImage = latestImage(context);
    const shouldAutoEdit = builtInName === 'generate_image' && !!sourceImage && !!models.edit_image && looksLikeImageEdit(args.prompt);
    const effectiveName: keyof OmniComponentMap = shouldAutoEdit ? 'edit_image' : builtInName;
    const model = shouldAutoEdit ? models.edit_image : requestedModel;
    if (!model) {
      return {
        tool_call_id: call.id,
        role: 'tool',
        content: JSON.stringify({ error: `Omni tool '${call.function.name}' is not available for this collection.` }),
        error: true,
        displayResult: `Error: ${call.function.name} unavailable`,
      };
    }

    try {
      if (effectiveName === 'generate_image') {
        const prompt = String(args.prompt || '').trim();
        if (!prompt) throw new Error('generate_image requires a prompt.');
        const urls = await api.imageGeneration(model, prompt, imageGenerationOptions(args));
        return mediaResult(call.id, 'Image generated successfully.', urls.map(url => artifactFromDataUrl(url, 'image')));
      }

      if (effectiveName === 'edit_image') {
        const prompt = String(args.prompt || '').trim();
        if (!prompt) throw new Error('edit_image requires a prompt.');
        if (!sourceImage) throw new Error('No previous image is available to edit.');
        const urls = await api.imageEdit(model, prompt, sourceImage, imageEditOptions(args));
        return mediaResult(call.id, 'Image edited successfully.', urls.map(url => artifactFromDataUrl(url, 'image')));
      }

      if (effectiveName === 'text_to_speech') {
        const input = String(args.input || '').trim();
        if (!input) throw new Error('text_to_speech requires input text.');
        const voice = typeof args.voice === 'string' && args.voice.trim() ? args.voice.trim() : 'af_heart';
        const audio = await api.textToSpeech(model, input, voice, { response_format: 'mp3' });
        return mediaResult(call.id, 'Audio generated successfully.', [{ type: 'audio', url: audio.url, mime: audio.blob.type || 'audio/mpeg', name: `${model}.mp3` }]);
      }

      if (effectiveName === 'transcribe_audio') {
        const file = context.attachedAudioFiles[0];
        if (!file) throw new Error('No audio file was provided.');
        const language = typeof args.language === 'string' && args.language.trim() ? args.language.trim() : undefined;
        const text = await api.audioTranscription(model, file, language);
        return textResult(call.id, `Transcription: ${text}`, `Transcribed ${file.name}`);
      }

      if (effectiveName === 'analyze_image') {
        // Only trust LLM-provided image_url values if they are same-message
        // data URLs. Otherwise fall back to the UI-owned latest image. This
        // mirrors GUI2/main and prevents arbitrary http:/file:/javascript: URLs
        // from being sent through the backend because a planner hallucinated one.
        const rawImageUrl = typeof args.image_url === 'string' ? args.image_url.trim() : '';
        const imageUrl = rawImageUrl.startsWith('data:image/') ? rawImageUrl : sourceImage;
        if (!imageUrl) throw new Error('No image was provided.');
        const question = String(args.question || 'Describe this image.');
        const content = await api.chatCompletionOnce(model, [{
          role: 'user',
          content: [
            { type: 'image_url', image_url: { url: imageUrl } },
            { type: 'text', text: question },
          ],
        } as ChatMessage]);
        return textResult(call.id, content || 'Image analyzed.', 'Image analyzed');
      }

      return textResult(call.id, `Unknown Omni tool: ${call.function.name}`, 'Error: unknown tool', true);
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      return textResult(call.id, JSON.stringify({ error: message }), `Error: ${message}`, true);
    }
  };

  return { tools, execute, systemPrompt };
}

function looksLikeImageEdit(value: unknown): boolean {
  const text = String(value || '').toLowerCase();
  return ['add ', 'remove ', 'change ', 'modify ', 'edit ', 'adjust ', 'fix ', 'replace ', 'make it ', 'turn it '].some(needle => text.includes(needle));
}

function imageGenerationOptions(args: Record<string, unknown>): Record<string, unknown> {
  return {
    n: 1,
    size: resolveImageSize(args),
    ...optionalImageArgs(args),
  };
}

function imageEditOptions(args: Record<string, unknown>): Record<string, unknown> {
  const opts: Record<string, unknown> = { n: 1, ...optionalImageArgs(args) };
  const explicitSize = resolveExplicitImageSize(args);
  if (explicitSize) opts.size = explicitSize;
  return opts;
}

function resolveImageSize(args: Record<string, unknown>): string {
  const explicitSize = resolveExplicitImageSize(args);
  if (explicitSize) return explicitSize;

  // Be conservative: prompt text describes image content, not the output canvas.
  // Only the explicit size tool argument can change canvas dimensions.
  return DEFAULT_IMAGE_SIZE;
}

function resolveExplicitImageSize(args: Record<string, unknown>): string {
  return parseSizeFromText(typeof args.size === 'string' ? args.size : '');
}

function parseSizeFromText(text: string): string {
  if (!text) return '';
  const match = text.match(/(?<!\d)(\d{2,4})\s*(?:x|×|by)\s*(\d{2,4})(?!\d)/i);
  if (!match) return '';
  return formatImageSize(Number(match[1]), Number(match[2]));
}

function formatImageSize(width: number, height: number): string {
  const roundedWidth = normalizeImageDimension(width);
  const roundedHeight = normalizeImageDimension(height);
  return roundedWidth !== null && roundedHeight !== null ? `${roundedWidth}x${roundedHeight}` : '';
}

function normalizeImageDimension(value: number): number | null {
  if (!Number.isInteger(value) || value <= 0) return null;
  const rounded = Math.round(value / IMAGE_DIMENSION_STEP) * IMAGE_DIMENSION_STEP;
  return Math.min(MAX_IMAGE_DIMENSION, Math.max(IMAGE_DIMENSION_STEP, rounded));
}

function coerceInteger(value: unknown): number | null {
  if (typeof value === 'number' && Number.isInteger(value)) return value;
  if (typeof value === 'string' && /^\d+$/.test(value.trim())) return Number(value.trim());
  return null;
}

function coerceNumber(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim()) {
    const parsed = Number(value.trim());
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
}

function optionalImageArgs(args: Record<string, unknown>): Record<string, unknown> {
  const opts: Record<string, unknown> = {};
  const steps = coerceInteger(args.steps);
  if (steps !== null && steps > 0) opts.steps = steps;

  const cfgScale = coerceNumber(args.cfg_scale);
  if (cfgScale !== null && cfgScale > 0) opts.cfg_scale = cfgScale;

  const seed = coerceInteger(args.seed);
  if (seed !== null) opts.seed = seed;

  if (typeof args.sample_method === 'string' && args.sample_method.trim()) {
    opts.sample_method = args.sample_method.trim();
  }

  const flowShift = coerceNumber(args.flow_shift);
  if (flowShift !== null && flowShift > 0) opts.flow_shift = flowShift;
  return opts;
}

function textResult(toolCallId: string, content: string, displayResult: string, error = false): ToolExecutionPayload {
  return {
    tool_call_id: toolCallId,
    role: 'tool',
    content,
    displayResult,
    error,
  };
}

function mediaResult(toolCallId: string, message: string, artifacts: ToolArtifact[]): ToolExecutionPayload {
  return {
    tool_call_id: toolCallId,
    role: 'tool',
    content: message,
    displayResult: message,
    artifacts,
  };
}
