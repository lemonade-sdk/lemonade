import { serverFetch } from './serverConfig';
import { ModelsData } from './modelData';
import { hasAnyModelLabel, isChatPlannerCandidate } from './modelLabels';
import { getCollectionComponents } from './collectionModels';
import { COLLECTION_IMAGE_SIZE } from './collectionImageConfig';
import toolDefinitions from './toolDefinitions.json';

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
  const customWorkflowComponents = info?.workflow_source === 'custom'
    ? info.workflow_components
    : undefined;

  const tools: LemonadeToolDef[] = [];
  const models: Record<string, string> = {};

  const substituteParams = (params: Record<string, any>): Record<string, any> => {
    const props = params?.properties as Record<string, any> | undefined;
    if (!props) return params;
    const newProps: Record<string, any> = {};
    for (const [key, prop] of Object.entries(props)) {
      newProps[key] = typeof prop?.description === 'string' && prop.description.includes('{image_size}')
        ? { ...prop, description: prop.description.replaceAll('{image_size}', COLLECTION_IMAGE_SIZE) }
        : prop;
    }
    return { ...params, properties: newProps };
  };

  const materialize = (def: ToolDefinitionEntry): LemonadeToolDef => ({
    type: 'function',
    function: { ...def.function, parameters: substituteParams(def.function.parameters) },
  });

  const componentHasAnyLabel = (component: string, requiredLabels: string[]): boolean => {
    return hasAnyModelLabel(modelsData[component], requiredLabels);
  };

  const findComponentWithAnyLabel = (requiredLabels: string[], llmOnly = false): string | undefined => {
    return components.find(c => {
      if (!componentHasAnyLabel(c, requiredLabels)) return false;
      if (!llmOnly) return true;
      return isChatPlannerCandidate(modelsData[c]);
    });
  };

  const getCustomWorkflowToolModel = (def: ToolDefinitionEntry): string | undefined => {
    if (!customWorkflowComponents) return undefined;

    const explicitToolModels: Record<string, string | undefined> = {
      generate_image: customWorkflowComponents.image,
      edit_image: customWorkflowComponents.edit ?? customWorkflowComponents.image,
      text_to_speech: customWorkflowComponents.speech,
      transcribe_audio: customWorkflowComponents.transcription,
      analyze_image: customWorkflowComponents.vision ?? customWorkflowComponents.llm,
    };

    const candidate = explicitToolModels[def.function.name];
    if (!candidate || !components.includes(candidate)) return undefined;

    const requiredLabels = def.requires_labels ?? def.requires_llm_labels;
    if (requiredLabels && !componentHasAnyLabel(candidate, requiredLabels)) return undefined;

    return candidate;
  };

  for (const def of (toolDefinitions.tools as ToolDefinitionEntry[])) {
    if (customWorkflowComponents) {
      const match = getCustomWorkflowToolModel(def);
      if (!match) continue;
      tools.push(materialize(def));
      models[def.function.name] = match;
      continue;
    }

    const requiresLabels = def.requires_labels;
    const requiresLlmLabels = def.requires_llm_labels;

    if (requiresLabels) {
      const match = findComponentWithAnyLabel(requiresLabels);
      if (!match) continue;
      tools.push(materialize(def));
      models[def.function.name] = match;
      continue;
    }

    if (requiresLlmLabels) {
      const match = findComponentWithAnyLabel(requiresLlmLabels, true);
      if (!match) continue;
      tools.push(materialize(def));
      models[def.function.name] = match;
    }
  }

  const toolList = tools.map(t => `- ${t.function.name}: ${t.function.description}`).join('\n');
  const systemPrompt = toolDefinitions.system_prompt.replace('{tool_list}', toolList);

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
  const isEdit = effectiveName === 'edit_image';

  if (isEdit) {
    // /images/edits requires multipart/form-data
    const formData = new FormData();
    formData.append('model', model);
    formData.append('prompt', args.prompt || '');
    formData.append('response_format', 'b64_json');
    formData.append('n', '1');
    formData.append('size', COLLECTION_IMAGE_SIZE);

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
    size: COLLECTION_IMAGE_SIZE,
  };

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
