import { serverFetch } from './serverConfig';
import { ModelsData } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { getCollectionComponents } from './collectionModels';
import { COLLECTION_IMAGE_SIZE } from './collectionImageConfig';
import toolDefinitions from './toolDefinitions.json';

// Neutral fallback used for actual image API requests. Do not use the old
// collection canvas default here; otherwise missing planner args still generate
// 512x256 / 256x512-looking images.
const DEFAULT_IMAGE_SIZE = '512x512';
const MAX_IMAGE_DIMENSION = 2048;

const ASPECT_RATIO_TO_SIZE: Record<string, string> = {
  '1:1': '512x512',
  '16:9': '1024x576',
  '9:16': '576x1024',
  '4:3': '768x576',
  '3:4': '576x768',
  '3:2': '768x512',
  '2:3': '512x768',
};

const ORIENTATION_TO_SIZE: Record<string, string> = {
  square: '512x512',
  landscape: '768x512',
  wide: '768x512',
  horizontal: '768x512',
  portrait: '512x768',
  vertical: '512x768',
  tall: '512x768',
};

// Types
export interface LemonadeToolDef {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, any>;
  };
}

interface ToolDefinitionEntry {
  requires_labels?: string[];
  requires_llm_labels?: string[];
  prompt_guidance?: string;
  function: { name: string; description: string; parameters: Record<string, any> };
}

export interface LemonadeToolsResult {
  tools: LemonadeToolDef[];
  systemPrompt: string;
  models: Record<string, string>;
}

export interface ToolExecutionContext {
  extractedAudio: Array<{ data: string; mime: string }>;
  extractedImages: Array<{ dataUrl: string }>;
  previousArtifacts: Array<{ type: string; data: string; mime: string }>;
}

export interface ToolExecutionResult {
  type: 'image' | 'audio' | 'text';
  data?: string;
  mime?: string;
  text?: string;
}

/**
 * Build tools, system prompt, and model map from a collection model's components.
 * Tool definitions are loaded from toolDefinitions.json — the single source of truth.
 */
export function buildLemonadeTools(
  collectionName: string,
  modelsData: ModelsData,
): LemonadeToolsResult {
  const info = modelsData[collectionName];
  const components = getCollectionComponents(info);

  const llmModel = components.find(c => isChatPlannerCandidate(modelsData[c])) || components[0] || '';

  const tools: LemonadeToolDef[] = [];
  const models: Record<string, string> = {};

  const substituteParams = (params: Record<string, any>): Record<string, any> => {
    const props = params?.properties as Record<string, any> | undefined;
    if (!props) return params;
    const newProps: Record<string, any> = {};
    for (const [key, prop] of Object.entries(props)) {
      newProps[key] = typeof prop?.description === 'string' && prop.description.includes('{image_size}')
        ? { ...prop, description: prop.description.replace(/\{image_size\}/g, COLLECTION_IMAGE_SIZE) }
        : prop;
    }
    return { ...params, properties: newProps };
  };

  const materialize = (def: ToolDefinitionEntry): LemonadeToolDef => ({
    type: 'function',
    function: { ...def.function, parameters: substituteParams(def.function.parameters) },
  });

  // Per-tool prompt guidance, collected only for the tools actually included so
  // the system prompt never references a tool the planner doesn't have.
  const guidance: string[] = [];
  const include = (def: ToolDefinitionEntry, model: string) => {
    tools.push(materialize(def));
    models[def.function.name] = model;
    if (def.prompt_guidance) guidance.push(def.prompt_guidance);
  };

  for (const def of (toolDefinitions.tools as ToolDefinitionEntry[])) {
    const requiresLabels = def.requires_labels;
    const requiresLlmLabels = def.requires_llm_labels;

    if (requiresLabels) {
      const labelSet = new Set(requiresLabels);
      const match = components.find(c => {
        const labels = modelsData[c]?.labels ?? [];
        return labels.some(l => labelSet.has(l));
      });
      if (!match) continue;
      include(def, match);
      continue;
    }

    if (requiresLlmLabels) {
      const labelSet = new Set(requiresLlmLabels);
      const llmLabels = modelsData[llmModel]?.labels ?? [];
      if (!llmLabels.some(l => labelSet.has(l))) continue;
      include(def, llmModel);
    }
  }

  const toolList = tools.map(t => `- ${t.function.name}: ${t.function.description}`).join('\n');
  const toolGuidance = guidance.length ? `\n${guidance.join('\n')}` : '';
  const systemPrompt = toolDefinitions.system_prompt
    .replace('{tool_list}', toolList)
    .replace('{tool_guidance}', toolGuidance);

  return { tools, systemPrompt, models };
}

