import { serverFetch } from './serverConfig';
import { ModelsData } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { getCollectionComponents } from './collectionModels';
import { COLLECTION_IMAGE_SIZE } from './collectionImageConfig';
import toolDefinitions from './toolDefinitions.json';

// Fallback used for actual image API requests when the planner does not pass
// a size. Keep it shared with collection image hints so defaults stay in sync.
const DEFAULT_IMAGE_SIZE = COLLECTION_IMAGE_SIZE;
const MAX_IMAGE_DIMENSION = 2048;

type ImageSizePreset = {
  size: string;
  ratios?: string[];
  hints?: string[];
};

// Keep user-facing aliases in one compact table. The planner schema stays
// token-light; these synonyms are executor fallbacks for natural-language
// prompts such as "vertical", "banner", or "16:9".
const IMAGE_SIZE_PRESETS: ImageSizePreset[] = [
  { size: DEFAULT_IMAGE_SIZE, ratios: ['2:1'], hints: ['landscape', 'wide', 'widescreen', 'horizontal', 'banner'] },
  { size: '512x512', ratios: ['1:1'], hints: ['square'] },
  { size: '1024x576', ratios: ['16:9'] },
  { size: '576x1024', ratios: ['9:16'] },
  { size: '768x576', ratios: ['4:3'] },
  { size: '576x768', ratios: ['3:4'] },
  { size: '768x512', ratios: ['3:2'] },
  { size: '512x768', ratios: ['2:3'], hints: ['portrait', 'vertical', 'tall'] },
];

const ASPECT_RATIO_TO_SIZE: Record<string, string> = Object.fromEntries(
  IMAGE_SIZE_PRESETS.flatMap(preset => (preset.ratios ?? []).map(ratio => [ratio, preset.size])),
);

const SIZE_HINT_TO_SIZE: Record<string, string> = Object.fromEntries(
  IMAGE_SIZE_PRESETS.flatMap(preset => (preset.hints ?? []).map(hint => [hint, preset.size])),
);

const IMAGE_EDIT_INSTRUCTIONS =
  '\nIMPORTANT: When an image has already been generated in this conversation and the user wants to add, remove, change, modify, or adjust it, use edit_image rather than generate_image. The edit_image tool automatically uses the most recent image as its source.';

const IMAGE_SIZE_INSTRUCTIONS =
  `\nWhen generating or editing images, pass size or width+height only when the user provides exact dimensions. For aspect-ratio or orientation requests, either pass an obvious concrete size or keep the hint in the prompt; otherwise omit size arguments and let the executor use its ${DEFAULT_IMAGE_SIZE} default. Preserve explicit image options such as steps, cfg_scale, seed, sample_method, and flow_shift as tool arguments.`;

const VISION_INSTRUCTIONS =
  "\nWhen the user sends an image (as an image_url in their message), use analyze_image to look at the image before responding about it.";

const TRANSCRIPTION_INSTRUCTIONS =
  "\nWhen you see '[User provided audio file #N]' in a message, it means the user sent audio data. Call transcribe_audio to transcribe it — the audio data is handled automatically by the system.";

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

  const enabledToolNames = new Set(tools.map(t => t.function.name));
  const toolInstructions = [
    enabledToolNames.has('edit_image') ? IMAGE_EDIT_INSTRUCTIONS : '',
    (enabledToolNames.has('generate_image') || enabledToolNames.has('edit_image')) ? IMAGE_SIZE_INSTRUCTIONS : '',
    enabledToolNames.has('analyze_image') ? VISION_INSTRUCTIONS : '',
    enabledToolNames.has('transcribe_audio') ? TRANSCRIPTION_INSTRUCTIONS : '',
  ].join('');

  const toolList = tools.map(t => `- ${t.function.name}: ${t.function.description}`).join('\n');
  const toolGuidance = guidance.length ? `\n${guidance.join('\n')}` : '';
  const systemPrompt = toolDefinitions.system_prompt
    .replace('{tool_list}', toolList)
    .replace('{tool_instructions}', toolInstructions);

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

  const modelLabels = modelsData?.[model]?.labels ?? [];
  const modelSupportsEdit = modelLabels.includes('edit');

  if (funcName === 'edit_image' && modelsData && !modelSupportsEdit) {
    return { type: 'text', text: `Image editing is not available for model: ${model}` };
  }

  if (funcName === 'generate_image' || funcName === 'edit_image') {
    return executeImageTool(funcName, args, model, context, signal);
  }
  if (funcName === 'text_to_speech') {
    return executeTTSTool(args, model, signal);
  }
  if (funcName === 'transcribe_audio') {
    return executeTranscriptionTool(args, model, context, signal);
  }
  if (funcName === 'analyze_image') {
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
    const lastImage = [...context.previousArtifacts].reverse().find(a => a.type === 'image');
    if (!lastImage) {
      throw new Error('Image edit requested, but no previous image is available as a source.');
    }

    // /images/edits requires multipart/form-data
    const formData = new FormData();
    formData.append('model', model);
    formData.append('prompt', args.prompt || '');
    formData.append('response_format', 'b64_json');
    formData.append('n', '1');
    formData.append('size', imageSize);
    appendOptionalImageFormArgs(args, formData);

    // Attach the most recent image as the source file
    const binaryStr = atob(lastImage.data);
    const bytes = new Uint8Array(binaryStr.length);
    for (let i = 0; i < binaryStr.length; i++) {
      bytes[i] = binaryStr.charCodeAt(i);
    }
    formData.append('image', new Blob([bytes], { type: lastImage.mime || 'image/png' }), 'image.png');

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
    if (SIZE_HINT_TO_SIZE[orientation]) return SIZE_HINT_TO_SIZE[orientation];
  }

  for (const [hint, size] of Object.entries(SIZE_HINT_TO_SIZE)) {
    const pattern = new RegExp(`\\b${hint}\\b`);
    if (pattern.test(text)) return size;
  }

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

function coerceNumber(value: any): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim()) {
    const parsed = Number(value.trim());
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
}

function copyOptionalImageArgs(args: Record<string, any>, body: Record<string, any>): void {
  const steps = coerceInteger(args.steps);
  if (steps !== null && steps > 0) body.steps = steps;

  const cfgScale = coerceNumber(args.cfg_scale);
  if (cfgScale !== null && cfgScale > 0) body.cfg_scale = cfgScale;

  const seed = coerceInteger(args.seed);
  if (seed !== null) body.seed = seed;

  if (typeof args.sample_method === 'string' && args.sample_method.trim()) {
    body.sample_method = args.sample_method.trim();
  }

  const flowShift = coerceNumber(args.flow_shift);
  if (flowShift !== null && flowShift > 0) body.flow_shift = flowShift;
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
