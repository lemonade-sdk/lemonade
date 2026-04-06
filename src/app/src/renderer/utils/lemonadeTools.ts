import { serverFetch } from './serverConfig';
import { ModelsData } from './modelData';
import { getExperienceComponents } from './experienceModels';
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

const NON_LLM_LABELS = new Set(['image', 'speech', 'tts', 'audio', 'transcription', 'embeddings', 'embedding', 'reranking']);

/**
 * Build tools, system prompt, and model map from an experience model's components.
 * Tool definitions are loaded from toolDefinitions.json — the single source of truth.
 */
export function buildLemonadeTools(
  experienceName: string,
  modelsData: ModelsData,
): LemonadeToolsResult {
  const info = modelsData[experienceName];
  const components = getExperienceComponents(info);

  const llmModel = components.find(c => {
    const labels = modelsData[c]?.labels ?? [];
    return !labels.some(l => NON_LLM_LABELS.has(l));
  }) || components[0] || '';

  const tools: LemonadeToolDef[] = [];
  const models: Record<string, string> = {};

  for (const def of toolDefinitions.tools) {
    const requiresLabels = (def as any).requires_labels as string[] | undefined;
    const requiresLlmLabels = (def as any).requires_llm_labels as string[] | undefined;

    if (requiresLabels) {
      const labelSet = new Set(requiresLabels);
      const match = components.find(c => {
        const labels = modelsData[c]?.labels ?? [];
        return labels.some(l => labelSet.has(l));
      });
      if (!match) continue;
      tools.push({ type: 'function', function: def.function });
      models[def.function.name] = match;
      continue;
    }

    if (requiresLlmLabels) {
      const labelSet = new Set(requiresLlmLabels);
      const llmLabels = modelsData[llmModel]?.labels ?? [];
      if (!llmLabels.some(l => labelSet.has(l))) continue;
      tools.push({ type: 'function', function: def.function });
      models[def.function.name] = llmModel;
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
): Promise<ToolExecutionResult> {
  const funcName = toolCall.function.name;
  let args: Record<string, any>;
  try {
    args = JSON.parse(toolCall.function.arguments);
  } catch {
    args = {};
  }

  const hasPreviousImage = context.previousArtifacts.some(a => a.type === 'image');
  const modelLabels = modelsData?.[model]?.labels ?? [];
  const modelSupportsEdit = modelLabels.includes('edit');
  const effectiveName = (funcName === 'generate_image' && hasPreviousImage && modelSupportsEdit) ? 'edit_image' : funcName;

  if (effectiveName === 'generate_image' || effectiveName === 'edit_image') {
    return executeImageTool(effectiveName, args, model, context);
  }
  if (effectiveName === 'text_to_speech') {
    return executeTTSTool(args, model);
  }
  if (effectiveName === 'transcribe_audio') {
    return executeTranscriptionTool(args, model, context);
  }
  if (effectiveName === 'analyze_image') {
    return executeVisionTool(args, model, context);
  }

  return { type: 'text', text: `Unknown tool: ${funcName}` };
}

async function executeImageTool(
  effectiveName: string,
  args: Record<string, any>,
  model: string,
  context: ToolExecutionContext,
): Promise<ToolExecutionResult> {
  const isEdit = effectiveName === 'edit_image';

  if (isEdit) {
    // /images/edits requires multipart/form-data
    const formData = new FormData();
    formData.append('model', model);
    formData.append('prompt', args.prompt || '');
    formData.append('response_format', 'b64_json');
    formData.append('n', '1');

    if (args.size) {
      formData.append('size', args.size);
    }

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
  };

  if (args.size) {
    const [w, h] = args.size.split('x').map(Number);
    if (w && h) {
      body.width = w;
      body.height = h;
    }
  }

  const response = await serverFetch('/images/generations', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
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
): Promise<ToolExecutionResult> {
  const body = {
    model,
    input: args.input || '',
    voice: args.voice || 'af_heart',
  };

  const response = await serverFetch('/audio/speech', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
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

  return { type: 'audio', data: b64, mime: 'audio/wav' };
}

async function executeTranscriptionTool(
  args: Record<string, any>,
  model: string,
  context: ToolExecutionContext,
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
): Promise<ToolExecutionResult> {
  let imageUrl = args.image_url || '';
  const question = args.question || 'Describe this image.';

  if ((!imageUrl || !imageUrl.startsWith('data:')) && context.extractedImages.length > 0) {
    imageUrl = context.extractedImages[context.extractedImages.length - 1].dataUrl;
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
  });

  const data = await response.json();
  if (data.choices?.[0]?.message?.content) {
    return { type: 'text', text: data.choices[0].message.content };
  }
  throw new Error(data.error?.message || 'Vision analysis failed');
}