/**
 * Execute a single Lemonade tool call.
 */
export async function executeLemonadeTool(
  toolCall: { function: { name: string; arguments: string } },
  model: string,
  context: ToolExecutionContext,
  modelsData?: ModelsData,
  signal?: AbortSignal,
): Promise<ToolExecutionResult> {
  const funcName = toolCall.function.name;
  let args: Record<string, any>;
  try {
    args = JSON.parse(toolCall.function.arguments);
  } catch (e) {
    console.warn(`[LemonadeTools] Failed to parse arguments for ${funcName}:`, e);
    args = {};
  }

  const hasPreviousImage = context.previousArtifacts.some(a => a.type === 'image');
  const modelLabels = modelsData?.[model]?.labels ?? [];
  const modelSupportsEdit = modelLabels.includes('edit');
  const effectiveName = (funcName === 'generate_image' && hasPreviousImage && modelSupportsEdit) ? 'edit_image' : funcName;

  if (effectiveName === 'generate_image' || effectiveName === 'edit_image') {
    return executeImageTool(effectiveName, args, model, context, signal);
  }
  if (effectiveName === 'text_to_speech') {
    return executeTTSTool(args, model, signal);
  }
  if (effectiveName === 'transcribe_audio') {
    return executeTranscriptionTool(args, model, context, signal);
  }
  if (effectiveName === 'analyze_image') {
    return executeVisionTool(args, model, context, signal);
  }

  return { type: 'text', text: `Unknown tool: ${funcName}` };
}

async function executeImageTool(
  effectiveName: string,
  args: Record<string, any>,
  model: string,
  context: ToolExecutionContext,
  signal?: AbortSignal,
): Promise<ToolExecutionResult> {
  const imageSize = resolveImageSize(args);
  const isEdit = effectiveName === 'edit_image';

  if (isEdit) {
    // /images/edits requires multipart/form-data
    const formData = new FormData();
    formData.append('model', model);
    formData.append('prompt', args.prompt || '');
    formData.append('response_format', 'b64_json');
    formData.append('n', '1');
    formData.append('size', imageSize);
    appendOptionalImageFormArgs(args, formData);

    // Attach the most recent image as the source file
    const lastImage = [...context.previousArtifacts].reverse().find(a => a.type === 'image');
    if (lastImage) {
      const binaryStr = atob(lastImage.data);
      const bytes = new Uint8Array(binaryStr.length);
      for (let i = 0; i < binaryStr.length; i++) {
        bytes[i] = binaryStr.charCodeAt(i);
      }
      formData.append('image', new Blob([bytes], { type: lastImage.mime || 'image/png' }), 'image.png');
    }

    const response = await serverFetch('/images/edits', {
      method: 'POST',
      body: formData,
      signal,
    });

    const data = await response.json();
    if (data.data?.[0]?.b64_json) {
      return { type: 'image', data: data.data[0].b64_json, mime: 'image/png' };
    }
    throw new Error(data.error?.message || 'Image edit failed');
  }

  // /images/generations accepts JSON
  const body: Record<string, any> = {
    model,
    prompt: args.prompt || '',
    response_format: 'b64_json',
    n: 1,
    size: imageSize,
  };
  copyOptionalImageArgs(args, body);

  const response = await serverFetch('/images/generations', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  });

  const data = await response.json();
  if (data.data?.[0]?.b64_json) {
    return { type: 'image', data: data.data[0].b64_json, mime: 'image/png' };
  }
  throw new Error(data.error?.message || 'Image generation failed');
}

