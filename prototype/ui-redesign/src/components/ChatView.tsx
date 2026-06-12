import React, { useState, useRef, useCallback, useEffect, useMemo } from 'react';
import api, { ChatMessage, ChatCompletionStats, LoadedModel, ModelInfo, RealtimeTranscriptionHandle, friendlyErrorMessage } from '../api';
import MarkdownMessage from './MarkdownMessage';
import LogViewer from './LogViewer';
import { Icon, CapabilityIcon } from './Icon';
import { useChatStreaming, ToolCallEntry, ChatToolRuntime, ToolArtifact } from '../hooks/useChatStreaming';
import { useAudioCapture } from '../hooks/useAudioCapture';
import {
  canSelectInComposer,
  canUseChatCompletions,
  capabilityBadge,
  capabilityFromLoaded,
  capabilityFromModelInfo,
  capabilityIcon,
  capabilityLabel,
  modelDisplayName,
  modelInitial,
  ModelCapability,
  ModelSnapshot,
  selectPreferredLoadedModel,
  snapshotFromLoaded,
  snapshotFromModelInfo,
  snapshotFromName,
} from '../modelCapabilities';
import { AccountSession, describeSession, scopedStorageKey } from '../features/accounts/accountStore';
import { customModelToModelInfo, loadCustomModels } from '../features/customModels/customModelStore';
import { findModelInfoByName, getAudioTranscriptionComponent, getPrimaryChatComponent, getVisionChatComponent, isCollectionModel } from '../features/collections/collectionModels';
import { LEMONADE_TOOLS, executeTool } from '../tools/lemonadeTools';
import { buildOmniToolRuntime } from '../tools/omniTools';
import { PRESET_STORE_EVENT, activePresetForModel, presetIcon } from '../presetStore';

interface Message {
  role: 'user' | 'assistant';
  content: string;
  images?: string[];  // transient base64 data URLs for user messages with images
  generatedImages?: string[]; // transient generated image data URLs
  audioUrl?: string; // transient object URL for TTS output
  audioName?: string;
  thinking?: string;
  stats?: ChatCompletionStats;
  toolCalls?: ToolCallEntry[];
  model?: ModelSnapshot | null;
  isError?: boolean;
}

interface Conversation {
  id: string;
  title: string;
  model: ModelSnapshot | null;
  messages: Message[];
  updatedAt: number;
  schemaVersion?: number;
}

const STORAGE_KEY = 'conversations';
const ACTIVE_KEY = 'active_conversation';
const PERSIST_KEY = 'persist_conversations';
const STORAGE_VERSION = 3;

function generateId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function scopedKey(scope: string, key: string): string {
  return scopedStorageKey(scope, key);
}

function loadPersistencePreference(scope: string): boolean {
  try { return localStorage.getItem(scopedKey(scope, PERSIST_KEY)) === 'true'; } catch { return false; }
}

function normalizeSnapshot(raw: unknown): ModelSnapshot | null {
  if (!raw) return null;
  if (typeof raw === 'string') {
    return { name: raw, type: 'unknown', capability: 'unknown' };
  }
  if (typeof raw !== 'object') return null;
  const obj = raw as Record<string, unknown>;
  const name = typeof obj.name === 'string' ? obj.name : (typeof obj.model_name === 'string' ? obj.model_name : '');
  if (!name) return null;
  const capability = typeof obj.capability === 'string' ? obj.capability as ModelCapability : 'unknown';
  return {
    name,
    type: typeof obj.type === 'string' ? obj.type : 'unknown',
    capability,
    recipe: typeof obj.recipe === 'string' ? obj.recipe : undefined,
    device: typeof obj.device === 'string' ? obj.device : undefined,
    checkpoint: typeof obj.checkpoint === 'string' ? obj.checkpoint : undefined,
  };
}

function normalizeConversation(raw: unknown): Conversation | null {
  if (!raw || typeof raw !== 'object') return null;
  const obj = raw as Record<string, unknown>;
  const id = typeof obj.id === 'string' ? obj.id : generateId();
  const messagesRaw = Array.isArray(obj.messages) ? obj.messages : [];
  const messages = messagesRaw
    .filter((m): m is Record<string, unknown> => !!m && typeof m === 'object')
    .map(m => ({
      role: m.role === 'assistant' ? 'assistant' as const : 'user' as const,
      content: typeof m.content === 'string' ? m.content : '',
      thinking: typeof m.thinking === 'string' ? m.thinking : undefined,
      stats: m.stats as ChatCompletionStats | undefined,
      toolCalls: Array.isArray(m.toolCalls) ? m.toolCalls as ToolCallEntry[] : undefined,
      model: normalizeSnapshot(m.model),
      isError: m.isError === true || (typeof m.content === 'string' && /^Error:/i.test(m.content)),
    }));
  return {
    id,
    title: typeof obj.title === 'string' && obj.title.trim() ? obj.title : deriveTitle(messages),
    model: normalizeSnapshot(obj.model),
    messages,
    updatedAt: typeof obj.updatedAt === 'number' ? obj.updatedAt : Date.now(),
    schemaVersion: STORAGE_VERSION,
  };
}

function loadConversations(persist: boolean, scope: string): Conversation[] {
  if (!persist) return [];
  try {
    const raw = localStorage.getItem(scopedKey(scope, STORAGE_KEY));
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    const list: unknown[] = Array.isArray(parsed) ? parsed : (Array.isArray(parsed?.conversations) ? parsed.conversations : []);
    return list.map(normalizeConversation).filter((c): c is Conversation => !!c);
  } catch { /* ignore */ }
  return [];
}

function saveConversations(convos: Conversation[], persist: boolean, scope: string) {
  if (!persist) {
    try {
      localStorage.removeItem(scopedKey(scope, STORAGE_KEY));
      localStorage.removeItem(scopedKey(scope, ACTIVE_KEY));
    } catch { /* ignore */ }
    return;
  }
  // Strip transient/generated media before persisting. Image prompts are redacted
  // so private context around an image does not leak into localStorage.
  const stripped = convos.map(c => ({
    ...c,
    schemaVersion: STORAGE_VERSION,
    messages: c.messages.map(m => ({
      ...m,
      content: m.images?.length ? '[image prompt not persisted]' : m.content,
      images: undefined,
      generatedImages: undefined,
      audioUrl: undefined,
      audioName: undefined,
    })),
  }));
  try { localStorage.setItem(scopedKey(scope, STORAGE_KEY), JSON.stringify({ version: STORAGE_VERSION, conversations: stripped })); } catch { /* ignore */ }
}

function loadActiveId(persist: boolean, scope: string): string | null {
  if (!persist) return null;
  try { return localStorage.getItem(scopedKey(scope, ACTIVE_KEY)); } catch { return null; }
}

function saveActiveId(id: string | null, persist: boolean, scope: string) {
  try {
    if (!persist) {
      localStorage.removeItem(scopedKey(scope, ACTIVE_KEY));
    } else if (id) {
      localStorage.setItem(scopedKey(scope, ACTIVE_KEY), id);
    } else {
      localStorage.removeItem(scopedKey(scope, ACTIVE_KEY));
    }
  } catch { /* ignore */ }
}

function titleFromInput(text: string, hasImages: boolean, audioFiles: File[] = []): string {
  const clean = text.trim();
  if (clean) return clean.slice(0, 50) + (clean.length > 50 ? '…' : '');
  if (audioFiles.length > 0) return `Audio: ${audioFiles[0].name}`.slice(0, 50);
  if (hasImages) return 'Image conversation';
  return 'New conversation';
}

function deriveTitle(messages: Message[]): string {
  const first = messages.find(m => m.role === 'user');
  if (!first) return 'New conversation';
  return titleFromInput(first.content, !!first.images?.length);
}

function isPersistableAssistantMessage(m: Message): boolean {
  return !(m.isError || /^Error:/i.test(m.content));
}

function timeAgo(ts: number): string {
  const diff = Date.now() - ts;
  if (diff < 60000) return 'just now';
  if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
  if (diff < 86400000) return `${Math.floor(diff / 3600000)}h ago`;
  return `${Math.floor(diff / 86400000)}d ago`;
}

interface ChatViewProps {
  currentModel: string | null;
  loadedModels: LoadedModel[];
  onModelSelect: (model: string) => void;
  onRefresh: () => void | Promise<void>;
  accountSession: AccountSession;
}

const TOOLS_KEY = 'use_tools';
const MAX_IMAGE_DIM = 1024;
const MAX_IMAGES = 4;
const IMAGE_SIZE_OPTIONS = [256, 512, 768, 1024, 1536, 2048] as const;

type ImageMode = 'generate' | 'edit';

interface ImageGenerationSettings {
  steps: number;
  cfgScale: number;
  width: number;
  height: number;
  seed: number | '';
  upscaleModel: string;
}

const DEFAULT_IMAGE_SETTINGS: ImageGenerationSettings = {
  steps: 20,
  cfgScale: 7,
  width: 512,
  height: 512,
  seed: -1,
  upscaleModel: '',
};

function numberFromUnknown(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim()) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }
  return null;
}

function intFromUnknown(value: unknown): number | null {
  const number = numberFromUnknown(value);
  return number === null ? null : Math.round(number);
}

function nestedRecord(source: Record<string, unknown> | undefined, key: string): Record<string, unknown> | undefined {
  const value = source?.[key];
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : undefined;
}

function numberAt(source: Record<string, unknown> | undefined, paths: string[][]): number | null {
  for (const path of paths) {
    let cursor: Record<string, unknown> | undefined = source;
    for (let i = 0; i < path.length - 1; i += 1) {
      cursor = nestedRecord(cursor, path[i]);
      if (!cursor) break;
    }
    if (!cursor) continue;
    const value = numberFromUnknown(cursor[path[path.length - 1]]);
    if (value !== null) return value;
  }
  return null;
}

function parseImageSize(value: unknown): Pick<ImageGenerationSettings, 'width' | 'height'> | null {
  if (typeof value !== 'string') return null;
  const match = /^(\d+)\s*x\s*(\d+)$/i.exec(value.trim());
  if (!match) return null;
  return { width: Number(match[1]), height: Number(match[2]) };
}

function nearestImageSize(value: number, fallback: number): number {
  if (!Number.isFinite(value)) return fallback;
  const exact = IMAGE_SIZE_OPTIONS.find(size => size === value);
  if (exact) return exact;
  return IMAGE_SIZE_OPTIONS.reduce((best, size) => (Math.abs(size - value) < Math.abs(best - value) ? size : best), fallback);
}

function partialImageSettingsFromSource(source?: Record<string, unknown> | null): Partial<ImageGenerationSettings> {
  if (!source) return {};
  const next: Partial<ImageGenerationSettings> = {};
  const steps = intFromUnknown(numberAt(source, [['steps'], ['sample_steps'], ['sample_params', 'sample_steps']]));
  if (steps !== null && steps > 0) next.steps = steps;
  const cfgScale = numberAt(source, [['cfg_scale'], ['txt_cfg'], ['guidance'], ['sample_params', 'guidance', 'txt_cfg']]);
  if (cfgScale !== null && cfgScale > 0) next.cfgScale = cfgScale;
  const parsedSize = parseImageSize(source.size);
  const width = parsedSize?.width ?? intFromUnknown(numberAt(source, [['width'], ['image_width']]));
  const height = parsedSize?.height ?? intFromUnknown(numberAt(source, [['height'], ['image_height']]));
  if (width !== null && width !== undefined && width > 0) next.width = nearestImageSize(width, DEFAULT_IMAGE_SETTINGS.width);
  if (height !== null && height !== undefined && height > 0) next.height = nearestImageSize(height, DEFAULT_IMAGE_SETTINGS.height);
  const seed = intFromUnknown(source.seed);
  if (seed !== null) next.seed = Math.max(seed, -1);
  return next;
}

