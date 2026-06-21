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

const OMNI_IMAGE_SIZE = '1024x1024';

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

  if (imageModel) {
    tools.push(makeTool(
      'generate_image',
      'Generate a new image from scratch based on a text description. Use this only when the user asks to create an entirely new image.',
      {
        type: 'object',
        properties: {
          prompt: { type: 'string', description: `Detailed image prompt. Output size is fixed at ${OMNI_IMAGE_SIZE}.` },
        },
        required: ['prompt'],
      },
    ));
    models.generate_image = imageModel;
  }

  if (editModel) {
    tools.push(makeTool(
      'edit_image',
      'Edit or modify the most recent image from this conversation. Use this when the user wants to add, remove, change, modify, fix, or adjust an existing image.',
      {
        type: 'object',
        properties: {
          prompt: { type: 'string', description: `Description of the requested edit. Output size is fixed at ${OMNI_IMAGE_SIZE}.` },
        },
        required: ['prompt'],
      },
    ));
    models.edit_image = editModel;
  }

  if (ttsModel) {
    tools.push(makeTool(
      'text_to_speech',
      'Convert text to spoken audio. Use this when the user asks you to speak, say, read aloud, narrate, or convert text to speech.',
      {
        type: 'object',
        properties: {
          input: { type: 'string', description: 'The text to convert to speech.' },
          voice: { type: 'string', description: 'Voice to use for speech synthesis.', default: 'af_heart' },
        },
        required: ['input'],
      },
    ));
    models.text_to_speech = ttsModel;
  }

  if (transcriptionModel) {
    tools.push(makeTool(
      'transcribe_audio',
      'Transcribe audio to text. Use this when the user provides an audio file or when a message contains a [User provided audio file #N] placeholder.',
      {
        type: 'object',
        properties: {
          language: { type: 'string', description: 'Optional ISO 639-1 language code, e.g. en, de, fr.', default: 'en' },
        },
        required: [],
      },
    ));
    models.transcribe_audio = transcriptionModel;
  }

  if (visionModel) {
    tools.push(makeTool(
      'analyze_image',
      'Analyze, describe, read, or answer questions about an image provided by the user. Use this when a message contains a [User provided image #N] placeholder.',
      {
        type: 'object',
        properties: {
          question: { type: 'string', description: "Question to answer about the image, or 'describe' for a general description." },
        },
        required: ['question'],
      },
    ));
    models.analyze_image = visionModel;
  }

  if (tools.length === 0) return null;

  const toolList = tools.map(tool => `- ${tool.function.name}: ${tool.function.description}`).join('\n');
  const systemPrompt = [
    'You are a helpful multimodal AI assistant and the planner LLM for a Lemonade Omni model.',
    'You can chat normally, and you can call specialized Omni tools when the user asks for media work.',
    '',
    'Available Omni tools:',
    toolList,
    '',
    'Use generate_image only for creating a brand-new image. If an image already exists and the user asks to add, remove, change, modify, fix, or adjust it, use edit_image when available.',
    'Use text_to_speech when the user asks you to speak, read aloud, narrate, or create audio.',
    "When the user message contains '[User provided image #N]', call analyze_image before answering image-specific questions.",
    "When the user message contains '[User provided audio file #N]', call transcribe_audio before answering audio-specific questions.",
    'After a tool succeeds, give a brief friendly response. Do not include base64 data in your message; the UI renders media artifacts automatically.',
    `Planner model: ${plannerModel || String((collectionModel as any).name || collectionModel.id || 'planner')}`,
  ].join('\n');

  const execute = async (call: ToolCall): Promise<ToolExecutionPayload> => {
    const name = call.function.name as keyof OmniComponentMap;
    let args: Record<string, unknown> = {};
    try { args = JSON.parse(call.function.arguments || '{}'); } catch { /* keep empty args */ }
    const requestedModel = models[name];
    const sourceImage = latestImage(context);
    const shouldAutoEdit = name === 'generate_image' && !!sourceImage && !!models.edit_image && looksLikeImageEdit(args.prompt);
    const effectiveName: keyof OmniComponentMap = shouldAutoEdit ? 'edit_image' : name;
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
        const urls = await api.imageGeneration(model, prompt, { n: 1, size: OMNI_IMAGE_SIZE });
        return mediaResult(call.id, 'Image generated successfully.', urls.map(url => artifactFromDataUrl(url, 'image')));
      }

      if (effectiveName === 'edit_image') {
        const prompt = String(args.prompt || '').trim();
        if (!prompt) throw new Error('edit_image requires a prompt.');
        if (!sourceImage) throw new Error('No previous image is available to edit.');
        const urls = await api.imageEdit(model, prompt, sourceImage, { n: 1, size: OMNI_IMAGE_SIZE });
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
        const text = await api.audioTranscription(model, file);
        return textResult(call.id, `Transcription: ${text}`, `Transcribed ${file.name}`);
      }

      if (effectiveName === 'analyze_image') {
        const imageUrl = sourceImage;
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