function resolveImageSize(args: Record<string, any>): string {
  const explicitSize = parseSizeFromText(typeof args.size === 'string' ? args.size : '');
  if (explicitSize) return explicitSize;

  const width = coerceInteger(args.width);
  const height = coerceInteger(args.height);
  const widthHeightSize = width !== null && height !== null ? formatImageSize(width, height) : '';
  if (widthHeightSize) return widthHeightSize;

  const promptSize = parseSizeFromText(typeof args.prompt === 'string' ? args.prompt : '');
  if (promptSize) return promptSize;

  const inferredSize = inferSizeFromRatioOrOrientation(args);
  if (inferredSize) return inferredSize;

  return DEFAULT_IMAGE_SIZE;
}

function parseSizeFromText(text: string): string {
  if (!text) return '';
  const match = text.match(/(?<!\d)(\d{2,4})\s*(?:x|×|by)\s*(\d{2,4})(?!\d)/i);
  if (match) {
    return formatImageSize(Number(match[1]), Number(match[2]));
  }

  const widthMatch = text.match(/\bwidth\s*[:=]?\s*(\d{2,4})\b/i);
  const heightMatch = text.match(/\bheight\s*[:=]?\s*(\d{2,4})\b/i);
  if (widthMatch && heightMatch) {
    return formatImageSize(Number(widthMatch[1]), Number(heightMatch[1]));
  }

  return '';
}

function inferSizeFromRatioOrOrientation(args: Record<string, any>): string {
  const textParts = ['aspect_ratio', 'orientation', 'size', 'prompt']
    .map(key => typeof args[key] === 'string' ? args[key] : '')
    .filter(Boolean);
  const text = textParts.join(' ').toLowerCase();

  if (typeof args.aspect_ratio === 'string') {
    const ratio = args.aspect_ratio.trim().toLowerCase().replace(/\s+/g, '').replace(/\//g, ':');
    if (ASPECT_RATIO_TO_SIZE[ratio]) return ASPECT_RATIO_TO_SIZE[ratio];
  }

  for (const [ratio, size] of Object.entries(ASPECT_RATIO_TO_SIZE)) {
    const [left, right] = ratio.split(':');
    const pattern = new RegExp(`(?<!\\d)${left}\\s*[:/]\\s*${right}(?!\\d)`);
    if (pattern.test(text)) return size;
  }

  if (typeof args.orientation === 'string') {
    const orientation = args.orientation.trim().toLowerCase();
    if (ORIENTATION_TO_SIZE[orientation]) return ORIENTATION_TO_SIZE[orientation];
  }

  if (/\b(square)\b|(?<!\d)1\s*[:/]\s*1(?!\d)/.test(text)) return '512x512';
  if (/\b(portrait|vertical|tall)\b/.test(text)) return '512x768';
  if (/\b(landscape|wide|widescreen|horizontal|banner)\b/.test(text)) return '768x512';

  return '';
}

function formatImageSize(width: number, height: number): string {
  if (isValidImageDimension(width) && isValidImageDimension(height)) {
    return `${width}x${height}`;
  }
  return '';
}

function isValidImageDimension(value: number): boolean {
  return Number.isInteger(value) && value >= 64 && value <= MAX_IMAGE_DIMENSION;
}

function coerceInteger(value: any): number | null {
  if (typeof value === 'number' && Number.isInteger(value)) return value;
  if (typeof value === 'string' && /^\d+$/.test(value.trim())) return Number(value.trim());
  return null;
}

function copyOptionalImageArgs(args: Record<string, any>, body: Record<string, any>): void {
  const steps = coerceInteger(args.steps);
  if (steps !== null && steps > 0) body.steps = steps;

  const cfgScale = Number(args.cfg_scale);
  if (Number.isFinite(cfgScale) && cfgScale > 0) body.cfg_scale = cfgScale;

  const seed = coerceInteger(args.seed);
  if (seed !== null) body.seed = seed;

  if (typeof args.sample_method === 'string' && args.sample_method.trim()) {
    body.sample_method = args.sample_method.trim();
  }

  const flowShift = Number(args.flow_shift);
  if (Number.isFinite(flowShift) && flowShift > 0) body.flow_shift = flowShift;
}

function appendOptionalImageFormArgs(args: Record<string, any>, formData: FormData): void {
  const body: Record<string, any> = {};
  copyOptionalImageArgs(args, body);
  for (const [key, value] of Object.entries(body)) {
    formData.append(key, String(value));
  }
}

async function executeTTSTool(
  args: Record<string, any>,
  model: string,
  signal?: AbortSignal,
): Promise<ToolExecutionResult> {
  // Request MP3 — it's widely playable in <audio> and is what the server
  // defaults to anyway. We collect the full body on the client; true
  // incremental playback would need MediaSource integration (stream_format:
  // "audio" returns raw PCM which <audio> can't decode).
  const body = {
    model,
    input: args.input || '',
    voice: args.voice || 'af_heart',
    response_format: 'mp3',
  };

  const response = await serverFetch('/audio/speech', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  });

  if (!response.ok) {
    const errData = await response.json().catch(() => ({}));
    throw new Error(errData.error?.message || 'TTS failed');
  }

  const arrayBuffer = await response.arrayBuffer();
  const uint8 = new Uint8Array(arrayBuffer);
  let binary = '';
  for (let i = 0; i < uint8.length; i++) {
    binary += String.fromCharCode(uint8[i]);
  }
  const b64 = btoa(binary);

  return { type: 'audio', data: b64, mime: 'audio/mpeg' };
}