function imageDefaultsForModel(loadedModel: LoadedModel | null, modelInfo: ModelInfo | null): ImageGenerationSettings {
  const modelImageDefaults = partialImageSettingsFromSource(modelInfo?.image_defaults as Record<string, unknown> | undefined);
  const modelRecipeOptions = partialImageSettingsFromSource(modelInfo?.recipe_options as Record<string, unknown> | undefined);
  const loadedRecipeOptions = partialImageSettingsFromSource(loadedModel?.recipe_options);
  return {
    ...DEFAULT_IMAGE_SETTINGS,
    ...modelImageDefaults,
    ...modelRecipeOptions,
    ...loadedRecipeOptions,
  };
}

function modelSupportsImageEdit(modelName: string | null, modelInfo: ModelInfo | null, loadedModel: LoadedModel | null): boolean {
  const labels = (modelInfo?.labels || []).map(label => label.toLowerCase().trim());
  if (labels.some(label => ['edit', 'image-edit', 'image-editing', 'image-to-image', 'img2img'].includes(label))) return true;

  const haystack = [
    modelName,
    modelInfo?.id,
    modelInfo?.name,
    modelInfo?.display_name,
    String((modelInfo as any)?.model_name || ''),
    loadedModel?.checkpoint,
  ].filter(Boolean).join(' ').toLowerCase();

  return haystack.includes('flux-2-klein')
    || haystack.includes('flux_2_klein')
    || haystack.includes('flux.2.klein')
    || haystack.includes('flux2-klein')
    || haystack.includes('qwen-edit')
    || haystack.includes('image-edit');
}

function modelSupportsRealtimeAudio(modelName: string | null, modelInfo: ModelInfo | null, loadedModel: LoadedModel | null): boolean {
  const labels = (modelInfo?.labels || []).map(label => label.toLowerCase().trim());
  if (labels.some(label => ['realtime-transcription', 'realtime', 'audio-input', 'audio-chat', 'chat-transcription'].includes(label))) return true;

  const recipe = String((modelInfo as any)?.recipe || loadedModel?.recipe || '').toLowerCase();
  if (recipe.includes('moonshine') || recipe.includes('whispercpp')) return true;

  const haystack = [
    modelName,
    modelInfo?.id,
    modelInfo?.name,
    modelInfo?.display_name,
    String((modelInfo as any)?.model_name || ''),
    loadedModel?.model_name,
    loadedModel?.checkpoint,
  ].filter(Boolean).join(' ').toLowerCase();
  return haystack.includes('moonshine') || haystack.includes('whisper') || haystack.includes('realtime') || haystack.includes('audio-chat');
}

function canUseMicrophone(): boolean {
  return typeof window !== 'undefined'
    && window.isSecureContext
    && typeof navigator !== 'undefined'
    && !!navigator.mediaDevices?.getUserMedia;
}

const LEMONADE_TOOL_RUNTIME: ChatToolRuntime = {
  tools: LEMONADE_TOOLS as unknown as Record<string, unknown>[],
  execute: executeTool as unknown as ChatToolRuntime['execute'],
};

function composeToolRuntimes(runtimes: Array<ChatToolRuntime | null | undefined>): ChatToolRuntime | null {
  const active = runtimes.filter((runtime): runtime is ChatToolRuntime => !!runtime && runtime.tools.length > 0);
  if (active.length === 0) return null;
  if (active.length === 1) return active[0];

  const tools: Record<string, unknown>[] = [];
  const byName = new Map<string, ChatToolRuntime>();
  const prompts: string[] = [];

  for (const runtime of active) {
    if (runtime.systemPrompt) prompts.push(runtime.systemPrompt);
    for (const tool of runtime.tools) {
      const name = String((tool as any).function?.name || '');
      if (!name || byName.has(name)) continue;
      tools.push(tool);
      byName.set(name, runtime);
    }
  }

  return {
    tools,
    systemPrompt: prompts.join('\n\n'),
    execute: async call => {
      const runtime = byName.get(call.function.name);
      if (!runtime) {
        return {
          tool_call_id: call.id,
          role: 'tool',
          content: JSON.stringify({ error: `Unknown tool: ${call.function.name}` }),
          error: true,
          displayResult: `Error: unknown tool ${call.function.name}`,
        };
      }
      return runtime.execute(call);
    },
  };
}

function collectToolArtifacts(toolCalls?: ToolCallEntry[]): ToolArtifact[] {
  return (toolCalls || []).flatMap(call => call.artifacts || []);
}

function summarizeToolOnlyResponse(toolCalls?: ToolCallEntry[]): string {
  const finished = (toolCalls || []).filter(call => call.status === 'done' || call.status === 'error');
  if (finished.length === 0) return '';
  const names = finished
    .map(call => TOOL_LABELS[call.name] || call.name)
    .filter(Boolean)
    .slice(0, 3);
  const suffix = finished.length > names.length ? ` and ${finished.length - names.length} more` : '';
  return `Completed ${finished.length} tool call${finished.length === 1 ? '' : 's'}: ${names.join(', ')}${suffix}.`;
}

function collectConversationImages(messages: Message[]): string[] {
  const images: string[] = [];
  for (const message of messages) {
    if (message.images?.length) images.push(...message.images);
    if (message.generatedImages?.length) images.push(...message.generatedImages);
  }
  return images;
}

/** Resize and compress an image file to base64 data URL */
async function imageToBase64(file: File | Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      const img = new Image();
      img.onload = () => {
        // Downscale if needed
        let { width, height } = img;
        if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
          const scale = MAX_IMAGE_DIM / Math.max(width, height);
          width = Math.round(width * scale);
          height = Math.round(height * scale);
        }
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d')!;
        ctx.drawImage(img, 0, 0, width, height);
        resolve(canvas.toDataURL('image/jpeg', 0.85));
      };
      img.onerror = reject;
      img.src = reader.result as string;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

async function audioToInputAudio(file: File): Promise<{ type: 'input_audio'; input_audio: { data: string; format: string } }> {
  const dataUrl = await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ''));
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
  const comma = dataUrl.indexOf(',');
  const payload = comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl;
  const lowerName = file.name.toLowerCase();
  const mime = file.type.toLowerCase();
  const format = mime.includes('mpeg') || lowerName.endsWith('.mp3') ? 'mp3'
    : mime.includes('wav') || lowerName.endsWith('.wav') ? 'wav'
      : mime.includes('webm') || lowerName.endsWith('.webm') ? 'webm'
        : mime.includes('ogg') || lowerName.endsWith('.ogg') ? 'ogg'
          : 'wav';
  return { type: 'input_audio', input_audio: { data: payload, format } };
}


async function copyTextToClipboard(text: string): Promise<void> {
  if (!text) return;
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.setAttribute('readonly', 'true');
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  textarea.style.pointerEvents = 'none';
  document.body.appendChild(textarea);
  textarea.select();
  document.execCommand('copy');
  textarea.remove();
}

const CopyInlineButton: React.FC<{ text: string; title?: string; className?: string }> = ({ text, title = 'Copy', className = '' }) => {
  const [copied, setCopied] = useState(false);
  const disabled = !text;
  const handleClick = async (event: React.MouseEvent<HTMLButtonElement>) => {
    event.preventDefault();
    event.stopPropagation();
    if (disabled) return;
    try {
      await copyTextToClipboard(text);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1200);
    } catch {
      setCopied(false);
    }
  };
  return (
    <button
      type="button"
      className={`copy-inline${copied ? ' copy-inline--copied' : ''}${className ? ` ${className}` : ''}`}
      onClick={handleClick}
      disabled={disabled}
      title={copied ? 'Copied' : title}
      aria-label={copied ? 'Copied' : title}
    >
      {copied ? <Icon name="check" size={13} /> : <Icon name="copy" size={13} />}
    </button>
  );
};

function friendlyChatError(message: string): string {
  const cleaned = message.replace(/^Error:\s*/i, '').trim();
  if (!cleaned) return "I couldn't complete that request. Please check the server logs for details.";
  return `I couldn't complete that request.\n\n${cleaned}`;
}

const ChatView: React.FC<ChatViewProps> = ({ currentModel, loadedModels, onModelSelect, onRefresh, accountSession }) => {
  const storageScope = accountSession.storageScope;
  const [persistHistory, setPersistHistory] = useState(() => loadPersistencePreference(storageScope));
  const [conversations, setConversations] = useState<Conversation[]>(() => loadConversations(loadPersistencePreference(storageScope), storageScope));
  const [activeId, setActiveId] = useState<string | null>(() => loadActiveId(loadPersistencePreference(storageScope), storageScope));
  const [inputValue, setInputValue] = useState('');
  const [imageMode, setImageMode] = useState<ImageMode>('generate');
  const [imageSettings, setImageSettings] = useState<ImageGenerationSettings>(DEFAULT_IMAGE_SETTINGS);
  const imageSettingsModelRef = useRef<string | null>(null);
  const imageSettingsTouchedRef = useRef(false);
  const imageSettingsCommittedRef = useRef(false);
  const [pendingImages, setPendingImages] = useState<string[]>([]);
  const [pendingAudioFiles, setPendingAudioFiles] = useState<File[]>([]);
  const [isLiveRecording, setIsLiveRecording] = useState(false);
  const [isLiveConnected, setIsLiveConnected] = useState(false);
  const [isSpeaking, setIsSpeaking] = useState(false);
  const [liveTranscript, setLiveTranscript] = useState('');
  const [liveError, setLiveError] = useState<string | null>(null);
  const [audioLevel, setAudioLevel] = useState(0);
  const [capabilityBusy, setCapabilityBusy] = useState(false);
  const [presetVersion, setPresetVersion] = useState(0);
  const [railExpanded, setRailExpanded] = useState(true);
  const [useTools, setUseTools] = useState(() => {
    try { return localStorage.getItem(scopedKey(storageScope, TOOLS_KEY)) === 'true'; } catch { return false; }
  });
  const [showInlineLogs, setShowInlineLogs] = useState(false);
  const [modelPickerOpen, setModelPickerOpen] = useState(false);
  const [modelPickerQuery, setModelPickerQuery] = useState('');
  const [modelPickerLoading, setModelPickerLoading] = useState<string | null>(null);
  const [modelPickerError, setModelPickerError] = useState<string | null>(null);
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const modelPickerRef = useRef<HTMLDivElement>(null);
  const thinkingContentRef = useRef<HTMLDivElement>(null);
  const thinkingSticky = useRef(true);
  const scrollRafRef = useRef<number>(0);
  const streamModelsRef = useRef<Record<string, ModelSnapshot | null>>({});
  const realtimeRef = useRef<RealtimeTranscriptionHandle | null>(null);
  const isLiveRecordingRef = useRef(false);
  const liveTranscriptRef = useRef('');
  const liveFinalizeTimerRef = useRef<number | null>(null);
  const audioLevelRef = useRef(0);

  const currentLoadedModel = useMemo(
    () => loadedModels.find(m => m.model_name === currentModel) || null,
    [loadedModels, currentModel],
  );
  const customModelInfos = useMemo(
    () => loadCustomModels(storageScope).map(customModelToModelInfo),
    [storageScope],
  );
  const knownModelInfos = useMemo(
    () => {
      const seen = new Set<string>();
      const infos: ModelInfo[] = [];
      [...customModelInfos, ...api.allModels].forEach(info => {
        const name = String((info as any).model_name || info.name || info.id || '').toLowerCase();
        if (!name || seen.has(name)) return;
        seen.add(name);
        infos.push(info);
      });
      return infos;
    },
    [customModelInfos, loadedModels],
  );
  const currentCustomModelInfo = useMemo(
    () => findModelInfoByName(customModelInfos, currentModel) || null,
    [customModelInfos, currentModel],
  );
  const currentKnownModelInfo = useMemo(
    () => findModelInfoByName(knownModelInfos, currentModel) || null,
    [knownModelInfos, currentModel],
  );
  const currentModelSnapshot = useMemo(() => {
    const loadedSnapshot = snapshotFromLoaded(currentLoadedModel);
    if (currentCustomModelInfo) {
      const customCapability = capabilityFromModelInfo(currentCustomModelInfo);
      const customSnapshot = {
        name: currentModel || currentCustomModelInfo.name || currentCustomModelInfo.id,
        type: String((currentCustomModelInfo as any).type || customCapability || 'unknown'),
        capability: customCapability,
        recipe: String((currentCustomModelInfo as any).recipe || ''),
        checkpoint: String((currentCustomModelInfo as any).checkpoint || ''),
        device: currentLoadedModel?.device,
      };
      if (!loadedSnapshot || loadedSnapshot.capability === 'unknown' || loadedSnapshot.capability === 'chat') return customSnapshot;
      return { ...loadedSnapshot, recipe: loadedSnapshot.recipe || customSnapshot.recipe, checkpoint: loadedSnapshot.checkpoint || customSnapshot.checkpoint };
    }
    const knownSnapshot = snapshotFromModelInfo(currentKnownModelInfo);
    if (knownSnapshot && (!loadedSnapshot || loadedSnapshot.capability === 'unknown' || (loadedSnapshot.capability === 'chat' && knownSnapshot.capability !== 'chat'))) {
      return { ...knownSnapshot, device: currentLoadedModel?.device };
    }
    return loadedSnapshot || snapshotFromName(currentModel, loadedModels);
  }, [currentLoadedModel, currentCustomModelInfo, currentKnownModelInfo, currentModel, loadedModels]);
  const currentCapability = currentModelSnapshot?.capability || 'unknown';
  useEffect(() => {
    const updatePresetVersion = () => setPresetVersion(v => v + 1);
    window.addEventListener(PRESET_STORE_EVENT, updatePresetVersion);
    return () => window.removeEventListener(PRESET_STORE_EVENT, updatePresetVersion);
  }, []);
  const currentPreset = useMemo(() => currentModel ? activePresetForModel(currentModel) : null, [currentModel, presetVersion]);

  const hasRealtimeAudio = useMemo(
    () => !!currentModel && modelSupportsRealtimeAudio(currentModel, currentKnownModelInfo, currentLoadedModel),
    [currentModel, currentKnownModelInfo, currentLoadedModel],
  );
  const supportsRealtimeAudio = useMemo(
    () => canUseMicrophone() && hasRealtimeAudio,
    [hasRealtimeAudio],
  );

  const defaultImageSettings = useMemo(
    () => imageDefaultsForModel(currentLoadedModel, currentKnownModelInfo),
    [currentLoadedModel, currentKnownModelInfo],
  );
  const defaultImageSettingsKey = useMemo(() => JSON.stringify(defaultImageSettings), [defaultImageSettings]);

  const markImageSettingsEdited = useCallback((updater: React.SetStateAction<ImageGenerationSettings>) => {
    imageSettingsTouchedRef.current = true;
    setImageSettings(updater);
  }, []);

  const supportsImageEdit = useMemo(
    () => currentCapability === 'image' && modelSupportsImageEdit(currentModel, currentKnownModelInfo, currentLoadedModel),
    [currentCapability, currentModel, currentKnownModelInfo, currentLoadedModel],
  );

  useEffect(() => {
    const modelKey = currentModel || '';
    const switchedModel = imageSettingsModelRef.current !== modelKey;
    if (switchedModel) {
      imageSettingsModelRef.current = modelKey;
      imageSettingsTouchedRef.current = false;
      imageSettingsCommittedRef.current = false;
      setImageSettings(defaultImageSettings);
      setImageMode('generate');
      return;
    }

    if (currentCapability === 'image' && !imageSettingsTouchedRef.current && !imageSettingsCommittedRef.current) {
      setImageSettings(defaultImageSettings);
    }
  }, [currentModel, currentCapability, defaultImageSettingsKey]);

  useEffect(() => {
    if (!supportsImageEdit && imageMode !== 'generate') {
      setImageMode('generate');
      setPendingImages([]);
    }
  }, [supportsImageEdit, imageMode]);

  const capabilityForLoaded = useCallback((model: LoadedModel) => {
    const customInfo = customModelInfos.find(m => (m.name || m.id) === model.model_name);
    return customInfo ? capabilityFromModelInfo(customInfo) : capabilityFromLoaded(model);
  }, [customModelInfos]);
  const selectableModels = useMemo(
    () => loadedModels.filter(m => canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'tts'].includes(capabilityForLoaded(m))),
    [loadedModels, capabilityForLoaded],
  );
  type ModelPickerOption = {
    name: string;
    capability: ModelCapability;
    loaded: boolean;
    info?: ModelInfo;
    detail: string;
  };

  const modelPickerOptions = useMemo<ModelPickerOption[]>(() => {
    const seen = new Set<string>();
    const options: ModelPickerOption[] = [];
    const addOption = (option: ModelPickerOption) => {
      const key = option.name.toLowerCase();
      if (!option.name || seen.has(key)) return;
      if (!['chat', 'omni', 'image', 'audio', 'tts'].includes(option.capability)) return;
      seen.add(key);
      options.push(option);
    };

    selectableModels.forEach(model => addOption({
      name: model.model_name,
      capability: capabilityForLoaded(model),
      loaded: true,
      detail: `Loaded · ${model.recipe || 'runtime'}${model.device ? ` · ${model.device}` : ''}`,
    }));

    knownModelInfos.forEach(info => {
      const name = String((info as any).model_name || info.name || info.id || '').trim();
      const capability = capabilityFromModelInfo(info);
      const labels = (info.labels || []).map(label => label.toLowerCase());
      const downloaded = labels.some(label => ['downloaded', 'local', 'installed', 'ready'].includes(label));
      addOption({
        name,
        capability,
        loaded: false,
        info,
        detail: downloaded ? 'Downloaded · click to load' : 'Registry · click to load',
      });
    });

    const q = modelPickerQuery.trim().toLowerCase();
    const filtered = q
      ? options.filter(option => `${option.name} ${capabilityLabel(option.capability)} ${option.detail}`.toLowerCase().includes(q))
      : options;
    return filtered.slice(0, 80);
  }, [capabilityForLoaded, knownModelInfos, modelPickerQuery, selectableModels]);

  const modeSupportsChatCompletions = currentLoadedModel ? canUseChatCompletions(currentLoadedModel) : (currentCapability === 'chat' || currentCapability === 'omni');
  const modeSupportsTools = modeSupportsChatCompletions;
  const canUseAudioInput = currentCapability === 'omni' || currentCapability === 'audio' || supportsRealtimeAudio;

  const handleLiveTranscription = useCallback((text: string, isFinal: boolean) => {
    if (!isLiveRecordingRef.current && liveFinalizeTimerRef.current === null) return;
    const trimmed = text.trim();
    if (!trimmed) return;
    const accumulated = liveTranscriptRef.current;
    if (isFinal) {
      const next = accumulated ? `${accumulated} ${trimmed}` : trimmed;
      liveTranscriptRef.current = next;
      setLiveTranscript(next);
    } else {
      setLiveTranscript(accumulated ? `${accumulated} ${trimmed}` : trimmed);
    }
  }, []);

  const handleLiveSpeechEvent = useCallback((event: 'started' | 'stopped') => {
    setIsSpeaking(event === 'started');
  }, []);

  const handleAudioChunk = useCallback((base64: string) => {
    realtimeRef.current?.sendAudio(base64);
  }, []);

  const handleAudioLevel = useCallback((level: number) => {
    const smoothed = audioLevelRef.current * 0.7 + level * 0.3;
    audioLevelRef.current = smoothed;
    setAudioLevel(smoothed);
  }, []);

  const { startRecording, stopRecording, error: micError } = useAudioCapture(handleAudioChunk, handleAudioLevel);

  useEffect(() => {
    if (!modelPickerOpen) return;
    const onPointerDown = (event: PointerEvent) => {
      const root = modelPickerRef.current;
      if (!root || root.contains(event.target as Node)) return;
      setModelPickerOpen(false);
    };
    window.addEventListener('pointerdown', onPointerDown);
    return () => window.removeEventListener('pointerdown', onPointerDown);
  }, [modelPickerOpen]);

  useEffect(() => {
    const currentStillUsable = currentModel && loadedModels.some(m => m.model_name === currentModel && canSelectInComposer(m));
    if (currentStillUsable || loadedModels.length === 0) return;
    const preferred = selectPreferredLoadedModel(loadedModels);
    if (preferred && canSelectInComposer(preferred)) onModelSelect(preferred.model_name);
  }, [currentModel, loadedModels, onModelSelect]);

  const updateConversation = useCallback((id: string, updater: (c: Conversation) => Conversation) => {
    setConversations(prev => prev.map(c => c.id === id ? updater(c) : c));
  }, []);

  const appendAssistantMessage = useCallback((convoId: string, message: Omit<Message, 'role'>) => {
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, { role: 'assistant', ...message }],
      updatedAt: Date.now(),
    }));
  }, [updateConversation]);

  // Streaming hook — owns token buffer, flush interval, abort controllers
  const handleStreamDone = useCallback((convoId: string, stats: ChatCompletionStats, toolCalls?: ToolCallEntry[]) => {
    const model = streamModelsRef.current[convoId] || null;
    delete streamModelsRef.current[convoId];
    const artifacts = collectToolArtifacts(toolCalls);
    const generatedImages = artifacts.filter(a => a.type === 'image').map(a => a.url);
    const generatedAudio = artifacts.find(a => a.type === 'audio');
    const mediaFallback = generatedImages.length > 0
      ? `Generated ${generatedImages.length} image${generatedImages.length === 1 ? '' : 's'} from your prompt.`
      : generatedAudio
        ? 'Generated speech audio from your text.'
        : '';
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, {
        role: 'assistant',
        content: stats.content || mediaFallback || summarizeToolOnlyResponse(toolCalls),
        thinking: stats.reasoning || undefined,
        toolCalls,
        stats,
        model,
        generatedImages: generatedImages.length > 0 ? generatedImages : undefined,
        audioUrl: generatedAudio?.url,
        audioName: generatedAudio?.name,
      }],
      updatedAt: Date.now(),
    }));
  }, [updateConversation]);

  const handleStreamError = useCallback((convoId: string, message: string) => {
    const model = streamModelsRef.current[convoId] || null;
    delete streamModelsRef.current[convoId];
    appendAssistantMessage(convoId, {
      content: friendlyChatError(message),
      model,
      isError: true,
    });
  }, [appendAssistantMessage]);

  const streaming = useChatStreaming(handleStreamDone, handleStreamError);

  // Derived: is the CURRENT conversation streaming?
  const currentStream = activeId ? streaming.getStream(activeId) : undefined;
  const isStreaming = !!currentStream;
  const isBusy = isStreaming || capabilityBusy || isLiveRecording;
  const streamingContent = currentStream?.content || '';
  const streamingThinking = currentStream?.thinking || '';
  const streamingToolStatus = currentStream?.toolStatus || '';
  const streamingToolCalls = currentStream?.toolCalls || [];
  const currentLiveStats = activeId ? streaming.getLiveStats(activeId) : undefined;

  const activeConvo = conversations.find(c => c.id === activeId) || null;
  const messages = activeConvo?.messages || [];

  // Persist conversations to localStorage only when the user explicitly opted in.
  useEffect(() => {
    saveConversations(conversations, persistHistory, storageScope);
    try { localStorage.setItem(scopedKey(storageScope, PERSIST_KEY), String(persistHistory)); } catch { /* ignore */ }
  }, [conversations, persistHistory, storageScope]);

  // Persist active conversation id
  useEffect(() => {
    saveActiveId(activeId, persistHistory, storageScope);
  }, [activeId, persistHistory, storageScope]);

  // Active ID can point at stale/missing data after manual localStorage edits or migrations.
  useEffect(() => {
    if (activeId && !conversations.some(c => c.id === activeId)) {
      setActiveId(null);
    }
  }, [activeId, conversations]);

  const scrollToBottom = useCallback(() => {
    if (scrollRafRef.current) cancelAnimationFrame(scrollRafRef.current);
    scrollRafRef.current = requestAnimationFrame(() => {
      if (threadRef.current) {
        threadRef.current.scrollTop = threadRef.current.scrollHeight;
      }
    });
  }, []);

  useEffect(() => {
    scrollToBottom();
  }, [messages, streamingContent, streamingThinking, capabilityBusy, scrollToBottom]);

  // Auto-scroll the thinking content box when sticky
  useEffect(() => {
    const el = thinkingContentRef.current;
    if (el && thinkingSticky.current) {
      el.scrollTop = el.scrollHeight;
    }
  }, [streamingThinking]);

  const handleThinkingScroll = useCallback(() => {
    const el = thinkingContentRef.current;
    if (!el) return;
    // "At bottom" = within 8px of the end
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 8;
    thinkingSticky.current = atBottom;
  }, []);

  const handleNewChat = useCallback(() => {
    setActiveId(null);
    inputRef.current?.focus();
  }, []);

  const handleSelectConversation = useCallback((id: string) => {
    setActiveId(id);
  }, []);

  const handleDeleteConversation = useCallback((e: React.MouseEvent, id: string) => {
    e.stopPropagation();
    setConversations(prev => prev.filter(c => c.id !== id));
    if (activeId === id) setActiveId(null);
  }, [activeId]);

  const handleStop = useCallback(() => {
    if (!activeId) return;
    const model = streamModelsRef.current[activeId] || currentModelSnapshot;
    const partial = streaming.stop(activeId);
    delete streamModelsRef.current[activeId];
    if (partial) {
      updateConversation(activeId, c => ({
        ...c,
        messages: [...c.messages, {
          role: 'assistant' as const,
          content: partial.content,
          thinking: partial.thinking,
          model,
        }],
        updatedAt: Date.now(),
      }));
    }
  }, [activeId, currentModelSnapshot, streaming, updateConversation]);

  const appendLiveTranscript = useCallback((text: string) => {
    if (!currentModelSnapshot) return;
    const finalText = text.trim();
    if (!finalText) return;

    // For chat/omni/audio-chat models, microphone input becomes editable draft text
    // so the same selected model can be used for both chat and audio capture.
    if (modeSupportsChatCompletions) {
      setInputValue(prev => prev.trim()
        ? `${prev.trimEnd()}

${finalText}`
        : finalText);
      requestAnimationFrame(() => inputRef.current?.focus());
      return;
    }

    const modelSnapshot = currentModelSnapshot;
    const userMessage: Message = {
      role: 'user',
      content: 'Live microphone recording',
      audioName: 'Microphone',
      model: modelSnapshot,
    };
    const assistantMessage: Message = {
      role: 'assistant',
      content: finalText,
      model: modelSnapshot,
    };

    if (!activeId) {
      const newConvo: Conversation = {
        id: generateId(),
        title: 'Live microphone recording',
        model: modelSnapshot,
        messages: [userMessage, assistantMessage],
        updatedAt: Date.now(),
        schemaVersion: STORAGE_VERSION,
      };
      setConversations(prev => [newConvo, ...prev]);
      setActiveId(newConvo.id);
      return;
    }

    updateConversation(activeId, c => ({
      ...c,
      messages: [...c.messages, userMessage, assistantMessage],
      model: modelSnapshot,
      title: c.messages.length === 0 ? 'Live microphone recording' : c.title,
      updatedAt: Date.now(),
    }));
  }, [activeId, currentModelSnapshot, modeSupportsChatCompletions, updateConversation]);

  const clearLiveMicState = useCallback(() => {
    setIsLiveRecording(false);
    setIsLiveConnected(false);
    setIsSpeaking(false);
    setAudioLevel(0);
    audioLevelRef.current = 0;
    isLiveRecordingRef.current = false;
  }, []);

  const handleMicStart = useCallback(async () => {
    if (!currentModel || !currentModelSnapshot || !supportsRealtimeAudio || isStreaming || capabilityBusy) return;
    if (liveFinalizeTimerRef.current) {
      window.clearTimeout(liveFinalizeTimerRef.current);
      liveFinalizeTimerRef.current = null;
    }
    setLiveError(null);
    setLiveTranscript('');
    liveTranscriptRef.current = '';
    try {
      const handle = await api.connectRealtimeTranscription(currentModel, {
        onConnected: () => setIsLiveConnected(true),
        onDisconnected: () => setIsLiveConnected(false),
        onError: message => setLiveError(message),
        onSpeechEvent: handleLiveSpeechEvent,
        onTranscription: handleLiveTranscription,
      });
      realtimeRef.current = handle;
      isLiveRecordingRef.current = true;
      await startRecording();
      setIsLiveRecording(true);
    } catch (err) {
      realtimeRef.current?.close();
      realtimeRef.current = null;
      stopRecording();
      clearLiveMicState();
      setLiveError(friendlyErrorMessage(err));
    }
  }, [
    capabilityBusy,
    clearLiveMicState,
    currentCapability,
    currentModel,
    currentModelSnapshot,
    handleLiveSpeechEvent,
    handleLiveTranscription,
    isStreaming,
    startRecording,
    stopRecording,
    supportsRealtimeAudio,
  ]);

  const handleMicStop = useCallback(() => {
    stopRecording();
    const handle = realtimeRef.current;
    handle?.commitAudio();
    clearLiveMicState();

    liveFinalizeTimerRef.current = window.setTimeout(() => {
      const finalText = liveTranscriptRef.current.trim();
      if (finalText) appendLiveTranscript(finalText);
      setLiveTranscript('');
      liveTranscriptRef.current = '';
      handle?.close();
      if (realtimeRef.current === handle) realtimeRef.current = null;
      liveFinalizeTimerRef.current = null;
    }, 1200);
  }, [appendLiveTranscript, clearLiveMicState, stopRecording]);

  useEffect(() => {
    return () => {
      stopRecording();
      realtimeRef.current?.close();
      if (liveFinalizeTimerRef.current) window.clearTimeout(liveFinalizeTimerRef.current);
    };
  }, [stopRecording]);

  const runCapabilityRequest = useCallback(async (
    convoId: string,
    model: ModelSnapshot,
    text: string,
    audioFiles: File[],
    images: string[] = [],
  ) => {
    setCapabilityBusy(true);
    try {
      if (model.capability === 'image') {
        if (!text) throw new Error('Image mode needs a text prompt.');
        imageSettingsCommittedRef.current = true;
        const imageOptions: Record<string, unknown> = {
          size: `${imageSettings.width}x${imageSettings.height}`,
          steps: imageSettings.steps,
          cfg_scale: imageSettings.cfgScale,
          seed: imageSettings.seed === '' ? -1 : imageSettings.seed,
        };
        const effectiveImageMode: ImageMode = images.length > 0 ? 'edit' : imageMode;
        const resultImages = effectiveImageMode === 'edit'
          ? await api.imageEdit(model.name, text, images[0], imageOptions)
          : await api.imageGeneration(model.name, text, imageOptions);
        const generatedImages = [...resultImages];
        let content = effectiveImageMode === 'edit'
          ? `Edited ${resultImages.length} image${resultImages.length === 1 ? '' : 's'} from your prompt.`
          : `Generated ${resultImages.length} image${resultImages.length === 1 ? '' : 's'} from your prompt.`;
        if (imageSettings.upscaleModel && resultImages[0]) {
          const upscaled = await api.imageUpscale(imageSettings.upscaleModel, resultImages[0]);
          generatedImages.push(upscaled);
          content = `${content} Added an upscaled version.`;
        }
        appendAssistantMessage(convoId, {
          content,
          generatedImages,
          model,
        });
      } else if (model.capability === 'tts') {
        if (!text) throw new Error('TTS mode needs text to speak.');
        const audio = await api.textToSpeech(model.name, text);
        appendAssistantMessage(convoId, {
          content: 'Generated speech audio from your text.',
          audioUrl: audio.url,
          audioName: `${model.name}.wav`,
          model,
        });
      } else if (model.capability === 'audio') {
        const file = audioFiles[0];
        if (!file) throw new Error('Audio mode needs an audio file to transcribe.');
        const transcript = await api.audioTranscription(model.name, file);
        appendAssistantMessage(convoId, {
          content: transcript,
          model,
        });
      } else {
        throw new Error(`${capabilityLabel(model.capability)} models cannot be used from the chat composer yet.`);
      }
      onRefresh();
    } catch (err) {
      appendAssistantMessage(convoId, {
        content: friendlyChatError(friendlyErrorMessage(err)),
        model,
        isError: true,
      });
    } finally {
      setCapabilityBusy(false);
    }
  }, [appendAssistantMessage, imageMode, imageSettings, onRefresh]);

  const startAssistantResponse = useCallback(async (
    convoId: string,
    modelSnapshot: ModelSnapshot,
    userMessage: Message,
    priorMessages: Message[],
    audioFiles: File[],
    appendUserToConversation: boolean,
  ) => {
    const text = userMessage.content.trim();
    const images = userMessage.images?.length ? [...userMessage.images] : undefined;
    const hasImages = !!images?.length;
    const collectionInfo = currentKnownModelInfo && isCollectionModel(currentKnownModelInfo) ? currentKnownModelInfo : null;

    if (appendUserToConversation) {
      updateConversation(convoId, c => ({
        ...c,
        messages: [...c.messages, userMessage],
        model: modelSnapshot,
        title: c.messages.length === 0 ? titleFromInput(text, hasImages, audioFiles) : c.title,
        updatedAt: Date.now(),
      }));
    }

    thinkingSticky.current = true;

    if (!modeSupportsChatCompletions) {
      if (modelSnapshot.capability === 'audio' && audioFiles.length === 0) {
        appendAssistantMessage(convoId, {
          content: friendlyChatError('Retrying an audio transcription needs the original audio file. Please attach it again.'),
          model: modelSnapshot,
          isError: true,
        });
        return;
      }
      await runCapabilityRequest(convoId, modelSnapshot, text, audioFiles, images || []);
      return;
    }

    let requestModelName = currentModel || modelSnapshot.name;
    let requestText = text;
    let requestImages = images;
    let includeDirectAudioParts = canUseAudioInput && modeSupportsChatCompletions && audioFiles.length > 0;

    const omniRuntime = collectionInfo
      ? buildOmniToolRuntime(collectionInfo, knownModelInfos, {
          attachedImages: images || [],
          attachedAudioFiles: audioFiles,
          previousImages: collectConversationImages(priorMessages),
        })
      : null;

    if (collectionInfo) {
      const primaryChatComponent = getPrimaryChatComponent(collectionInfo, knownModelInfos);
      if (omniRuntime) {
        requestModelName = primaryChatComponent || requestModelName;
        requestImages = undefined;
        includeDirectAudioParts = false;

        const placeholders: string[] = [];
        if (hasImages) placeholders.push(...(images || []).map((_, i) => `[User provided image #${i + 1}]`));
        if (audioFiles.length > 0) placeholders.push(...audioFiles.slice(0, 1).map((file, i) => `[User provided audio file #${i + 1}: ${file.name}]`));
        if (placeholders.length > 0) {
          requestText = `${requestText || 'Please respond to the attached media.'}\n\n${placeholders.join('\n')}`.trim();
        }
      } else {
        const visionComponent = hasImages ? getVisionChatComponent(collectionInfo, knownModelInfos) : null;
        requestModelName = visionComponent || primaryChatComponent || requestModelName;
        requestImages = visionComponent ? images : undefined;
        includeDirectAudioParts = false;
        if (hasImages && !visionComponent) {
          requestText = `${requestText || 'Please respond to the attached image.'}\n\n[Omni collection note: no vision-capable component is configured for this custom/registry collection, so the image itself was not sent.]`.trim();
        }
        if (audioFiles.length > 0) {
          const transcriptionComponent = getAudioTranscriptionComponent(collectionInfo, knownModelInfos);
          if (transcriptionComponent) {
            try {
              const transcript = await api.audioTranscription(transcriptionComponent, audioFiles[0]);
              requestText = `${requestText || 'Please respond to this audio file.'}\n\nAudio transcript (${audioFiles[0].name}):\n${transcript}`.trim();
            } catch (err) {
              appendAssistantMessage(convoId, {
                content: friendlyChatError(friendlyErrorMessage(err)),
                model: modelSnapshot,
                isError: true,
              });
              return;
            }
          } else {
            requestText = `${requestText || 'Please respond to this audio file.'}\n\n[Omni collection note: no audio transcription component is configured for this collection.]`.trim();
          }
        }
      }
    }

    const toolRuntime = composeToolRuntimes([
      omniRuntime,
      useTools && modeSupportsTools ? LEMONADE_TOOL_RUNTIME : null,
    ]);

    // Build chat history from the conversation's messages before this user prompt.
    // Do not feed prior friendly UI error messages or generated media artifacts back as assistant context.
    const chatMessages: ChatMessage[] = [];

    const systemPrompts: string[] = [];
    if (omniRuntime?.systemPrompt) systemPrompts.push(omniRuntime.systemPrompt);

    // Inject a system prompt when Lemonade tools are enabled so the model knows to use them.
    // Keep this privacy-safe: no backend URLs, API keys, PIDs, or local paths.
    if (useTools && modeSupportsTools) {
      const loadedList = loadedModels.length > 0
        ? loadedModels.map(m => `${m.model_name} (${capabilityLabel(capabilityFromLoaded(m))})`).join(', ')
        : 'none';
      systemPrompts.push([
        'You are a helpful assistant integrated with a local AI inference app called Lemonade.',
        'Use Lemonade management tools when the user asks about local models, backends, hardware capability, or server health.',
        'When presenting choices, use the ask_question tool so the UI can render clickable options.',
        '',
        'RICH CONTENT: Responses are rendered as Markdown with code blocks, Mermaid diagrams, HTML snippets, and LaTeX math.',
        '',
        `Currently loaded model names and capabilities: ${loadedList}`,
        `Active composer model: ${currentModel || 'none'}`,
      ].join('\n'));
    }

    if (systemPrompts.length > 0) {
      chatMessages.push({ role: 'system' as const, content: systemPrompts.join('\n\n') });
    }

    const historyMessages = priorMessages.filter(m => {
      if (m.role === 'assistant' && !isPersistableAssistantMessage(m)) return false;
      if (m.generatedImages?.length || m.audioUrl) return false;
      return true;
    });

    chatMessages.push(...historyMessages.map(m => {
      if (m.images?.length) {
        return {
          role: m.role,
          content: [
            { type: 'text' as const, text: m.content },
            ...m.images.map(url => ({ type: 'image_url' as const, image_url: { url } })),
          ],
        };
      }
      return { role: m.role, content: m.content };
    }));

    // Add the user message being sent or retried.
    if (requestImages?.length || includeDirectAudioParts) {
      const audioParts = includeDirectAudioParts
        ? await Promise.all(audioFiles.slice(0, 1).map(audioToInputAudio))
        : [];
      chatMessages.push({
        role: 'user' as const,
        content: [
          { type: 'text' as const, text: requestText || (audioFiles[0] ? `Please respond to this audio file: ${audioFiles[0].name}` : '') },
          ...(requestImages || []).map(url => ({ type: 'image_url' as const, image_url: { url } })),
          ...audioParts,
        ],
      });
    } else {
      chatMessages.push({ role: 'user' as const, content: requestText });
    }

    streamModelsRef.current[convoId] = modelSnapshot;
    await streaming.send(convoId, requestModelName, chatMessages, toolRuntime);
  }, [
    appendAssistantMessage,
    currentCapability,
    currentKnownModelInfo,
    imageMode,
    currentModel,
    knownModelInfos,
    loadedModels,
    modeSupportsChatCompletions,
    modeSupportsTools,
    canUseAudioInput,
    runCapabilityRequest,
    streaming,
    updateConversation,
    useTools,
  ]);

  const handleSend = async (overrideText?: string) => {
    const text = (overrideText ?? inputValue).trim();
    const audioFiles = [...pendingAudioFiles];
    const hasImages = pendingImages.length > 0;
    const canSubmitContent = currentCapability === 'audio' && !modeSupportsChatCompletions
      ? audioFiles.length > 0
      : currentCapability === 'image'
        ? (imageMode === 'edit' ? (!!text && hasImages) : !!text)
        : (!!text || hasImages || (canUseAudioInput && audioFiles.length > 0));
    if (!canSubmitContent || isBusy) return;
    if (!api.isConnected || !currentModel || !currentModelSnapshot) return;

    let convoId = activeId;
    const modelSnapshot = currentModelSnapshot;
    const currentMessages = (conversations.find(c => c.id === convoId)?.messages || []);

    // Create a new conversation if none is active
    if (!convoId) {
      const newConvo: Conversation = {
        id: generateId(),
        title: titleFromInput(text, hasImages, audioFiles),
        model: modelSnapshot,
        messages: [],
        updatedAt: Date.now(),
        schemaVersion: STORAGE_VERSION,
      };
      convoId = newConvo.id;
      setConversations(prev => [newConvo, ...prev]);
      setActiveId(convoId);
    }

    const userMessage: Message = {
      role: 'user',
      content: text || (audioFiles[0] ? `Audio file: ${audioFiles[0].name}` : ''),
      images: hasImages ? [...pendingImages] : undefined,
      audioName: audioFiles[0]?.name,
      model: modelSnapshot,
    };

    setInputValue('');
    setPendingImages([]);
    setPendingAudioFiles([]);

    await startAssistantResponse(convoId, modelSnapshot, userMessage, currentMessages, audioFiles, true);
  };

  const handleRetryAssistant = useCallback(async (messageIndex: number) => {
    if (!activeId || isBusy) return;
    if (!api.isConnected || !currentModel || !currentModelSnapshot) return;
    const convo = conversations.find(c => c.id === activeId);
    if (!convo || convo.messages[messageIndex]?.role !== 'assistant') return;

    let userIndex = messageIndex - 1;
    while (userIndex >= 0 && convo.messages[userIndex].role !== 'user') userIndex--;
    if (userIndex < 0) return;

    const originalUserMessage = convo.messages[userIndex];
    if (originalUserMessage.audioName && !originalUserMessage.images?.length && !originalUserMessage.content.trim()) {
      appendAssistantMessage(activeId, {
        content: friendlyChatError('Retrying an audio transcription needs the original audio file. Please attach it again.'),
        model: currentModelSnapshot,
        isError: true,
      });
      return;
    }

    const trimmedMessages = convo.messages.slice(0, userIndex + 1);
    setConversations(prev => prev.map(c => c.id === activeId ? {
      ...c,
      messages: trimmedMessages,
      updatedAt: Date.now(),
    } : c));

    await startAssistantResponse(
      activeId,
      currentModelSnapshot,
      { ...originalUserMessage, model: currentModelSnapshot },
      trimmedMessages.slice(0, -1),
      [],
      false,
    );
  }, [activeId, appendAssistantMessage, conversations, currentModel, currentModelSnapshot, isBusy, startAssistantResponse]);

  const handleEditUserMessage = useCallback(async (messageIndex: number, revisedContent: string) => {
    const text = revisedContent.trim();
    if (!text || !activeId || isBusy) return;
    if (!api.isConnected || !currentModel || !currentModelSnapshot) return;
    const convo = conversations.find(c => c.id === activeId);
    if (!convo || convo.messages[messageIndex]?.role !== 'user') return;

    const originalMessage = convo.messages[messageIndex];
    const priorMessages = convo.messages.slice(0, messageIndex);
    const editedUserMessage: Message = {
      ...originalMessage,
      content: text,
      model: currentModelSnapshot,
    };

    setConversations(prev => prev.map(c => c.id === activeId ? {
      ...c,
      messages: [...priorMessages, editedUserMessage],
      model: currentModelSnapshot,
      title: messageIndex === 0 ? titleFromInput(text, !!editedUserMessage.images?.length) : c.title,
      updatedAt: Date.now(),
    } : c));

    await startAssistantResponse(activeId, currentModelSnapshot, editedUserMessage, priorMessages, [], false);
  }, [activeId, conversations, currentModel, currentModelSnapshot, isBusy, startAssistantResponse]);

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  // ── Attachment handling ────────────────────────────────────

  const addAttachments = useCallback(async (files: File[]) => {
    if (canUseAudioInput) {
      const audioFiles = files.filter(f => f.type.startsWith('audio/'));
      if (audioFiles.length > 0) {
        setPendingAudioFiles(audioFiles.slice(0, 1));
        if (currentCapability === 'audio' && !modeSupportsChatCompletions) return;
      }
    }

    const imageFiles = files.filter(f => f.type.startsWith('image/'));
    if (imageFiles.length === 0) return;
    if (currentCapability === 'image' && imageMode !== 'edit') return;

    if (currentCapability === 'image' && imageMode === 'edit') {
      const encoded = await imageToBase64(imageFiles[0]);
      setPendingImages([encoded]);
      return;
    }

    const remaining = MAX_IMAGES - pendingImages.length;
    const toProcess = imageFiles.slice(0, remaining);
    const encoded = await Promise.all(toProcess.map(imageToBase64));
    setPendingImages(prev => [...prev, ...encoded].slice(0, MAX_IMAGES));
  }, [canUseAudioInput, currentCapability, imageMode, modeSupportsChatCompletions, pendingImages.length]);

  const handlePaste = useCallback((e: React.ClipboardEvent) => {
    const items = e.clipboardData?.items;
    if (!items) return;
    const files: File[] = [];
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      if (item.type.startsWith('image/') || item.type.startsWith('audio/')) {
        const file = item.getAsFile();
        if (file) files.push(file);
      }
    }
    if (files.length > 0) {
      e.preventDefault();
      addAttachments(files);
    }
  }, [addAttachments]);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const files = Array.from(e.dataTransfer.files);
    addAttachments(files);
  }, [addAttachments]);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
  }, []);

  const handleFileSelect = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const files = Array.from(e.target.files || []);
    addAttachments(files);
    e.target.value = '';
  }, [addAttachments]);

  const removeImage = useCallback((index: number) => {
    setPendingImages(prev => prev.filter((_, i) => i !== index));
  }, []);

  const removeAudio = useCallback(() => {
    setPendingAudioFiles([]);
  }, []);

  const handlePersistenceToggle = useCallback(() => {
    setPersistHistory(prev => !prev);
  }, []);

  const handleModelPickerSelect = useCallback(async (option: ModelPickerOption) => {
    if (option.loaded) {
      onModelSelect(option.name);
      setModelPickerOpen(false);
      setModelPickerQuery('');
      return;
    }
    if (!api.isConnected || modelPickerLoading) return;
    setModelPickerError(null);
    setModelPickerLoading(option.name);
    try {
      await api.loadModel(option.name, undefined, option.info || null);
      await Promise.resolve(onRefresh());
      onModelSelect(option.name);
      setModelPickerOpen(false);
      setModelPickerQuery('');
    } catch (err) {
      setModelPickerError(friendlyErrorMessage(err));
    } finally {
      setModelPickerLoading(null);
    }
  }, [modelPickerLoading, onModelSelect, onRefresh]);

  // ── Option select from assistant messages ───────────────────

  const handleOptionSelect = useCallback((text: string) => {
    handleSend(text);
  }, [handleSend]);

  const hasMessages = messages.length > 0 || isStreaming || capabilityBusy;
  const canAttach = currentCapability === 'chat'
    || currentCapability === 'omni'
    || currentCapability === 'audio'
    || supportsRealtimeAudio
    || (currentCapability === 'image' && imageMode === 'edit');
  const fileAccept = canUseAudioInput
    ? (currentCapability === 'image' ? 'image/*' : 'image/*,audio/*')
    : 'image/*';
  const canSubmit = !!currentModel && !isBusy && (currentCapability === 'audio' && !modeSupportsChatCompletions
    ? pendingAudioFiles.length > 0
    : currentCapability === 'image'
      ? (imageMode === 'edit' ? (!!inputValue.trim() && pendingImages.length > 0) : !!inputValue.trim())
      : (!!inputValue.trim() || pendingImages.length > 0 || (canUseAudioInput && pendingAudioFiles.length > 0)));
  const composerPlaceholder = !currentModel
    ? 'Draft a message — connect and load a model to send…'
    : currentCapability === 'omni' || supportsRealtimeAudio
      ? `Message ${currentModel} with text${canUseAudioInput ? ', images, or audio' : ' or images'}…`
      : currentCapability === 'image'
      ? (imageMode === 'edit' ? `Describe the edit for ${currentModel}…` : `Describe an image for ${currentModel}…`)
      : currentCapability === 'audio'
        ? `Attach audio or use the mic with ${currentModel}…`
        : currentCapability === 'tts'
          ? `Text to speak with ${currentModel}…`
          : `Message ${currentModel}…`;
  const composerHint = supportsRealtimeAudio && modeSupportsChatCompletions
    ? 'Chat + audio mode · mic transcribes into the draft, and audio files are routed through chat completions'
    : currentCapability === 'omni'
    ? 'Omni mode · text, image and audio are routed through chat completions'
    : currentCapability === 'image'
      ? (imageMode === 'edit' ? 'Image mode · attach one source image and prompt becomes /images/edits' : 'Image mode · prompt becomes /images/generations')
    : currentCapability === 'audio'
      ? 'Audio mode · attach a file for /audio/transcriptions or use live mic via /v1/realtime'
      : currentCapability === 'tts'
        ? 'TTS mode · text becomes /audio/speech'
        : 'Enter to send · Shift+Enter for newline · Paste or drop images';

  const upscalingModels = useMemo(
    () => knownModelInfos
      .filter(info => Array.isArray(info.labels) && info.labels.includes('upscaling'))
      .map(info => String(info.name || info.id))
      .filter(Boolean),
    [knownModelInfos],
  );

  return (
    <div className={`chat ${railExpanded ? 'rail-expanded' : ''}${showInlineLogs ? ' chat--with-logs' : ''}`}>
      {/* Conversation rail */}
      <aside className="rail">
        <div className="rail__head">
          <button
            className="rail__toggle"
            onClick={() => setRailExpanded(!railExpanded)}
            aria-label="Toggle conversations"
          >
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round">
              <line x1="3" y1="4" x2="13" y2="4" />
              <line x1="3" y1="8" x2="13" y2="8" />
              <line x1="3" y1="12" x2="13" y2="12" />
            </svg>
          </button>
          <span className="rail__title">Conversations</span>
        </div>

        <div className="rail__new-wrap">
          <button className="rail__new" onClick={handleNewChat} aria-label="New chat">
            <span className="rail__new-icon" aria-hidden="true">
              <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                <line x1="7" y1="2.5" x2="7" y2="11.5" />
                <line x1="2.5" y1="7" x2="11.5" y2="7" />
              </svg>
            </span>
            <span className="rail__new-label">New chat</span>
          </button>
        </div>

        <ul className="rail__list" role="listbox">
          {conversations.map(c => {
            const badge = capabilityBadge(c.model?.capability || 'chat');
            return (
              <li
                className={`rail__item ${c.id === activeId ? 'is-active' : ''}`}
                key={c.id}
                role="option"
                onClick={() => handleSelectConversation(c.id)}
              >
                <span className="rail__item-title">
                  {c.title || deriveTitle(c.messages)}
                </span>
                <span className="rail__item-meta">
                  {streaming.streamingConvoIds.has(c.id) && (
                    <span className="rail__streaming-badge">● generating</span>
                  )}
                  <span className={`rail__model-badge rail__model-badge--${badge}`}>
                    {badge}
                  </span>
                  <span>{timeAgo(c.updatedAt)}</span>
                </span>
                <button
                  className="rail__item-delete"
                  onClick={(e) => handleDeleteConversation(e, c.id)}
                  aria-label="Delete conversation"
                  title="Delete"
                >×</button>
              </li>
            );
          })}
          {conversations.length === 0 && (
            <li className="rail__empty">No conversations yet</li>
          )}
        </ul>

        <div className="rail__privacy">
          <label className="rail__privacy-toggle">
            <input type="checkbox" checked={persistHistory} onChange={handlePersistenceToggle} />
            <span>{accountSession.isGuest ? 'Shared guest history' : 'Private local history'} {persistHistory ? 'ON' : 'OFF'}</span>
          </label>
          <span className="rail__privacy-note">{describeSession(accountSession)} · Media is never persisted.</span>
        </div>
      </aside>

      {/* Main pane */}
      <div className="chat__main" ref={threadRef}>
        <div className="chat__inner">
          {!hasMessages ? (
            <EmptyState
              loadedModels={loadedModels}
              currentModel={currentModel}
              onModelSelect={onModelSelect}
              onChipClick={(text) => setInputValue(text)}
              customModelInfos={customModelInfos}
            />
          ) : (
            <div className="thread">
              {messages.map((msg, i) => (
                <MessageBubble
                  key={i}
                  message={msg}
                  activeModel={currentModelSnapshot}
                  userLabel={accountSession.isGuest ? 'Guest' : accountSession.name}
                  onOptionSelect={handleOptionSelect}
                  onRetry={msg.role === 'assistant' ? () => handleRetryAssistant(i) : undefined}
                  onEditUser={msg.role === 'user' ? (text) => handleEditUserMessage(i, text) : undefined}
                />
              ))}

              {isStreaming && (
                <article className="message message--assistant">
                  <div className="message__avatar">
                    {modelInitial(currentModelSnapshot)}
                  </div>
                  <div className="message__body">
                    <div className="message__author-row">
                      <div className="message__author">{modelDisplayName(currentModelSnapshot)}</div>
                      {currentModelSnapshot?.name && <CopyInlineButton text={currentModelSnapshot.name} title="Copy model name" className="copy-inline--author" />}
                    </div>
                    {streamingThinking && (
                      <details className="message__thinking" open={streaming.thinkingExpanded}>
                        <summary>Thinking…</summary>
                        <div
                          className="message__thinking-content"
                          ref={thinkingContentRef}
                          onScroll={handleThinkingScroll}
                        >
                          <MarkdownMessage content={streamingThinking} isComplete={false} />
                        </div>
                      </details>
                    )}
                    {streamingToolCalls.length > 0 && <ToolCallsDisplay calls={streamingToolCalls} onOptionSelect={handleOptionSelect} />}
                    {streamingContent ? (
                      <MarkdownMessage content={streamingContent} isComplete={false} onOptionSelect={handleOptionSelect} />
                    ) : !streamingThinking ? (
                      <div className="message__content">
                        <span className="streaming-cursor" aria-hidden="true" />
                      </div>
                    ) : null}
                    {streamingContent && <span className="streaming-cursor" aria-hidden="true" />}
                    {currentLiveStats && (
                      <div className="message__live-stats">
                        <span>{currentLiveStats.tps.toFixed(1)} tok/s</span>
                        {currentLiveStats.ttft != null && <span>{(currentLiveStats.ttft / 1000).toFixed(2)}s TTFT</span>}
                        <span>{currentLiveStats.tokens + currentLiveStats.reasoningTokens} tokens</span>
                        <span>{(currentLiveStats.elapsed / 1000).toFixed(1)}s</span>
                      </div>
                    )}
                  </div>
                </article>
              )}

              {capabilityBusy && !isStreaming && (
                <article className="message message--assistant">
                  <div className="message__avatar"><CapabilityIcon capability={currentCapability} size={16} /></div>
                  <div className="message__body">
                    <div className="message__author-row">
                      <div className="message__author">{modelDisplayName(currentModelSnapshot)}</div>
                      {currentModelSnapshot?.name && <CopyInlineButton text={currentModelSnapshot.name} title="Copy model name" className="copy-inline--author" />}
                    </div>
                    <div className="message__content message__content--pending">
                      <span className="streaming-cursor" aria-hidden="true" />
                      Working in {capabilityLabel(currentCapability)} mode…
                    </div>
                  </div>
                </article>
              )}
            </div>
          )}
        </div>
      </div>

      {showInlineLogs && (
        <aside className="chat__logs" aria-label="Lemonade logs next to chat">
          <LogViewer />
        </aside>
      )}

      {/* Composer */}
      <div className="composer" onDrop={handleDrop} onDragOver={handleDragOver}>
        <div className="composer__toolbar">
          {(modelPickerOptions.length > 0 || modelPickerOpen) && (
            <div className="composer__model-picker" ref={modelPickerRef}>
              <span className="composer__model-label">Model</span>
              <button
                type="button"
                className="composer__model-button"
                onClick={() => { setModelPickerOpen(v => !v); setModelPickerError(null); }}
                aria-haspopup="listbox"
                aria-expanded={modelPickerOpen}
              >
                <CapabilityIcon capability={currentCapability} size={14} />
                <span className="composer__model-button-name">{currentModel || 'Choose model'}</span>
                <span className="composer__model-button-caret">▾</span>
              </button>
              {modelPickerOpen && (
                <div className="composer__model-menu" role="dialog" aria-label="Search models">
                  <label className="composer__model-search">
                    <Icon name="search" size={14} />
                    <input
                      autoFocus
                      value={modelPickerQuery}
                      placeholder="Search loaded or downloaded models…"
                      onChange={e => setModelPickerQuery(e.target.value)}
                    />
                  </label>
                  <div className="composer__model-results" role="listbox">
                    {modelPickerOptions.map(option => (
                      <button
                        type="button"
                        key={option.name}
                        className={`composer__model-option${option.name === currentModel ? ' is-active' : ''}`}
                        onClick={() => handleModelPickerSelect(option)}
                        disabled={!!modelPickerLoading}
                        role="option"
                        aria-selected={option.name === currentModel}
                      >
                        <CapabilityIcon capability={option.capability} size={15} />
                        <span className="composer__model-option-text">
                          <strong>{option.name}</strong>
                          <span>{capabilityLabel(option.capability)} · {option.detail}</span>
                        </span>
                        {modelPickerLoading === option.name && <span className="composer__model-option-loading">Loading…</span>}
                      </button>
                    ))}
                    {modelPickerOptions.length === 0 && <div className="composer__model-empty">No matching models</div>}
                  </div>
                  {modelPickerError && <div className="composer__model-error">{modelPickerError}</div>}
                </div>
              )}
            </div>
          )}
          <button
            type="button"
            className={`composer__mode-badge composer__mode-badge--${capabilityBadge(currentCapability)} composer__mode-badge--interactive`}
            onClick={() => setModelPickerOpen(true)}
            title="Mode follows the selected model. Open model search to change it."
          >
            <CapabilityIcon capability={currentCapability} size={13} /> {supportsRealtimeAudio && modeSupportsChatCompletions ? 'Chat + Audio' : capabilityLabel(currentCapability)} mode
          </button>
          {currentPreset && (
            <span className="composer__preset-badge" title="Active preset for this model">
              <span aria-hidden="true">{presetIcon(currentPreset)}</span> Preset: {currentPreset.name}
            </span>
          )}
          <button
            className={`composer__tools-toggle ${useTools ? 'composer__tools-toggle--active' : ''}`}
            onClick={() => {
              const next = !useTools;
              setUseTools(next);
              try { localStorage.setItem(scopedKey(storageScope, TOOLS_KEY), String(next)); } catch { /* ignore */ }
            }}
            disabled={!modeSupportsTools}
            title={modeSupportsTools
              ? (useTools ? 'Lemonade tools enabled — click to disable' : 'Enable lemonade tools (model management via chat)')
              : 'Tools are only available for chat-completion models'}
            aria-pressed={useTools && modeSupportsTools}
          >
            <Icon name="tools" size={13} /> Tools {useTools && modeSupportsTools ? 'ON' : 'OFF'}
          </button>
          <button
            className={`composer__tools-toggle ${showInlineLogs ? 'composer__tools-toggle--active' : ''}`}
            onClick={() => setShowInlineLogs(v => !v)}
            aria-pressed={showInlineLogs}
            title="Show logs next to the chat"
          >
            <Icon name="logs" size={13} /> Logs
          </button>
        </div>
        {streamingToolStatus && (
          <div className="composer__tool-status">
            <span className="composer__tool-status-dot" />
            {streamingToolStatus}
          </div>
        )}
        {currentCapability === 'image' && (
          <div className="composer__image-settings" aria-label="Image generation settings">
            <label className="composer__image-setting composer__image-setting--mode">
              <span>Mode</span>
              <select
                value={imageMode}
                onChange={e => {
                  const nextMode = e.target.value as ImageMode;
                  setImageMode(nextMode);
                  if (nextMode === 'generate') setPendingImages([]);
                }}
                disabled={isBusy}
              >
                <option value="generate">Generate</option>
                {supportsImageEdit && <option value="edit">Edit</option>}
              </select>
            </label>
            <label className="composer__image-setting">
              <span>Steps</span>
              <input
                type="number"
                min={1}
                max={50}
                value={imageSettings.steps}
                onChange={e => markImageSettingsEdited(prev => ({ ...prev, steps: Math.max(1, parseInt(e.target.value, 10) || 1) }))}
                disabled={isBusy}
              />
            </label>
            <label className="composer__image-setting">
              <span>CFG Scale</span>
              <input
                type="number"
                min={1}
                max={20}
                step={0.5}
                value={imageSettings.cfgScale}
                onChange={e => markImageSettingsEdited(prev => ({ ...prev, cfgScale: Math.max(1, parseFloat(e.target.value) || 1) }))}
                disabled={isBusy}
              />
            </label>
            <label className="composer__image-setting">
              <span>Width</span>
              <select
                value={imageSettings.width}
                onChange={e => markImageSettingsEdited(prev => ({ ...prev, width: parseInt(e.target.value, 10) }))}
                disabled={isBusy}
              >
                {IMAGE_SIZE_OPTIONS.map(size => <option key={size} value={size}>{size}</option>)}
              </select>
            </label>
            <label className="composer__image-setting">
              <span>Height</span>
              <select
                value={imageSettings.height}
                onChange={e => markImageSettingsEdited(prev => ({ ...prev, height: parseInt(e.target.value, 10) }))}
                disabled={isBusy}
              >
                {IMAGE_SIZE_OPTIONS.map(size => <option key={size} value={size}>{size}</option>)}
              </select>
            </label>
            <label className="composer__image-setting">
              <span>Seed</span>
              <input
                type="number"
                min={-1}
                value={imageSettings.seed}
                placeholder="-1"
                onChange={e => {
                  const value = e.target.value;
                  if (value === '') {
                    markImageSettingsEdited(prev => ({ ...prev, seed: '' }));
                    return;
                  }
                  const seed = parseInt(value, 10);
                  markImageSettingsEdited(prev => ({ ...prev, seed: Number.isNaN(seed) ? -1 : Math.max(seed, -1) }));
                }}
                disabled={isBusy}
              />
            </label>
            <label className="composer__image-setting composer__image-setting--upscale">
              <span>Upscale</span>
              <select
                value={imageSettings.upscaleModel}
                onChange={e => markImageSettingsEdited(prev => ({ ...prev, upscaleModel: e.target.value }))}
                disabled={isBusy || upscalingModels.length === 0}
              >
                <option value="">Off</option>
                {upscalingModels.map(name => <option key={name} value={name}>{name}</option>)}
              </select>
            </label>
          </div>
        )}
        {pendingImages.length > 0 && (
          <div className="composer__images">
            {pendingImages.map((src, i) => (
              <div key={i} className="composer__image-thumb">
                <img src={src} alt={`Attachment ${i + 1}`} />
                <button className="composer__image-remove" onClick={() => removeImage(i)} aria-label="Remove image">×</button>
              </div>
            ))}
          </div>
        )}
        {pendingAudioFiles.length > 0 && (
          <div className="composer__files">
            {pendingAudioFiles.map((file, i) => (
              <div key={`${file.name}-${i}`} className="composer__file-chip">
                <span><Icon name="mic" size={13} /> {file.name}</span>
                <button onClick={removeAudio} aria-label="Remove audio file">×</button>
              </div>
            ))}
          </div>
        )}
        {(isLiveRecording || liveTranscript || liveError || micError) && (supportsRealtimeAudio || currentCapability === 'audio') && (
          <div className={`composer__live${liveError || micError ? ' composer__live--error' : ''}`}>
            <div className="composer__live-head">
              <span className={`composer__live-dot${isSpeaking ? ' composer__live-dot--speaking' : ''}`} />
              <span>{isLiveRecording ? (isLiveConnected ? 'Live microphone' : 'Connecting microphone…') : 'Microphone'}</span>
              {isLiveRecording && <span className="composer__live-meter"><span style={{ width: `${Math.round(audioLevel * 100)}%` }} /></span>}
            </div>
            <div className="composer__live-text">
              {liveError || micError || liveTranscript || 'Listening… start speaking to see transcription.'}
            </div>
          </div>
        )}
        <div className="composer__bar">
          <button
            className="composer__attach"
            onClick={() => fileInputRef.current?.click()}
            disabled={!canAttach || !currentModel || isBusy || pendingImages.length >= MAX_IMAGES}
            title={canUseAudioInput ? 'Attach image or audio' : 'Attach image'}
            aria-label={canUseAudioInput ? 'Attach image or audio' : 'Attach image'}
          >
            <Icon name="paperclip" size={16} />
          </button>
          <input
            ref={fileInputRef}
            type="file"
            accept={fileAccept}
            multiple={!(currentCapability === 'audio' && !modeSupportsChatCompletions) && !(currentCapability === 'image' && imageMode === 'edit')}
            style={{ display: 'none' }}
            onChange={handleFileSelect}
          />
          {(supportsRealtimeAudio || isLiveRecording) && (
            <button
              className={`composer__mic${isLiveRecording ? ' composer__mic--recording' : ''}`}
              onClick={isLiveRecording ? handleMicStop : handleMicStart}
              disabled={!currentModel || (!supportsRealtimeAudio && !isLiveRecording) || ((isStreaming || capabilityBusy) && !isLiveRecording)}
              title={isLiveRecording ? 'Stop live microphone transcription' : supportsRealtimeAudio ? 'Start live microphone transcription' : 'Live microphone needs HTTPS/localhost and a realtime-capable audio model'}
              aria-label={isLiveRecording ? 'Stop live microphone transcription' : 'Start live microphone transcription'}
              aria-pressed={isLiveRecording}
            >
              <Icon name="mic" size={16} />
            </button>
          )}
          <textarea
            ref={inputRef}
            className="composer__input"
            placeholder={composerPlaceholder}
            value={inputValue}
            onChange={e => setInputValue(e.target.value)}
            onKeyDown={handleKeyDown}
            onPaste={handlePaste}
            disabled={isBusy}
            rows={1}
          />
          {isStreaming ? (
            <button className="composer__stop" onClick={handleStop} aria-label="Stop generating" title="Stop"><Icon name="stop" size={16} /></button>
          ) : (
            <button
              className="composer__send"
              onClick={() => handleSend()}
              disabled={!canSubmit}
              aria-label="Send"
            ><Icon name="send" size={16} /></button>
          )}
        </div>
        <div className="composer__hint">{composerHint}</div>
      </div>
    </div>
  );
};