async function executeTranscriptionTool(
  args: Record<string, any>,
  model: string,
  context: ToolExecutionContext,
  signal?: AbortSignal,
): Promise<ToolExecutionResult> {
  if (context.extractedAudio.length === 0) {
    return { type: 'text', text: 'No audio data provided for transcription.' };
  }

  const audio = context.extractedAudio[0];
  const binaryStr = atob(audio.data);
  const bytes = new Uint8Array(binaryStr.length);
  for (let i = 0; i < binaryStr.length; i++) {
    bytes[i] = binaryStr.charCodeAt(i);
  }

  let ext = '.wav';
  if (audio.mime.includes('mp3') || audio.mime.includes('mpeg')) ext = '.mp3';
  else if (audio.mime.includes('m4a') || audio.mime.includes('mp4')) ext = '.m4a';
  else if (audio.mime.includes('ogg')) ext = '.ogg';
  else if (audio.mime.includes('flac')) ext = '.flac';
  else if (audio.mime.includes('webm')) ext = '.webm';

  const formData = new FormData();
  formData.append('file', new Blob([bytes], { type: audio.mime }), `audio${ext}`);
  formData.append('model', model);
  if (args.language) formData.append('language', args.language);

  const response = await serverFetch('/audio/transcriptions', {
    method: 'POST',
    body: formData,
    signal,
  });

  const data = await response.json();
  if (data.text !== undefined) {
    return { type: 'text', text: `Transcription: ${data.text}` };
  }
  throw new Error(data.error?.message || 'Transcription failed');
}

async function executeVisionTool(
  args: Record<string, any>,
  model: string,
  context: ToolExecutionContext,
  signal?: AbortSignal,
): Promise<ToolExecutionResult> {
  const question = args.question || 'Describe this image.';

  // Only accept an LLM-provided image_url if it's a base64 data URL. Reject
  // arbitrary http:/file:/javascript: URIs — if the LLM hallucinates one,
  // the backend's handling is out of our control, and the rendered
  // MessageContent already enforces data:image/ for display. Fall back to
  // the user's uploaded image (same-origin data URL) in all other cases.
  const rawImageUrl = typeof args.image_url === 'string' ? args.image_url : '';
  let imageUrl = rawImageUrl.startsWith('data:image/') ? rawImageUrl : '';

  if (!imageUrl && context.extractedImages.length > 0) {
    imageUrl = context.extractedImages[context.extractedImages.length - 1].dataUrl;
  }

  if (!imageUrl) {
    return { type: 'text', text: 'No image available to analyze.' };
  }

  const body = {
    model,
    messages: [{
      role: 'user',
      content: [
        { type: 'image_url', image_url: { url: imageUrl } },
        { type: 'text', text: question },
      ],
    }],
    stream: false,
  };

  const response = await serverFetch('/chat/completions', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  });

  const data = await response.json();
  if (data.choices?.[0]?.message?.content) {
    return { type: 'text', text: data.choices[0].message.content };
  }
  throw new Error(data.error?.message || 'Vision analysis failed');
}