/* ─── Empty state ─────────────────────────────────────── */

interface EmptyStateProps {
  loadedModels: LoadedModel[];
  currentModel: string | null;
  onModelSelect: (model: string) => void;
  onChipClick: (text: string) => void;
  customModelInfos: ModelInfo[];
}

const EmptyState: React.FC<EmptyStateProps> = ({ loadedModels, currentModel, onModelSelect, onChipClick, customModelInfos }) => (
  <>
    <div className="hero">
      <h1 className="hero__title">What's on your mind?</h1>
      <p className="hero__subtitle">
        {loadedModels.length > 0
          ? `${loadedModels.length} model${loadedModels.length > 1 ? 's' : ''} loaded. Choose the right mode, then start fresh.`
          : 'Connect to a server and load a chat, omni, image, audio, or TTS model to begin.'}
      </p>

      <div className="chips" role="list">
        <button className="chip" role="listitem" onClick={() => onChipClick('Summarize this document for me')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="file" size={16} /></span>
          Summarize a doc
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Review this code and suggest improvements')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="code" size={16} /></span>
          Code review
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Create an image of a cozy lemonade stand at sunset')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="image" size={16} /></span>
          Create image
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('Turn this text into natural speech')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="tts" size={16} /></span>
          Text to speech
        </button>
      </div>
    </div>

    {loadedModels.length > 0 && (
      <>
        <div className="section-label">
          <span>Loaded right now</span>
          <span className="section-label__rule" />
        </div>
        <div className="active-models">
          {loadedModels.map(m => {
            const customInfo = customModelInfos.find(cm => (cm.name || cm.id) === m.model_name);
            const cap = customInfo ? capabilityFromModelInfo(customInfo) : capabilityFromLoaded(m);
            const selectable = canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'tts'].includes(cap);
            const isActive = currentModel === m.model_name;
            return (
              <div className="active-card" key={m.model_name}>
                <div className="active-card__head">
                  <div>
                    <div className="active-card__name-row">
                      <div className="active-card__name">{m.model_name}</div>
                      <CopyInlineButton text={m.model_name} title="Copy model name" />
                    </div>
                    <div className="active-card__meta">{m.recipe || 'runtime'} · {m.checkpoint || 'default'}</div>
                  </div>
                  <span className="active-card__device">{m.device || 'device unknown'}</span>
                </div>
                <div className="active-card__badges">
                  <span className={`cap-badge cap-badge--${capabilityBadge(cap)}`}><CapabilityIcon capability={cap} size={13} /> {capabilityLabel(cap)}</span>
                </div>
                {isActive ? (
                  <span className="active-card__status">● Active {capabilityLabel(cap)} mode</span>
                ) : selectable ? (
                  <button className="active-card__action" onClick={() => onModelSelect(m.model_name)}>
                    Use in {capabilityLabel(cap)} mode ▸
                  </button>
                ) : (
                  <span className="active-card__status active-card__status--muted">Utility model only</span>
                )}
              </div>
            );
          })}
        </div>
      </>
    )}
  </>
);

/* ─── Message bubble ──────────────────────────────────── */

/* ── Tool call indicator ─────────────────────────────────── */

const TOOL_LABELS: Record<string, string> = {
  list_models: 'List models',
  get_model_info: 'Get model info',
  load_model: 'Load model',
  unload_model: 'Unload model',
  get_loaded_models: 'Get loaded models',
  get_server_health: 'Server health',
  pull_model: 'Pull model',
  delete_model: 'Delete model',
  get_system_info: 'System info',
  list_backends: 'List backends',
  install_backend: 'Install backend',
  ask_question: 'Asking you',
};

const ToolCallsDisplay: React.FC<{ calls: ToolCallEntry[]; onOptionSelect?: (text: string) => void }> = ({ calls, onOptionSelect }) => {
  if (calls.length === 0) return null;
  return (
    <div className="message__tool-calls">
      {calls.map((tc, i) => {
        // Render ask_question as interactive buttons directly from tool call data
        if (tc.name === 'ask_question' && tc.rawArgs && tc.status === 'done') {
          try {
            const parsed = JSON.parse(tc.rawArgs);
            const question = parsed.question || '';
            const choices: string[] = parsed.choices || [];
            const allowCustom = parsed.allowCustom !== false;
            return (
              <div key={i} className="options-block">
                {question && <div className="options-block__question">{question}</div>}
                <div className="options-block__choices">
                  {choices.map((choice: string, ci: number) => (
                    <button key={ci} className="options-block__btn" onClick={() => onOptionSelect?.(choice)}>{choice}</button>
                  ))}
                </div>
                {allowCustom && (
                  <div className="options-block__custom">
                    <input className="options-block__input" placeholder="Or type your own\u2026"
                      onKeyDown={e => { if (e.key === 'Enter' && (e.target as HTMLInputElement).value.trim()) { onOptionSelect?.((e.target as HTMLInputElement).value.trim()); } }} />
                    <button className="options-block__submit" onClick={e => {
                      const input = (e.target as HTMLElement).previousElementSibling as HTMLInputElement;
                      if (input?.value.trim()) onOptionSelect?.(input.value.trim());
                    }}>Send</button>
                  </div>
                )}
              </div>
            );
          } catch { /* fall through to normal display */ }
        }
        return (
          <details key={i} className={`message__tool-call message__tool-call--${tc.status}`}>
            <summary>
              <span className="message__tool-call-icon">{tc.status === 'running' ? <Icon name="clock" size={13} /> : tc.status === 'error' ? <Icon name="x" size={13} /> : <Icon name="check" size={13} />}</span>
              <span className="message__tool-call-name">{TOOL_LABELS[tc.name] || tc.name}</span>
              {tc.args && <span className="message__tool-call-args">{tc.args}</span>}
            </summary>
            {tc.result && <div className="message__tool-call-result">{tc.result}</div>}
          </details>
        );
      })}
    </div>
  );
};

/* ── Message bubble ──────────────────────────────────────── */

const MessageBubble: React.FC<{ message: Message; activeModel: ModelSnapshot | null; userLabel: string; onOptionSelect?: (text: string) => void; onRetry?: () => void; onEditUser?: (text: string) => void }> = ({ message, activeModel, userLabel, onOptionSelect, onRetry, onEditUser }) => {
  const [thinkingOpen, setThinkingOpen] = useState(false);
  const [isEditing, setIsEditing] = useState(false);
  const [editDraft, setEditDraft] = useState(message.content || '');

  useEffect(() => {
    if (!isEditing) setEditDraft(message.content || '');
  }, [isEditing, message.content]);

  if (message.role === 'user') {
    const saveEdit = () => {
      const trimmed = editDraft.trim();
      if (!trimmed) return;
      setIsEditing(false);
      onEditUser?.(trimmed);
    };
    return (
      <article className="message message--user">
        <div className="message__avatar">{userLabel.charAt(0).toUpperCase()}</div>
        <div className="message__body">
          <div className="message__author">{userLabel}</div>
          {message.images && message.images.length > 0 && (
            <div className="message__images">
              {message.images.map((src, i) => (
                <img key={i} src={src} alt={`Attached image ${i + 1}`} className="message__image" />
              ))}
            </div>
          )}
          {message.audioName && (
            <div className="message__file-chip"><Icon name="mic" size={13} /> {message.audioName}</div>
          )}
          {isEditing ? (
            <div className="message__edit">
              <textarea
                className="message__edit-input"
                value={editDraft}
                onChange={event => setEditDraft(event.target.value)}
                rows={Math.max(3, Math.min(10, editDraft.split('\n').length + 1))}
                autoFocus
              />
              <div className="message__edit-actions">
                <button type="button" className="message__action" onClick={saveEdit} disabled={!editDraft.trim()}><Icon name="send" size={13} /> Save & resend</button>
                <button type="button" className="message__action" onClick={() => { setEditDraft(message.content || ''); setIsEditing(false); }}><Icon name="x" size={13} /> Cancel</button>
              </div>
            </div>
          ) : message.content ? (
            <div className="message__content message__content--user">
              <MarkdownMessage content={message.content} />
            </div>
          ) : null}
          {!isEditing && onEditUser && message.content && (
            <div className="message__actions" aria-label="Message actions">
              <button type="button" className="message__action" onClick={() => setIsEditing(true)}>
                <Icon name="edit" size={13} /> Edit & resend
              </button>
            </div>
          )}
        </div>
      </article>
    );
  }

  const displayModel = message.model || activeModel;
  const articleClass = `message message--assistant${message.isError ? ' message--error' : ''}`;

  return (
    <article className={articleClass}>
      <div className="message__avatar">
        {message.isError ? '!' : modelInitial(displayModel)}
      </div>
      <div className="message__body">
        <div className="message__author-row">
          <div className="message__author">{message.isError ? 'Lemonade' : modelDisplayName(displayModel)}</div>
          {!message.isError && displayModel?.name && <CopyInlineButton text={displayModel.name} title="Copy model name" className="copy-inline--author" />}
        </div>
        {message.thinking && (
          <details
            className="message__thinking"
            open={thinkingOpen}
            onToggle={e => setThinkingOpen((e.target as HTMLDetailsElement).open)}
          >
            <summary>Reasoning {message.stats?.reasoningTokens ? `· ${message.stats.reasoningTokens} tokens` : ''}</summary>
            <div className="message__thinking-content">
              <MarkdownMessage content={message.thinking} />
            </div>
          </details>
        )}
        {message.toolCalls && <ToolCallsDisplay calls={message.toolCalls} onOptionSelect={onOptionSelect} />}
        {message.content && <MarkdownMessage content={message.content} onOptionSelect={onOptionSelect} />}
        {message.generatedImages && message.generatedImages.length > 0 && (
          <div className="message__images message__images--generated">
            {message.generatedImages.map((src, i) => (
              <img key={i} src={src} alt={`Generated image ${i + 1}`} className="message__image message__image--generated" />
            ))}
          </div>
        )}
        {message.audioUrl && (
          <div className="message__audio">
            <audio controls src={message.audioUrl}>Your browser does not support audio playback.</audio>
          </div>
        )}
        {message.stats && (
          <div className="message__metrics">
            <span>{message.stats.tps} tok/s</span>
            {message.stats.ttft && <span>{(Number(message.stats.ttft) / 1000).toFixed(2)}s TTFT</span>}
            <span>{message.stats.tokens} tokens</span>
          </div>
        )}
        <div className="message__actions" aria-label="Message actions">
          <button
            type="button"
            className="message__action"
            onClick={() => copyTextToClipboard(message.content || message.thinking || '')}
            disabled={!(message.content || message.thinking)}
          >
            <Icon name="copy" size={13} /> Copy
          </button>
          {onRetry && (
            <button type="button" className="message__action" onClick={onRetry}>
              ↻ Retry
            </button>
          )}
        </div>
      </div>
    </article>
  );
};

export default ChatView;
