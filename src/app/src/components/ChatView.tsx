import React, { Suspense, lazy, useState, useRef, useCallback, useEffect, useMemo } from 'react';
import api, { ChatMessage, ChatCompletionStats, LoadedModel, ModelInfo, RealtimeTranscriptionHandle, friendlyErrorMessage } from '../api';
import MarkdownMessage from './MarkdownMessage';
import LogViewer from './LogViewer';
import { Icon, CapabilityIcon, PresetIcon } from './Icon';

const Model3DResult = lazy(() => import('./Model3DResult'));
import { useChatStreaming, ToolCallEntry, ChatToolRuntime, ToolArtifact } from '../hooks/useChatStreaming';
import { useAudioCapture } from '../hooks/useAudioCapture';
import { useFocusTrap } from '../hooks/useFocusTrap';
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
  modelSupportsChatAudioInput,
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
import { buildOmniToolRuntime } from '../tools/omniTools';
import { buildSelectedMcpRuntime, composeMcpRuntimes } from '../tools/mcpRuntime';
import {
  DEFAULT_PRESET,
  PRESET_STORE_EVENT,
  type Preset,
  activePresetForModel,
  allStoredPresets,
  isCompatible,
  loadApplied,
  saveApplied,
  classifyPresetChange,
  runningPresetIdForModel,
  setRunningPreset,
  systemPromptTextForPreset,
  systemPromptNameForPreset,
  presetMcpServerIds,
  presetMcpDisplayText,
} from '../presetStore';
import { TTS_SETTINGS_EVENT, loadTtsPlaybackSettings, ttsVoiceFromRecipeOptions } from '../features/audio/ttsSettings';

interface Message {
  role: 'user' | 'assistant';
  content: string;
  images?: string[];  // transient base64 data URLs for user messages with images
  generatedImages?: string[]; // transient generated image data URLs
  audioUrl?: string; // transient object URL for TTS output
  audioName?: string;
  model3dUrl?: string; // transient generated GLB object URL
  model3dName?: string;
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

const CHAT_LOGS_WIDTH_KEY = 'chat_logs_panel_width';
const CHAT_LOGS_DEFAULT_WIDTH = 520;
const CHAT_LOGS_MIN_WIDTH = 340;
const CHAT_LOGS_MAX_WIDTH = 920;

function maxChatLogsWidthForViewport(railExpanded = true): number {
  if (typeof window === 'undefined') return CHAT_LOGS_MAX_WIDTH;
  const railWidth = railExpanded ? 280 : 56;
  const viewportMax = window.innerWidth - railWidth - 380;
  return Math.max(CHAT_LOGS_MIN_WIDTH, Math.min(CHAT_LOGS_MAX_WIDTH, viewportMax));
}

function clampChatLogsWidth(width: number, railExpanded = true): number {
  return Math.max(CHAT_LOGS_MIN_WIDTH, Math.min(maxChatLogsWidthForViewport(railExpanded), Math.round(width)));
}

function loadChatLogsWidth(scope: string): number {
  if (typeof window === 'undefined') return CHAT_LOGS_DEFAULT_WIDTH;
  try {
    const stored = Number(window.localStorage.getItem(scopedKey(scope, CHAT_LOGS_WIDTH_KEY)));
    const width = Number.isFinite(stored) ? stored : CHAT_LOGS_DEFAULT_WIDTH;
    return clampChatLogsWidth(width, true);
  } catch {
    return clampChatLogsWidth(CHAT_LOGS_DEFAULT_WIDTH, true);
  }
}

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
      model3dUrl: undefined,
      model3dName: undefined,
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


const ModelModeIcons: React.FC<{
  capability: ModelCapability;
  audioInput?: boolean;
  size?: number;
}> = ({ capability, audioInput = false, size = 14 }) => {
  const showAudio = audioInput && capability === 'chat';
  return (
    <span className="capability-icon-pair" aria-hidden="true">
      <CapabilityIcon capability={capability} size={size} />
      {showAudio && <CapabilityIcon capability="audio" size={Math.max(11, size - 1)} />}
    </span>
  );
};

function modelModeLabel(capability: ModelCapability, audioInput = false): string {
  return audioInput && capability === 'chat'
    ? 'Chat + Audio'
    : capabilityLabel(capability);
}

function deriveTitle(messages: Message[]): string {
  const first = messages.find(m => m.role === 'user');
  if (!first) return 'New conversation';
  return titleFromInput(first.content, !!first.images?.length);
}

function isPersistableAssistantMessage(m: Message): boolean {
  return !(m.isError || /^Error:/i.test(m.content));
}


function formatDurationMs(ms: number | null | undefined): string | null {
  if (!Number.isFinite(Number(ms)) || Number(ms) <= 0) return null;
  const value = Number(ms);
  if (value < 1000) return `${Math.round(value)}ms`;
  const seconds = value / 1000;
  if (seconds < 10) return `${seconds.toFixed(2)}s`;
  if (seconds < 60) return `${seconds.toFixed(1)}s`;
  const minutes = Math.floor(seconds / 60);
  const remainder = Math.round(seconds % 60);
  return `${minutes}m ${remainder}s`;
}

function reasoningSummary(stats: Pick<ChatCompletionStats, 'reasoningTokens' | 'reasoningElapsedMs'> | null | undefined): string {
  const parts: string[] = [];
  if (stats?.reasoningTokens) parts.push(`${stats.reasoningTokens} tokens`);
  const duration = formatDurationMs(stats?.reasoningElapsedMs);
  if (duration) parts.push(duration);
  return parts.length ? ` · ${parts.join(' · ')}` : '';
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

const MCP_ENABLED_KEY = 'mcp_enabled';
const LEGACY_TOOLS_KEY = 'use_tools';
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

interface AudioGenerationSettings {
  duration: number;
  steps: number;
  cfg: number;
  seed: number | '';
  lyrics: string;
  vocalLanguage: string;
}

const DEFAULT_AUDIO_GENERATION_SETTINGS: AudioGenerationSettings = {
  duration: 10,
  steps: 50,
  cfg: 4.5,
  seed: -1,
  lyrics: '',
  vocalLanguage: 'en',
};

type OpenMossMode = 'plain' | 'describe' | 'clone';

interface OpenMossSettings {
  mode: OpenMossMode;
  voiceDescription: string;
}

const DEFAULT_OPENMOSS_SETTINGS: OpenMossSettings = {
  mode: 'plain',
  voiceDescription: '',
};

const OPENMOSS_VOICE_DESIGN_PHRASE =
  'Hello there. This is a short sample of the voice you described.';

type Model3DSourceMode = 'image' | 'text';

interface Model3DSettings {
  sourceMode: Model3DSourceMode;
  resolution: 512 | 1024 | 1536;
  backgroundRemoval: 'birefnet' | 'threshold';
  seed: number | '';
  imageModel: string;
}

const DEFAULT_MODEL3D_SETTINGS: Model3DSettings = {
  sourceMode: 'image',
  resolution: 512,
  backgroundRemoval: 'birefnet',
  seed: -1,
  imageModel: '',
};

const MODEL3D_REFERENCE_PROMPT =
  'single subject, centered, whole object in frame, three-quarter view from slightly above showing the top and two sides, plain white background, even soft studio lighting, high detail, 3D asset render';

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

function seedFromInput(value: string): number | '' {
  if (value === '') return '';
  const parsed = Number.parseInt(value, 10);
  return Number.isNaN(parsed) ? -1 : Math.max(-1, parsed);
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

function imageDefaultsForModel(loadedModel: LoadedModel | null, modelInfo: ModelInfo | null, activePresetRecipeOptions?: Record<string, unknown> | null): ImageGenerationSettings {
  const modelImageDefaults = partialImageSettingsFromSource(modelInfo?.image_defaults as Record<string, unknown> | undefined);
  const modelRecipeOptions = partialImageSettingsFromSource(modelInfo?.recipe_options as Record<string, unknown> | undefined);
  const loadedRecipeOptions = partialImageSettingsFromSource(loadedModel?.recipe_options);
  const presetDefaults = partialImageSettingsFromSource(activePresetRecipeOptions);
  return {
    ...DEFAULT_IMAGE_SETTINGS,
    ...modelImageDefaults,
    ...modelRecipeOptions,
    ...loadedRecipeOptions,
    ...presetDefaults,
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

function collectToolArtifacts(toolCalls?: ToolCallEntry[]): ToolArtifact[] {
  return (toolCalls || []).flatMap(call => call.artifacts || []);
}

function summarizeToolOnlyResponse(toolCalls?: ToolCallEntry[]): string {
  const finished = (toolCalls || []).filter(call => call.status === 'done' || call.status === 'error');
  if (finished.length === 0) return '';
  const lines = finished.slice(0, 6).map(call => {
    const label = TOOL_LABELS[call.name] || call.name;
    const result = (call.result || '').trim();
    return result ? `**${label}**\n${result}` : `**${label}** ${call.status === 'error' ? 'failed.' : 'completed.'}`;
  });
  const suffix = finished.length > lines.length ? `\n\n…and ${finished.length - lines.length} more tool call(s).` : '';
  return `${lines.join('\n\n')}${suffix}`;
}

function collectConversationImages(messages: Message[]): string[] {
  const images: string[] = [];
  for (const message of messages) {
    if (message.images?.length) images.push(...message.images);
    if (message.generatedImages?.length) images.push(...message.generatedImages);
  }
  return images;
}

async function blobToDataUrl(file: Blob): Promise<string> {
  return await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ''));
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

/** Resize and compress a chat/image-edit attachment to a base64 data URL. */
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

async function fileToBase64(file: Blob): Promise<string> {
  const dataUrl = await blobToDataUrl(file);
  const comma = dataUrl.indexOf(',');
  return comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl;
}

async function wavVoiceSampleToBase64(file: File): Promise<string> {
  const maxBytes = 10 * 1024 * 1024;
  if (file.size > maxBytes) {
    throw new Error(`'${file.name}' is too large (max 10 MB). A few seconds of clean speech is enough.`);
  }
  const bytes = new Uint8Array(await file.arrayBuffer());
  const hasMagic = (offset: number, text: string) =>
    bytes.length >= offset + text.length
    && [...text].every((character, index) => bytes[offset + index] === character.charCodeAt(0));
  if (!hasMagic(0, 'RIFF') || !hasMagic(8, 'WAVE')) {
    throw new Error(`'${file.name}' is not a WAV file. Voice samples must use WAV audio.`);
  }
  let binary = '';
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + chunkSize));
  }
  return btoa(binary);
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
  const [audioGenerationSettings, setAudioGenerationSettings] = useState<AudioGenerationSettings>(DEFAULT_AUDIO_GENERATION_SETTINGS);
  const [openMossSettings, setOpenMossSettings] = useState<OpenMossSettings>(DEFAULT_OPENMOSS_SETTINGS);
  const [model3dSettings, setModel3dSettings] = useState<Model3DSettings>(DEFAULT_MODEL3D_SETTINGS);
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
  const [ttsPlaybackSettings, setTtsPlaybackSettings] = useState(() => loadTtsPlaybackSettings(storageScope));
  const [railExpanded, setRailExpanded] = useState(true);
  const [mobileSheetOpen, setMobileSheetOpen] = useState(false);
  const sheetHandleRef = useRef<HTMLDivElement>(null);
  const sheetTriggerRef = useRef<HTMLButtonElement>(null);
  const bottomSheetRef = useRef<HTMLDivElement>(null);
  const [useMcp, setUseMcp] = useState(() => {
    try {
      const explicit = localStorage.getItem(scopedKey(storageScope, MCP_ENABLED_KEY));
      if (explicit !== null) return explicit === 'true';
      return localStorage.getItem(scopedKey(storageScope, LEGACY_TOOLS_KEY)) === 'true';
    } catch { return false; }
  });
  const presetMcpSeedRef = useRef('');
  const [showInlineLogs, setShowInlineLogs] = useState(false);
  const [chatLogsWidth, setChatLogsWidth] = useState(() => loadChatLogsWidth(storageScope));
  const [modelPickerOpen, setModelPickerOpen] = useState(false);
  const [modelPickerQuery, setModelPickerQuery] = useState('');
  const [modelPickerLoading, setModelPickerLoading] = useState<string | null>(null);
  const [modelPickerError, setModelPickerError] = useState<string | null>(null);
  const [modelPickerUnloading, setModelPickerUnloading] = useState<string | null>(null);
  const [unloadAnnouncement, setUnloadAnnouncement] = useState('');
  const [presetPickerOpen, setPresetPickerOpen] = useState(false);
  const [presetPickerQuery, setPresetPickerQuery] = useState('');
  const [presetPickerApplying, setPresetPickerApplying] = useState<string | null>(null);
  const [presetPickerError, setPresetPickerError] = useState<string | null>(null);
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const modelPickerRef = useRef<HTMLDivElement>(null);
  const presetPickerRef = useRef<HTMLDivElement>(null);
  const thinkingContentRef = useRef<HTMLDivElement>(null);
  const thinkingSticky = useRef(true);
  const scrollRafRef = useRef<number>(0);
  const [liveText, setLiveText] = useState('');
  const [streamStatus, setStreamStatus] = useState('');
  const liveBufferRef = useRef('');
  const liveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const wasStreamingRef = useRef(false);
  const streamModelsRef = useRef<Record<string, ModelSnapshot | null>>({});
  const realtimeRef = useRef<RealtimeTranscriptionHandle | null>(null);
  const isLiveRecordingRef = useRef(false);
  const liveTranscriptRef = useRef('');
  const liveFinalizeTimerRef = useRef<number | null>(null);
  const audioLevelRef = useRef(0);
  const autoSpeechRef = useRef<{ audio: HTMLAudioElement; url: string } | null>(null);

  const generatedMediaUrlsRef = useRef<Set<string>>(new Set());
  const trackGeneratedMediaUrl = useCallback((url: string): string => {
    if (url.startsWith('blob:')) generatedMediaUrlsRef.current.add(url);
    return url;
  }, []);

  useEffect(() => () => {
    generatedMediaUrlsRef.current.forEach(url => URL.revokeObjectURL(url));
    generatedMediaUrlsRef.current.clear();
  }, []);

  useEffect(() => {
    try {
      window.localStorage.setItem(scopedKey(storageScope, CHAT_LOGS_WIDTH_KEY), String(chatLogsWidth));
    } catch {
      // Non-critical: inline log width persistence is best-effort only.
    }
  }, [chatLogsWidth, storageScope]);

  const chatLayoutStyle = useMemo(() => ({
    '--chat-logs-width': `${chatLogsWidth}px`,
  } as React.CSSProperties), [chatLogsWidth]);

  const handleChatLogsResizeStart = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    if (window.innerWidth <= 980) return;
    event.preventDefault();

    const startX = event.clientX;
    const startWidth = chatLogsWidth;
    const handle = event.currentTarget;
    try { handle.setPointerCapture(event.pointerId); } catch { /* ignore */ }

    const handlePointerMove = (moveEvent: PointerEvent) => {
      // The handle sits on the left edge of the logs panel: dragging left makes
      // the panel wider, dragging right makes it narrower.
      const nextWidth = clampChatLogsWidth(startWidth - (moveEvent.clientX - startX), railExpanded);
      setChatLogsWidth(nextWidth);
    };

    const stopResize = () => {
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('pointerup', stopResize);
      window.removeEventListener('pointercancel', stopResize);
      document.body.classList.remove('is-resizing-chat-logs');
      try { handle.releasePointerCapture(event.pointerId); } catch { /* ignore */ }
    };

    document.body.classList.add('is-resizing-chat-logs');
    window.addEventListener('pointermove', handlePointerMove);
    window.addEventListener('pointerup', stopResize, { once: true });
    window.addEventListener('pointercancel', stopResize, { once: true });
  }, [chatLogsWidth, railExpanded]);

  const handleChatLogsResizeKeyDown = useCallback((event: React.KeyboardEvent<HTMLDivElement>) => {
    const step = event.shiftKey ? 48 : 20;
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      setChatLogsWidth(width => clampChatLogsWidth(width + step, railExpanded));
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      setChatLogsWidth(width => clampChatLogsWidth(width - step, railExpanded));
    } else if (event.key === 'Home') {
      event.preventDefault();
      setChatLogsWidth(CHAT_LOGS_MIN_WIDTH);
    } else if (event.key === 'End') {
      event.preventDefault();
      setChatLogsWidth(clampChatLogsWidth(CHAT_LOGS_MAX_WIDTH, railExpanded));
    }
  }, [railExpanded]);

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
  const currentRecipe = String(currentModelSnapshot?.recipe || currentKnownModelInfo?.recipe || '').toLowerCase();
  const isAceStepAudio = currentCapability === 'audio-generation'
    && (currentRecipe.includes('acestep') || currentRecipe.includes('ace-step') || (/ace[-_ ]?step/.test(String(currentModel || '').toLowerCase())));
  const currentLabels = (currentKnownModelInfo?.labels || []).map(label => String(label).toLowerCase());
  const isOpenMossTts = currentCapability === 'tts'
    && (currentRecipe.includes('openmoss') || /moss[-_ ]?(tts|voicegen)/i.test(String(currentModel || '')));
  const currentIsVoiceDesign = currentLabels.includes('voice-design')
    || /voicegen/i.test(String(currentModel || ''));
  const openMossModels = useMemo(() => {
    const loadedNames = new Set(loadedModels.map(model => model.model_name.toLowerCase()));
    return knownModelInfos
      .map(info => {
        const name = String((info as any).model_name || info.name || info.id || '').trim();
        const recipe = String(
          (info as any).recipe
          || ((Array.isArray(info.recipes) && info.recipes[0]) ? (info.recipes[0] as any).recipe : ''),
        ).toLowerCase();
        const labels = (info.labels || []).map(label => String(label).toLowerCase());
        return { name, recipe, labels, downloaded: Boolean((info as any).downloaded) };
      })
      .filter(model => model.name
        && (model.recipe.includes('openmoss') || /moss[-_ ]?(tts|voicegen)/i.test(model.name))
        && (model.downloaded || loadedNames.has(model.name.toLowerCase()) || model.name === currentModel));
  }, [currentModel, knownModelInfos, loadedModels]);
  const openMossVoiceDesignModel = currentIsVoiceDesign && isOpenMossTts
    ? currentModel
    : (openMossModels.find(model => model.labels.includes('voice-design') || /voicegen/i.test(model.name))?.name || '');
  const openMossCloneModel = isOpenMossTts && !currentIsVoiceDesign
    ? currentModel
    : (openMossModels.find(model => !model.labels.includes('voice-design') && !/voicegen/i.test(model.name))?.name || '');
  const openMossDescribeUnavailable = isOpenMossTts
    && openMossSettings.mode === 'describe'
    && !openMossVoiceDesignModel;
  const openMossCloneUnavailable = isOpenMossTts
    && openMossSettings.mode === 'clone'
    && (!openMossCloneModel || pendingAudioFiles.length === 0);
  const imageGenerationModels = useMemo(() => {
    const names = new Set<string>();
    loadedModels.forEach(model => {
      const info = findModelInfoByName(knownModelInfos, model.model_name);
      const capability = info ? capabilityFromModelInfo(info) : capabilityFromLoaded(model);
      if (capability === 'image') names.add(model.model_name);
    });
    knownModelInfos.forEach(info => {
      if (capabilityFromModelInfo(info) !== 'image' || !(info as any).downloaded) return;
      const name = String((info as any).model_name || info.name || info.id || '').trim();
      if (name) names.add(name);
    });
    return Array.from(names).sort((a, b) => a.localeCompare(b));
  }, [knownModelInfos, loadedModels]);

  useEffect(() => {
    if (currentCapability !== 'model3d') return;
    setModel3dSettings(prev => ({
      ...prev,
      imageModel: prev.imageModel && imageGenerationModels.includes(prev.imageModel)
        ? prev.imageModel
        : (imageGenerationModels[0] || ''),
    }));
  }, [currentCapability, imageGenerationModels]);
  useEffect(() => {
    const updatePresetVersion = () => setPresetVersion(v => v + 1);
    window.addEventListener(PRESET_STORE_EVENT, updatePresetVersion);
    return () => window.removeEventListener(PRESET_STORE_EVENT, updatePresetVersion);
  }, []);

  useEffect(() => {
    const reloadTtsSettings = () => setTtsPlaybackSettings(loadTtsPlaybackSettings(storageScope));
    reloadTtsSettings();
    window.addEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
    return () => window.removeEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
  }, [storageScope]);

  const allPresets = useMemo(() => allStoredPresets(), [presetVersion]);
  const currentPreset = useMemo(() => currentModel ? activePresetForModel(currentModel) : null, [currentModel, presetVersion]);

  useEffect(() => {
    if (currentCapability !== 'audio-generation') return;
    const recipeOptions = currentPreset?.recipe_options || {};
    setAudioGenerationSettings(prev => ({
      ...prev,
      duration: isAceStepAudio ? 150 : 10,
      steps: typeof recipeOptions.steps === 'number' ? recipeOptions.steps : 50,
      cfg: typeof recipeOptions.cfg_scale === 'number' ? recipeOptions.cfg_scale : 4.5,
      lyrics: '',
    }));
  }, [currentModel, currentCapability, currentPreset, isAceStepAudio]);

  useEffect(() => {
    if (!isOpenMossTts) return;
    setOpenMossSettings({
      mode: 'plain',
      voiceDescription: String(currentPreset?.recipe_options?.voice || ''),
    });
    setPendingAudioFiles([]);
  }, [currentModel, currentPreset, isOpenMossTts]);

  useEffect(() => {
    const keepsAudioAttachments = currentCapability === 'audio'
      || currentCapability === 'omni'
      || modelSupportsChatAudioInput(currentKnownModelInfo, currentLoadedModel);
    if (keepsAudioAttachments) return;
    if (isOpenMossTts && openMossSettings.mode === 'clone') return;
    setPendingAudioFiles([]);
  }, [
    currentCapability,
    currentKnownModelInfo,
    currentLoadedModel,
    isOpenMossTts,
    openMossSettings.mode,
  ]);

  useEffect(() => {
    if (!currentModel || !currentPreset) return;
    const selectedIds = presetMcpServerIds(currentPreset);
    const next = selectedIds.length > 0;
    const seed = `${storageScope}:${currentModel}:${currentPreset.id}:${selectedIds.join(',')}:${next ? 'mcp-on' : 'mcp-off'}`;
    if (presetMcpSeedRef.current === seed) return;
    presetMcpSeedRef.current = seed;
    setUseMcp(next);
    try { localStorage.setItem(scopedKey(storageScope, MCP_ENABLED_KEY), String(next)); } catch { /* ignore */ }
  }, [currentModel, currentPreset, storageScope]);

  const hasRealtimeAudio = useMemo(
    () => !!currentModel && modelSupportsRealtimeAudio(currentModel, currentKnownModelInfo, currentLoadedModel),
    [currentModel, currentKnownModelInfo, currentLoadedModel],
  );
  const supportsRealtimeAudio = useMemo(
    () => canUseMicrophone() && hasRealtimeAudio,
    [hasRealtimeAudio],
  );
  const supportsChatAudioInput = useMemo(
    () => modelSupportsChatAudioInput(currentKnownModelInfo, currentLoadedModel),
    [currentKnownModelInfo, currentLoadedModel],
  );

  const defaultImageSettings = useMemo(
    () => imageDefaultsForModel(
      currentLoadedModel,
      currentKnownModelInfo,
      currentCapability === 'image' ? (currentPreset?.recipe_options as Record<string, unknown> | undefined) : undefined,
    ),
    [currentLoadedModel, currentKnownModelInfo, currentPreset, currentCapability],
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
  const audioInputForLoaded = useCallback((model: LoadedModel) => {
    const info = findModelInfoByName(knownModelInfos, model.model_name);
    return modelSupportsChatAudioInput(info, model);
  }, [knownModelInfos]);
  const selectableModels = useMemo(
    () => loadedModels.filter(m => canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(capabilityForLoaded(m))),
    [loadedModels, capabilityForLoaded],
  );
  type ModelPickerOption = {
    name: string;
    capability: ModelCapability;
    loaded: boolean;
    audioInput: boolean;
    info?: ModelInfo;
    detail: string;
  };

  const modelPickerOptions = useMemo<ModelPickerOption[]>(() => {
    const seen = new Set<string>();
    const options: ModelPickerOption[] = [];
    const addOption = (option: ModelPickerOption) => {
      const key = option.name.toLowerCase();
      if (!option.name || seen.has(key)) return;
      if (!['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(option.capability)) return;
      seen.add(key);
      options.push(option);
    };

    selectableModels.forEach(model => addOption({
      name: model.model_name,
      capability: capabilityForLoaded(model),
      loaded: true,
      audioInput: audioInputForLoaded(model),
      detail: `Loaded · ${model.recipe || 'runtime'}${model.device ? ` · ${model.device}` : ''}`,
    }));

    knownModelInfos.forEach(info => {
      const name = String((info as any).model_name || info.name || info.id || '').trim();
      const capability = capabilityFromModelInfo(info);
      const downloaded = Boolean((info as any).downloaded);
      if (!downloaded) return;
      addOption({
        name,
        capability,
        loaded: false,
        audioInput: modelSupportsChatAudioInput(info, null),
        info,
        detail: 'Downloaded · click to load',
      });
    });

    const q = modelPickerQuery.trim().toLowerCase();
    const filtered = q
      ? options.filter(option => `${option.name} ${modelModeLabel(option.capability, option.audioInput)} ${option.detail}`.toLowerCase().includes(q))
      : options;
    return filtered.slice(0, 80);
  }, [audioInputForLoaded, capabilityForLoaded, knownModelInfos, modelPickerQuery, selectableModels]);


  const presetPickerTarget = currentKnownModelInfo || currentCustomModelInfo || currentModel || null;
  const presetPickerOptions = useMemo(() => {
    if (!currentModel) return [];
    const q = presetPickerQuery.trim().toLowerCase();
    return allPresets
      .filter(preset => isCompatible(preset, presetPickerTarget))
      .filter(preset => {
        if (!q) return true;
        return [
          preset.name,
          preset.description,
          preset.applies_to.join(' '),
          systemPromptNameForPreset(preset),
          `mcp ${presetMcpDisplayText(preset)}`,
        ].join(' ').toLowerCase().includes(q);
      })
      .slice(0, 80);
  }, [allPresets, currentModel, presetPickerQuery, presetPickerTarget]);

  const handlePresetPickerSelect = useCallback(async (preset: Preset) => {
    if (!currentModel || presetPickerApplying) return;

    const targetName = currentModel;
    const previousApplied = loadApplied();
    const previouslyLinkedPreset = currentPreset || activePresetForModel(targetName);
    const runId = runningPresetIdForModel(targetName);
    const runningPreset = runId
      ? (allStoredPresets().find(p => p.id === runId) ?? previouslyLinkedPreset)
      : previouslyLinkedPreset;
    const changeKind = currentLoadedModel
      ? classifyPresetChange(runningPreset, preset)
      : 'none';

    const nextApplied = { ...previousApplied };
    if (preset.id === DEFAULT_PRESET.id) delete nextApplied[targetName];
    else nextApplied[targetName] = preset.id;

    setPresetPickerApplying(preset.id);
    setPresetPickerError(null);
    saveApplied(nextApplied);

    const nextMcp = presetMcpServerIds(preset).length > 0;
    setUseMcp(nextMcp);
    try { localStorage.setItem(scopedKey(storageScope, MCP_ENABLED_KEY), String(nextMcp)); } catch { /* ignore */ }

    // Choosing a preset from the Chat composer is an explicit user action, just
    // like choosing a different model. Apply the target state immediately: live
    // request-time changes take effect on the next request; load-time changes
    // trigger a reload so the running backend is actually using the selected
    // preset instead of merely linking it for later.
    try {
      if (currentCapability === 'image') {
        imageSettingsTouchedRef.current = false;
        imageSettingsCommittedRef.current = false;
        setImageSettings(imageDefaultsForModel(
          currentLoadedModel,
          currentKnownModelInfo,
          preset.recipe_options as Record<string, unknown> | undefined,
        ));
        setImageMode('generate');
      }

      if (currentLoadedModel && changeKind === 'reload') {
        await api.reloadModel(
          targetName,
          Object.keys(preset.recipe_options || {}).length > 0
            ? preset.recipe_options as Record<string, unknown>
            : undefined,
          currentKnownModelInfo || currentCustomModelInfo || null,
        );
        await Promise.resolve(onRefresh());
      }

      if (currentLoadedModel) setRunningPreset(targetName, preset.id);
      setPresetPickerOpen(false);
      setPresetPickerQuery('');
    } catch (err) {
      setPresetPickerOpen(true);
      setPresetPickerError(friendlyErrorMessage(err));
    } finally {
      setPresetPickerApplying(null);
    }
  }, [
    currentCapability,
    currentCustomModelInfo,
    currentKnownModelInfo,
    currentLoadedModel,
    currentModel,
    currentPreset,
    onRefresh,
    presetPickerApplying,
    storageScope,
  ]);

  const modeSupportsChatCompletions = currentCapability === 'chat' || currentCapability === 'omni';
  const modeSupportsMcp = modeSupportsChatCompletions;
  const canUseAudioInput = currentCapability === 'omni' || currentCapability === 'audio' || (currentCapability === 'chat' && supportsChatAudioInput);

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
    if (!presetPickerOpen) return;
    const onPointerDown = (event: PointerEvent) => {
      const root = presetPickerRef.current;
      if (!root || root.contains(event.target as Node)) return;
      setPresetPickerOpen(false);
    };
    window.addEventListener('pointerdown', onPointerDown);
    return () => window.removeEventListener('pointerdown', onPointerDown);
  }, [presetPickerOpen]);

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

  const stopAutoSpeech = useCallback(() => {
    const current = autoSpeechRef.current;
    if (!current) return;
    current.audio.pause();
    current.audio.src = '';
    URL.revokeObjectURL(current.url);
    autoSpeechRef.current = null;
  }, []);

  useEffect(() => () => stopAutoSpeech(), [stopAutoSpeech]);

  const speakWithPinnedTts = useCallback(async (text: string, source: 'assistant' | 'user', force = false) => {
    const trimmed = text.trim();
    const modelName = ttsPlaybackSettings.modelName;
    if (!trimmed || !modelName) return;
    if (!force) {
      if (ttsPlaybackSettings.playbackMode !== 'always') return;
      if (source === 'user' && !ttsPlaybackSettings.speakUserText) return;
    }
    try {
      const isLoaded = loadedModels.some(model => model.model_name.toLowerCase() === modelName.toLowerCase());
      if (!isLoaded) {
        await api.loadModel(modelName, undefined, findModelInfoByName(knownModelInfos, modelName) || null);
      }
      const modelInfo = findModelInfoByName(knownModelInfos, modelName);
      const modelRecipe = String(
        (modelInfo as any)?.recipe
        || ((Array.isArray(modelInfo?.recipes) && modelInfo?.recipes?.[0]) ? (modelInfo.recipes[0] as any).recipe : ''),
      ).toLowerCase();
      const presetOptions = activePresetForModel(modelName).recipe_options;
      const voice = modelRecipe.includes('openmoss')
        ? String(presetOptions.voice || '')
        : ttsVoiceFromRecipeOptions(presetOptions);
      const audio = await api.textToSpeech(modelName, trimmed, voice);
      stopAutoSpeech();
      const player = new Audio(audio.url);
      autoSpeechRef.current = { audio: player, url: audio.url };
      player.onended = () => {
        if (autoSpeechRef.current?.url === audio.url) {
          URL.revokeObjectURL(audio.url);
          autoSpeechRef.current = null;
        }
      };
      await player.play();
    } catch (err) {
      console.warn(`Could not play ${source} text with TTS model:`, err);
    }
  }, [knownModelInfos, loadedModels, presetVersion, stopAutoSpeech, ttsPlaybackSettings.modelName, ttsPlaybackSettings.playbackMode, ttsPlaybackSettings.speakUserText]);

  // Streaming hook — owns token buffer, flush interval, abort controllers
  const handleStreamDone = useCallback((convoId: string, stats: ChatCompletionStats, toolCalls?: ToolCallEntry[]) => {
    const model = streamModelsRef.current[convoId] || null;
    delete streamModelsRef.current[convoId];
    const artifacts = collectToolArtifacts(toolCalls);
    const generatedImages = artifacts.filter(a => a.type === 'image').map(a => a.url);
    const generatedAudio = artifacts.find(a => a.type === 'audio');
    const generated3d = artifacts.find(a => a.type === 'model3d');
    const generatedAudioUrl = generatedAudio?.url ? trackGeneratedMediaUrl(generatedAudio.url) : undefined;
    const generated3dUrl = generated3d?.url ? trackGeneratedMediaUrl(generated3d.url) : undefined;
    const mediaFallback = generated3d
      ? 'Generated a 3D model from the reference image.'
      : generatedImages.length > 0
        ? `Generated ${generatedImages.length} image${generatedImages.length === 1 ? '' : 's'} from your prompt.`
        : generatedAudio
          ? 'Generated speech audio from your text.'
          : '';
    const assistantContent = stats.content || mediaFallback || summarizeToolOnlyResponse(toolCalls);
    updateConversation(convoId, c => ({
      ...c,
      messages: [...c.messages, {
        role: 'assistant',
        content: assistantContent,
        thinking: stats.reasoning || undefined,
        toolCalls,
        stats,
        model,
        generatedImages: generatedImages.length > 0 ? generatedImages : undefined,
        audioUrl: generatedAudioUrl,
        audioName: generatedAudio?.name,
        model3dUrl: generated3dUrl,
        model3dName: generated3d?.name,
      }],
      updatedAt: Date.now(),
    }));
    if (!generatedAudio && !generated3d && !generatedImages.length) void speakWithPinnedTts(assistantContent, 'assistant');
  }, [speakWithPinnedTts, trackGeneratedMediaUrl, updateConversation]);

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

  useFocusTrap(bottomSheetRef, mobileSheetOpen);

  useEffect(() => {
    if (isStreaming) {
      if (!wasStreamingRef.current) {
        setStreamStatus('Assistant is responding');
        liveBufferRef.current = '';
        setLiveText('');
      }
      wasStreamingRef.current = true;
      return;
    }

    if (!wasStreamingRef.current) return;

    if (liveTimerRef.current) {
      clearTimeout(liveTimerRef.current);
      liveTimerRef.current = null;
    }

    setStreamStatus('Response complete');
    wasStreamingRef.current = false;
  }, [isStreaming]);

  useEffect(() => {
    if (!isStreaming) {
      if (liveBufferRef.current.trim()) {
        setLiveText(liveBufferRef.current);
        liveBufferRef.current = '';
      }
      return;
    }

    liveBufferRef.current = streamingContent;
    if (liveTimerRef.current) clearTimeout(liveTimerRef.current);
    const hasBoundary = /[.!?\n]/.test(streamingContent.slice(-2));
    const delay = hasBoundary ? 100 : 400;
    liveTimerRef.current = setTimeout(() => {
      setLiveText(streamingContent);
    }, delay);
  }, [streamingContent, isStreaming]);

  useEffect(() => {
    return () => {
      if (liveTimerRef.current) clearTimeout(liveTimerRef.current);
    };
  }, []);

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

  const handleRailKeyDown = useCallback((e: React.KeyboardEvent<HTMLUListElement>) => {
    const list = e.currentTarget;
    const options = Array.from(list.querySelectorAll<HTMLElement>('[role="option"]'));
    if (!options.length) return;
    const currentIdx = options.findIndex(el =>
      el === document.activeElement || el.contains(document.activeElement as Node),
    );
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      const next = currentIdx < 0 ? 0 : (currentIdx + 1) % options.length;
      options[next].focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      const prev = currentIdx < 0 ? options.length - 1 : (currentIdx - 1 + options.length) % options.length;
      options[prev].focus();
    } else if (e.key === 'Home') {
      e.preventDefault();
      options[0].focus();
    } else if (e.key === 'End') {
      e.preventDefault();
      options[options.length - 1].focus();
    } else if ((e.key === 'Enter' || e.key === ' ') && currentIdx >= 0) {
      if ((e.target as HTMLElement).tagName !== 'BUTTON') {
        e.preventDefault();
        handleSelectConversation(conversations[currentIdx].id);
      }
    }
  }, [conversations, handleSelectConversation]);


  const handleRailToggle = useCallback(() => {
    if (window.innerWidth <= 480) {
      setMobileSheetOpen(prev => !prev);
    } else {
      setRailExpanded(prev => !prev);
    }
  }, []);

  const closeMobileSheet = useCallback(() => {
    setMobileSheetOpen(false);
    sheetTriggerRef.current?.focus();
  }, []);

  const handleSheetKeyDown = useCallback((e: React.KeyboardEvent<HTMLUListElement>) => {
    const list = e.currentTarget;
    const options = Array.from(list.querySelectorAll<HTMLElement>('[role="option"]'));
    if (!options.length) return;
    const currentIdx = options.findIndex(el =>
      el === document.activeElement || el.contains(document.activeElement as Node),
    );
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      const next = currentIdx < 0 ? 0 : (currentIdx + 1) % options.length;
      options[next].focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      const prev = currentIdx < 0 ? options.length - 1 : (currentIdx - 1 + options.length) % options.length;
      options[prev].focus();
    } else if (e.key === 'Home') {
      e.preventDefault();
      options[0].focus();
    } else if (e.key === 'End') {
      e.preventDefault();
      options[options.length - 1].focus();
    } else if ((e.key === 'Enter' || e.key === ' ') && currentIdx >= 0) {
      if ((e.target as HTMLElement).tagName !== 'BUTTON') {
        e.preventDefault();
        handleSelectConversation(conversations[currentIdx].id);
        closeMobileSheet();
      }
    }
  }, [conversations, handleSelectConversation, closeMobileSheet]);

  // ESC closes mobile sheet
  useEffect(() => {
    if (!mobileSheetOpen) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') { closeMobileSheet(); }
    };
    document.addEventListener('keydown', handler);
    return () => document.removeEventListener('keydown', handler);
  }, [mobileSheetOpen, closeMobileSheet]);

  // Drag-to-close on the sheet handle
  useEffect(() => {
    if (!mobileSheetOpen) return;
    const handle = sheetHandleRef.current;
    if (!handle) return;
    let startY = 0;
    let deltaY = 0;
    let dragging = false;
    const sheetEl = handle.closest('.bottom-sheet') as HTMLElement | null;

    const onDown = (e: PointerEvent) => {
      dragging = true;
      startY = e.clientY;
      deltaY = 0;
      handle.setPointerCapture(e.pointerId);
    };
    const onMove = (e: PointerEvent) => {
      if (!dragging) return;
      deltaY = Math.max(0, e.clientY - startY);
      if (sheetEl) sheetEl.style.transform = `translateY(${deltaY}px)`;
    };
    const onUp = (e: PointerEvent) => {
      if (!dragging) return;
      dragging = false;
      handle.releasePointerCapture(e.pointerId);
      if (deltaY > 100) {
        closeMobileSheet();
      }
      if (sheetEl) sheetEl.style.transform = '';
    };
    handle.addEventListener('pointerdown', onDown);
    handle.addEventListener('pointermove', onMove);
    handle.addEventListener('pointerup', onUp);
    return () => {
      handle.removeEventListener('pointerdown', onDown);
      handle.removeEventListener('pointermove', onMove);
      handle.removeEventListener('pointerup', onUp);
    };
  }, [mobileSheetOpen, closeMobileSheet]);
  // --- End mobile bottom sheet logic ---

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
      } else if (model.capability === 'audio-generation') {
        if (!text) throw new Error('Audio generation needs a prompt.');
        const isAceStepModel = String(model.recipe || '').toLowerCase().includes('acestep')
          || /ace[-_ ]?step/.test(String(model.name || '').toLowerCase());
        const audioOptions: Record<string, unknown> = {
          duration: audioGenerationSettings.duration,
          steps: audioGenerationSettings.steps,
          seed: audioGenerationSettings.seed === '' ? -1 : audioGenerationSettings.seed,
        };
        if (isAceStepModel) {
          const lyrics = audioGenerationSettings.lyrics.trim();
          if (lyrics) {
            audioOptions.lyrics = lyrics;
            audioOptions.vocal_language = audioGenerationSettings.vocalLanguage.trim() || 'en';
          }
        } else {
          audioOptions.cfg = audioGenerationSettings.cfg;
        }
        const audio = await api.audioGeneration(model.name, text, audioOptions);
        appendAssistantMessage(convoId, {
          content: isAceStepModel
            ? `Generated ${audioGenerationSettings.lyrics.trim() ? 'a vocal track' : 'an instrumental track'} from your prompt.`
            : 'Generated a sound effect from your prompt.',
          audioUrl: trackGeneratedMediaUrl(audio.url),
          audioName: audio.filename,
          model,
        });
      } else if (model.capability === 'model3d') {
        let referenceImage = images[0] || '';
        let generatedReference: string[] | undefined;
        if (model3dSettings.sourceMode === 'text') {
          if (!text) throw new Error('Text-to-3D needs an object description.');
          if (!model3dSettings.imageModel) throw new Error('Choose a downloaded image model for the text-to-3D reference step.');
          const imageInfo = findModelInfoByName(knownModelInfos, model3dSettings.imageModel) || null;
          if (!loadedModels.some(item => item.model_name.toLowerCase() === model3dSettings.imageModel.toLowerCase())) {
            await api.loadModel(model3dSettings.imageModel, undefined, imageInfo);
          }
          const references = await api.imageGeneration(
            model3dSettings.imageModel,
            `${text.trim()} -- ${MODEL3D_REFERENCE_PROMPT}`,
            { n: 1, size: '1024x1024' },
          );
          referenceImage = references[0];
          generatedReference = [referenceImage];
          await api.loadModel(model.name, undefined, findModelInfoByName(knownModelInfos, model.name) || null);
        } else if (!referenceImage) {
          throw new Error('Image-to-3D needs one reference image.');
        }
        const result = await api.model3dGeneration(model.name, referenceImage, {
          resolution: model3dSettings.resolution,
          bg_removal: model3dSettings.backgroundRemoval,
          seed: model3dSettings.seed === '' ? -1 : model3dSettings.seed,
        });
        appendAssistantMessage(convoId, {
          content: model3dSettings.sourceMode === 'text'
            ? 'Rendered a reference image and reconstructed it as a textured 3D model.'
            : 'Reconstructed the reference image as a textured 3D model.',
          generatedImages: generatedReference,
          model3dUrl: trackGeneratedMediaUrl(result.url),
          model3dName: result.filename,
          model,
        });
      } else if (model.capability === 'tts') {
        if (!text) throw new Error('TTS mode needs text to speak.');
        let targetModel = model.name;
        let voice = ttsVoiceFromRecipeOptions(activePresetForModel(model.name).recipe_options);
        let speechOptions: Record<string, unknown> = {};
        let content = 'Generated speech audio from your text.';
        let reloadTargetAfterVoiceDesign = false;

        if (isOpenMossTts) {
          voice = openMossSettings.voiceDescription.trim();
          if (openMossSettings.mode === 'describe') {
            if (!openMossVoiceDesignModel) {
              throw new Error('Install MOSS-VoiceGen to design a voice from a description.');
            }
            targetModel = openMossVoiceDesignModel;
            if (openMossCloneModel) {
              if (!loadedModels.some(item => item.model_name.toLowerCase() === openMossVoiceDesignModel.toLowerCase())) {
                await api.loadModel(
                  openMossVoiceDesignModel,
                  undefined,
                  findModelInfoByName(knownModelInfos, openMossVoiceDesignModel) || null,
                );
              }
              const designedSample = await api.textToSpeech(
                openMossVoiceDesignModel,
                OPENMOSS_VOICE_DESIGN_PHRASE,
                voice,
              );
              try {
                speechOptions.reference_wav_b64 = await fileToBase64(designedSample.blob);
              } finally {
                URL.revokeObjectURL(designedSample.url);
              }
              targetModel = openMossCloneModel;
              voice = '';
              reloadTargetAfterVoiceDesign = true;
              content = 'Designed a voice from your description and generated speech with it.';
            } else {
              content = 'Generated speech with the described voice.';
            }
          } else if (openMossSettings.mode === 'clone') {
            const sample = audioFiles[0];
            if (!sample) throw new Error('Attach a WAV voice sample to clone.');
            if (!openMossCloneModel) throw new Error('Install OpenMOSS-TTS to clone a voice sample.');
            targetModel = openMossCloneModel;
            speechOptions.reference_wav_b64 = await wavVoiceSampleToBase64(sample);
            content = 'Generated speech using the attached voice sample.';
          }

          if (reloadTargetAfterVoiceDesign || !loadedModels.some(item => item.model_name.toLowerCase() === targetModel.toLowerCase())) {
            await api.loadModel(targetModel, undefined, findModelInfoByName(knownModelInfos, targetModel) || null);
          }
        }

        const audio = await api.textToSpeech(targetModel, text, voice, speechOptions);
        const targetInfo = findModelInfoByName(knownModelInfos, targetModel);
        const outputModel = targetModel === model.name
          ? model
          : (snapshotFromModelInfo(targetInfo) || { ...model, name: targetModel });
        appendAssistantMessage(convoId, {
          content,
          audioUrl: trackGeneratedMediaUrl(audio.url),
          audioName: `${targetModel}.wav`,
          model: outputModel,
        });
      } else if (model.capability === 'audio') {
        const file = audioFiles[0];
        if (!file) throw new Error('Audio mode needs an audio file to transcribe.');
        const transcript = await api.audioTranscription(model.name, file);
        appendAssistantMessage(convoId, {
          content: transcript,
          model,
        });
        void speakWithPinnedTts(transcript, 'assistant');
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
  }, [
    appendAssistantMessage, audioGenerationSettings, imageMode, imageSettings,
    isOpenMossTts, knownModelInfos, loadedModels, model3dSettings, onRefresh,
    openMossCloneModel, openMossSettings, openMossVoiceDesignModel,
    presetVersion, speakWithPinnedTts, trackGeneratedMediaUrl,
  ]);

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
      void speakWithPinnedTts(text, 'user');
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

    let selectedMcpRuntime: ChatToolRuntime | null = null;
    if (useMcp && modeSupportsMcp) {
      try {
        selectedMcpRuntime = await buildSelectedMcpRuntime(
          presetMcpServerIds(currentPreset || DEFAULT_PRESET),
          {
            attachedImages: images || [],
            attachedAudioFiles: audioFiles,
            previousImages: collectConversationImages(priorMessages),
          },
        );
      } catch (err) {
        // MCP availability must never dead-end the chat composer. Surface the
        // failure, switch MCP off for this chat, and continue the same request
        // as a normal model completion without tools.
        setUseMcp(false);
        try { localStorage.setItem(scopedKey(storageScope, MCP_ENABLED_KEY), 'false'); } catch { /* ignore */ }
        appendAssistantMessage(convoId, {
          content: friendlyChatError(`MCP setup failed and was switched off for this chat. Continuing without tools: ${friendlyErrorMessage(err)}`),
          model: modelSnapshot,
          isError: true,
        });
        selectedMcpRuntime = null;
      }
    }

    const toolRuntime = composeMcpRuntimes([omniRuntime, selectedMcpRuntime]);

    // Build chat history from the conversation's messages before this user prompt.
    // Do not feed prior friendly UI error messages or generated media artifacts back as assistant context.
    const chatMessages: ChatMessage[] = [];

    const systemPrompts: string[] = [];
    const presetSystemPrompt = systemPromptTextForPreset(currentPreset);
    if (presetSystemPrompt) systemPrompts.push(presetSystemPrompt);
    if (toolRuntime?.systemPrompt) systemPrompts.push(toolRuntime.systemPrompt);

    if (systemPrompts.length > 0) {
      chatMessages.push({ role: 'system' as const, content: systemPrompts.join('\n\n') });
    }

    const historyMessages = priorMessages.filter(m => {
      if (m.role === 'assistant' && !isPersistableAssistantMessage(m)) return false;
      if (m.generatedImages?.length || m.audioUrl || m.model3dUrl) return false;
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
    currentPreset,
    knownModelInfos,
    loadedModels,
    modeSupportsChatCompletions,
    modeSupportsMcp,
    canUseAudioInput,
    runCapabilityRequest,
    speakWithPinnedTts,
    streaming,
    storageScope,
    updateConversation,
    useMcp,
  ]);

  const handleSend = async (overrideText?: string) => {
    const text = (overrideText ?? inputValue).trim();
    const audioFiles = [...pendingAudioFiles];
    const hasImages = pendingImages.length > 0;
    const canSubmitContent = currentCapability === 'audio' && !modeSupportsChatCompletions
      ? audioFiles.length > 0
      : currentCapability === 'image'
        ? (imageMode === 'edit' ? (!!text && hasImages) : !!text)
        : currentCapability === 'audio-generation'
          ? !!text
          : currentCapability === 'model3d'
            ? (model3dSettings.sourceMode === 'image' ? hasImages : (!!text && !!model3dSettings.imageModel))
            : currentCapability === 'tts'
              ? (!!text && !openMossDescribeUnavailable && !openMossCloneUnavailable)
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
    if (originalUserMessage.audioName) {
      appendAssistantMessage(activeId, {
        content: friendlyChatError('Retrying a request with an audio attachment needs the original file. Please attach it again.'),
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

  const handleSpeakAssistantMessage = useCallback((text: string) => {
    void speakWithPinnedTts(text, 'assistant', true);
  }, [speakWithPinnedTts]);

  const canReadAssistantMessages = Boolean(ttsPlaybackSettings.modelName);

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
    if (isOpenMossTts && openMossSettings.mode === 'clone') {
      const wav = files.find(file => file.type.toLowerCase().includes('wav') || file.name.toLowerCase().endsWith('.wav'));
      if (wav) setPendingAudioFiles([wav]);
      return;
    }

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
    if (currentCapability === 'model3d' && model3dSettings.sourceMode !== 'image') return;

    if (currentCapability === 'model3d') {
      const source = imageFiles.find(file => {
        const mime = file.type.toLowerCase();
        const name = file.name.toLowerCase();
        return ['image/png', 'image/jpeg', 'image/bmp', 'image/gif'].includes(mime)
          || /\.(png|jpe?g|bmp|gif)$/.test(name);
      });
      if (!source) return;
      // TRELLIS accepts these source formats directly. Preserve alpha and the
      // original pixels instead of routing the reference through the generic
      // chat attachment JPEG compressor.
      setPendingImages([await blobToDataUrl(source)]);
      return;
    }

    if (currentCapability === 'image' && imageMode === 'edit') {
      const encoded = await imageToBase64(imageFiles[0]);
      setPendingImages([encoded]);
      return;
    }

    const remaining = MAX_IMAGES - pendingImages.length;
    const toProcess = imageFiles.slice(0, remaining);
    const encoded = await Promise.all(toProcess.map(imageToBase64));
    setPendingImages(prev => [...prev, ...encoded].slice(0, MAX_IMAGES));
  }, [
    canUseAudioInput, currentCapability, imageMode, isOpenMossTts,
    modeSupportsChatCompletions, model3dSettings.sourceMode,
    openMossSettings.mode, pendingImages.length,
  ]);

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

  const handleModelPickerUnload = useCallback(async (modelName: string, e: React.MouseEvent) => {
    e.stopPropagation();
    if (modelPickerUnloading) return;
    setModelPickerUnloading(modelName);
    try {
      await api.unloadModel(modelName);
      await Promise.resolve(onRefresh());
      setUnloadAnnouncement(`${modelName} unloaded`);
    } finally {
      setModelPickerUnloading(null);
    }
  }, [modelPickerUnloading, onRefresh]);

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
  const isOpenMossCloneMode = isOpenMossTts && openMossSettings.mode === 'clone';
  const canAttach = currentCapability === 'chat'
    || currentCapability === 'omni'
    || currentCapability === 'audio'
    || supportsRealtimeAudio
    || isOpenMossCloneMode
    || (currentCapability === 'image' && imageMode === 'edit')
    || (currentCapability === 'model3d' && model3dSettings.sourceMode === 'image');
  const fileAccept = isOpenMossCloneMode
    ? 'audio/wav,audio/x-wav,.wav'
    : currentCapability === 'model3d'
      ? 'image/png,image/jpeg,image/bmp,image/gif,.png,.jpg,.jpeg,.bmp,.gif'
      : currentCapability === 'image'
        ? 'image/*'
        : canUseAudioInput
          ? 'image/*,audio/*'
          : 'image/*';
  const canSubmit = !!currentModel && !isBusy && (currentCapability === 'audio' && !modeSupportsChatCompletions
    ? pendingAudioFiles.length > 0
    : currentCapability === 'image'
      ? (imageMode === 'edit' ? (!!inputValue.trim() && pendingImages.length > 0) : !!inputValue.trim())
      : currentCapability === 'audio-generation'
        ? !!inputValue.trim()
        : currentCapability === 'model3d'
          ? (model3dSettings.sourceMode === 'image' ? pendingImages.length > 0 : (!!inputValue.trim() && !!model3dSettings.imageModel))
          : currentCapability === 'tts'
            ? (!!inputValue.trim() && !openMossDescribeUnavailable && !openMossCloneUnavailable)
            : (!!inputValue.trim() || pendingImages.length > 0 || (canUseAudioInput && pendingAudioFiles.length > 0)));
  const composerPlaceholder = !currentModel
    ? 'Draft a message — connect and load a model to send…'
    : currentCapability === 'omni'
      ? `Message ${currentModel} through the Omni collection…`
      : currentCapability === 'chat' && supportsChatAudioInput
        ? `Message ${currentModel} with text, images, or audio…`
      : currentCapability === 'image'
      ? (imageMode === 'edit' ? `Describe the edit for ${currentModel}…` : `Describe an image for ${currentModel}…`)
      : currentCapability === 'audio'
        ? `Attach audio or use the mic with ${currentModel}…`
        : currentCapability === 'audio-generation'
          ? (isAceStepAudio ? 'Describe the music style, mood, tempo, instruments, and voice…' : 'Describe the sound effect to generate…')
          : currentCapability === 'model3d'
            ? (model3dSettings.sourceMode === 'image' ? 'Attach a reference image for 3D reconstruction…' : 'Describe the object to render and reconstruct in 3D…')
            : currentCapability === 'tts'
              ? (isOpenMossCloneMode ? 'Type text to speak, then attach a WAV voice sample…' : `Text to speak with ${currentModel}…`)
              : `Message ${currentModel}…`;
  const composerHint = supportsChatAudioInput && modeSupportsChatCompletions
    ? (supportsRealtimeAudio
      ? 'Chat + audio mode · mic transcribes into the draft, and audio files are routed through chat completions'
      : 'Chat + audio mode · attached audio is routed through chat completions')
    : currentCapability === 'omni'
    ? 'Omni collection mode · requests are orchestrated across collection components'
    : currentCapability === 'image'
      ? (imageMode === 'edit' ? 'Image mode · attach one source image and prompt becomes /images/edits' : 'Image mode · prompt becomes /images/generations')
    : currentCapability === 'audio'
      ? 'Transcription mode · attach a file for /audio/transcriptions or use live mic via /v1/realtime'
      : currentCapability === 'audio-generation'
        ? (isAceStepAudio ? 'Music mode · instrumental or optional structured lyrics via /audio/generations' : 'Sound mode · prompt becomes /audio/generations')
        : currentCapability === 'model3d'
          ? (model3dSettings.sourceMode === 'image' ? '3D mode · image becomes /3d/generations · export GLB or geometry-only STL' : '3D mode · image model renders a reference, then TRELLIS reconstructs it')
          : currentCapability === 'tts'
            ? (isOpenMossTts
              ? openMossSettings.mode === 'describe'
                ? 'OpenMOSS · describe a voice; MOSS-VoiceGen creates a reference for speech synthesis'
                : openMossSettings.mode === 'clone'
                  ? 'OpenMOSS · attach one WAV sample to clone its voice'
                  : 'OpenMOSS · optional voice style instruction via /audio/speech'
              : 'TTS mode · text becomes /audio/speech')
            : 'Enter to send · Shift+Enter for newline · Paste or drop images';

  const upscalingModels = useMemo(
    () => knownModelInfos
      .filter(info => Array.isArray(info.labels) && info.labels.includes('upscaling'))
      .map(info => String(info.name || info.id))
      .filter(Boolean),
    [knownModelInfos],
  );

  return (
    <>
      <div
        className={`chat ${railExpanded ? 'rail-expanded' : ''}${showInlineLogs ? ' chat--with-logs' : ''}`}
        style={showInlineLogs ? chatLayoutStyle : undefined}
      >
      {/* Conversation rail */}
      <aside className="rail">
        <div className="rail__head">
          <button
            className="rail__toggle"
            onClick={handleRailToggle}
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

        <ul className="rail__list" role="listbox" aria-label="Conversations" onKeyDown={handleRailKeyDown}>
          {conversations.map((c, idx) => {
            const badge = capabilityBadge(c.model?.capability || 'chat');
            const isSelected = c.id === activeId;
            const isTabTarget = isSelected || (idx === 0 && !activeId);
            const convTitle = c.title || deriveTitle(c.messages);
            return (
              <li
                id={`rail-conv-${c.id}`}
                className={`rail__item ${isSelected ? 'is-active' : ''}`}
                key={c.id}
                role="option"
                aria-selected={isSelected}
                tabIndex={isTabTarget ? 0 : -1}
                onClick={() => handleSelectConversation(c.id)}
              >
                <span className="rail__item-title">
                  {convTitle}
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
                  aria-label={`Delete conversation: ${convTitle}`}
                  title="Delete"
                  tabIndex={-1}
                >×</button>
              </li>
            );
          })}
        </ul>
        {conversations.length === 0 && (
          <p className="rail__empty">No conversations yet</p>
        )}

        <div className="rail__privacy">
          <label className="rail__privacy-toggle">
            <input type="checkbox" checked={persistHistory} onChange={handlePersistenceToggle} />
            <span>{accountSession.isGuest ? 'Shared guest history' : 'Private local history'} {persistHistory ? 'ON' : 'OFF'}</span>
          </label>
          <span className="rail__privacy-note">{describeSession(accountSession)} · Media is never persisted.</span>
        </div>
      </aside>

      {/* Mobile bottom sheet for conversations */}
      {mobileSheetOpen && (
        <div className="bottom-sheet-backdrop" onClick={closeMobileSheet} aria-hidden="true" />
      )}
      <div
        ref={bottomSheetRef}
        className={`bottom-sheet ${mobileSheetOpen ? 'bottom-sheet--open' : ''}`}
        role="dialog"
        aria-label="Conversations"
        aria-modal="true"
      >
        <div className="bottom-sheet__handle" ref={sheetHandleRef} aria-hidden="true">
          <div className="bottom-sheet__handle-pill" />
        </div>
        <button className="bottom-sheet__new" onClick={() => { handleNewChat(); closeMobileSheet(); }}>
          <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" aria-hidden="true">
            <line x1="7" y1="2.5" x2="7" y2="11.5" />
            <line x1="2.5" y1="7" x2="11.5" y2="7" />
          </svg>
          New Chat
        </button>
        <ul className="bottom-sheet__list rail__list" role="listbox" aria-label="Conversations" onKeyDown={handleSheetKeyDown}>
          {conversations.map((c, idx) => {
            const badge = capabilityBadge(c.model?.capability || 'chat');
            const isSelected = c.id === activeId;
            const isTabTarget = isSelected || (idx === 0 && !activeId);
            const convTitle = c.title || deriveTitle(c.messages);
            return (
              <li
                id={`sheet-conv-${c.id}`}
                className={`rail__item ${isSelected ? 'is-active' : ''}`}
                key={c.id}
                role="option"
                aria-selected={isSelected}
                tabIndex={isTabTarget ? 0 : -1}
                onClick={() => { handleSelectConversation(c.id); closeMobileSheet(); }}
              >
                <span className="rail__item-title">
                  {convTitle}
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
                  aria-label={`Delete conversation: ${convTitle}`}
                  title="Delete"
                  tabIndex={-1}
                >×</button>
              </li>
            );
          })}
        </ul>
        {conversations.length === 0 && (
          <p className="rail__empty">No conversations yet</p>
        )}
      </div>

      {/* Main pane */}
      <div className="chat__main" ref={threadRef}>
        {/* Mobile-only conversations trigger */}
        <button
          className="chat__mobile-rail-trigger"
          ref={sheetTriggerRef}
          onClick={() => setMobileSheetOpen(true)}
          aria-label="Open conversations"
          title="Conversations"
        >
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" aria-hidden="true">
            <line x1="3" y1="4" x2="13" y2="4" />
            <line x1="3" y1="8" x2="13" y2="8" />
            <line x1="3" y1="12" x2="13" y2="12" />
          </svg>
          <span>Conversations</span>
        </button>
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
                  onSpeak={canReadAssistantMessages && msg.role === 'assistant' && !msg.isError && msg.content ? () => handleSpeakAssistantMessage(msg.content) : undefined}
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
          <div
            className="chat__logs-resizer"
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize logs panel"
            aria-valuemin={CHAT_LOGS_MIN_WIDTH}
            aria-valuemax={CHAT_LOGS_MAX_WIDTH}
            aria-valuenow={chatLogsWidth}
            tabIndex={0}
            onPointerDown={handleChatLogsResizeStart}
            onKeyDown={handleChatLogsResizeKeyDown}
          />
          <LogViewer />
        </aside>
      )}

      {/* Composer */}
      <div aria-live="polite" aria-atomic="true" className="sr-only">{unloadAnnouncement}</div>
      <div className="composer" onDrop={handleDrop} onDragOver={handleDragOver}>
        <div className="composer__toolbar">
          {(modelPickerOptions.length > 0 || modelPickerOpen) && (
            <div className="composer__model-picker" ref={modelPickerRef}>
              <span className="composer__model-label">Model</span>
              <button
                type="button"
                className="composer__model-button"
                onClick={() => { setModelPickerOpen(v => !v); setPresetPickerOpen(false); setModelPickerError(null); }}
                aria-haspopup="listbox"
                aria-expanded={modelPickerOpen}
              >
                <ModelModeIcons capability={currentCapability} audioInput={supportsChatAudioInput} size={14} />
                <span className="composer__model-button-name">{currentModel || 'Choose model'}</span>
                {selectableModels.length > 0 && (
                  <span className="composer__model-button-badge">({selectableModels.length})</span>
                )}
                <span className="composer__model-button-caret">▾</span>
              </button>
              {modelPickerOpen && (
                <div className="composer__model-menu" role="dialog" aria-label="Search models">
                  <label className="composer__model-search">
                    <Icon name="search" size={14} />
                    <input
                      autoFocus
                      value={modelPickerQuery}
                      placeholder="Search downloaded models…"
                      onChange={e => setModelPickerQuery(e.target.value)}
                    />
                  </label>
                  <div className="composer__model-results" role="listbox">
                    {modelPickerOptions.map(option => (
                      <div
                        key={option.name}
                        className={`composer__model-option-row${option.name === currentModel ? ' is-active' : ''}${modelPickerUnloading === option.name ? ' is-unloading' : ''}`}
                      >
                        <button
                          type="button"
                          className="composer__model-option"
                          onClick={() => handleModelPickerSelect(option)}
                          disabled={!!modelPickerLoading || modelPickerUnloading === option.name}
                          role="option"
                          aria-selected={option.name === currentModel}
                        >
                          <ModelModeIcons capability={option.capability} audioInput={option.audioInput} size={15} />
                          <span className="composer__model-option-text">
                            <strong>{option.name}</strong>
                            <span>{modelModeLabel(option.capability, option.audioInput)} · {option.detail}</span>
                          </span>
                          {modelPickerLoading === option.name && <span className="composer__model-option-loading">Loading…</span>}
                        </button>
                        {option.loaded && (
                          <button
                            type="button"
                            className="composer__model-option-unload"
                            onClick={(e) => handleModelPickerUnload(option.name, e)}
                            disabled={!!modelPickerUnloading}
                            aria-label={`Unload ${option.name}`}
                          >
                            {modelPickerUnloading === option.name ? '…' : '×'}
                          </button>
                        )}
                      </div>
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
            onClick={() => { setModelPickerOpen(true); setPresetPickerOpen(false); }}
            title="Mode follows the selected model. Open model search to change it."
          >
            <ModelModeIcons capability={currentCapability} audioInput={supportsChatAudioInput} size={13} /> {modelModeLabel(currentCapability, supportsChatAudioInput)} mode
          </button>
          {currentPreset && currentModel && (
            <div className="composer__preset-picker" ref={presetPickerRef}>
              <button
                type="button"
                className="composer__preset-badge composer__preset-badge--interactive"
                onClick={() => { setPresetPickerOpen(v => !v); setModelPickerOpen(false); }}
                title={`Active preset for this model. Prompt: ${systemPromptNameForPreset(currentPreset)}. MCP: ${presetMcpDisplayText(currentPreset || DEFAULT_PRESET)}. Click to change preset.`}
                aria-haspopup="listbox"
                aria-expanded={presetPickerOpen}
              >
                <PresetIcon preset={currentPreset} /> Preset: {currentPreset.name}
                <span className="composer__model-button-caret">▾</span>
              </button>
              {presetPickerOpen && (
                <div className="composer__model-menu composer__preset-menu" role="dialog" aria-label="Search presets">
                  <label className="composer__model-search">
                    <Icon name="search" size={14} />
                    <input
                      autoFocus
                      value={presetPickerQuery}
                      placeholder="Search presets…"
                      onChange={e => setPresetPickerQuery(e.target.value)}
                    />
                  </label>
                  <div className="composer__model-results" role="listbox">
                    {presetPickerOptions.map(preset => {
                      const isActive = preset.id === currentPreset.id;
                      return (
                        <button
                          type="button"
                          key={preset.id}
                          className={`composer__model-option${isActive ? ' is-active' : ''}`}
                          onClick={() => handlePresetPickerSelect(preset)}
                          disabled={!!presetPickerApplying}
                          role="option"
                          aria-selected={isActive}
                        >
                          <PresetIcon preset={preset} />
                          <span className="composer__model-option-text">
                            <strong>{preset.name}</strong>
                            <span>{preset.description || 'No description'} · Prompt: {systemPromptNameForPreset(preset)} · MCP {presetMcpDisplayText(preset)}</span>
                          </span>
                          {presetPickerApplying === preset.id && <span className="composer__model-option-loading">Applying…</span>}
                        </button>
                      );
                    })}
                    {presetPickerOptions.length === 0 && <div className="composer__model-empty">No matching presets</div>}
                  </div>
                  {presetPickerError && <div className="composer__model-error">{presetPickerError}</div>}
                </div>
              )}
            </div>
          )}
          <button
            className={`composer__tools-toggle ${useMcp && modeSupportsMcp ? 'composer__tools-toggle--active' : ''}`}
            onClick={() => {
              const next = !useMcp;
              setUseMcp(next);
              try { localStorage.setItem(scopedKey(storageScope, MCP_ENABLED_KEY), String(next)); } catch { /* ignore */ }
            }}
            disabled={!modeSupportsMcp}
            title={modeSupportsMcp
              ? (useMcp ? `MCP enabled (${presetMcpDisplayText(currentPreset || DEFAULT_PRESET)}) — click to disable for this chat` : 'Enable the MCP servers selected by this preset')
              : 'MCP is only available for chat-completion models'}
            aria-pressed={useMcp && modeSupportsMcp}
          >
            <Icon name="plug" size={13} /> MCP {useMcp && modeSupportsMcp ? presetMcpServerIds(currentPreset || DEFAULT_PRESET).length : 'OFF'}
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
        {currentCapability === 'audio-generation' && (
          <div className="composer__capability-settings composer__audio-generation-settings" aria-label="Audio generation settings">
            <label className="composer__image-setting">
              <span>Duration</span>
              <input
                type="number"
                min={1}
                max={600}
                value={audioGenerationSettings.duration}
                onChange={e => setAudioGenerationSettings(prev => ({ ...prev, duration: Math.max(1, Math.min(600, parseInt(e.target.value, 10) || 1)) }))}
                disabled={isBusy}
              />
              <small>s</small>
            </label>
            <label className="composer__image-setting">
              <span>Steps</span>
              <input
                type="number"
                min={1}
                max={200}
                value={audioGenerationSettings.steps}
                onChange={e => setAudioGenerationSettings(prev => ({ ...prev, steps: Math.max(1, Math.min(200, parseInt(e.target.value, 10) || 1)) }))}
                disabled={isBusy}
              />
            </label>
            {!isAceStepAudio && (
              <label className="composer__image-setting">
                <span>CFG</span>
                <input
                  type="number"
                  min={0}
                  max={30}
                  step={0.5}
                  value={audioGenerationSettings.cfg}
                  onChange={e => setAudioGenerationSettings(prev => ({ ...prev, cfg: Math.max(0, Math.min(30, parseFloat(e.target.value) || 0)) }))}
                  disabled={isBusy}
                />
              </label>
            )}
            <label className="composer__image-setting">
              <span>Seed</span>
              <input
                type="number"
                min={-1}
                value={audioGenerationSettings.seed}
                placeholder="-1"
                onChange={e => setAudioGenerationSettings(prev => ({ ...prev, seed: seedFromInput(e.target.value) }))}
                disabled={isBusy}
              />
            </label>
            {isAceStepAudio && (
              <label className="composer__image-setting composer__image-setting--language">
                <span>Lyrics language</span>
                <input
                  type="text"
                  maxLength={12}
                  value={audioGenerationSettings.vocalLanguage}
                  onChange={e => setAudioGenerationSettings(prev => ({ ...prev, vocalLanguage: e.target.value }))}
                  placeholder="en"
                  disabled={isBusy}
                />
              </label>
            )}
            {isAceStepAudio && (
              <label className="composer__audio-lyrics">
                <span>Lyrics <small>optional · leave empty for instrumental</small></span>
                <textarea
                  value={audioGenerationSettings.lyrics}
                  onChange={e => setAudioGenerationSettings(prev => ({ ...prev, lyrics: e.target.value }))}
                  placeholder={'[verse]\nMoonlight spills across the floor…\n\n[chorus]\nWe sing until the morning light…'}
                  rows={3}
                  disabled={isBusy}
                />
              </label>
            )}
          </div>
        )}
        {currentCapability === 'tts' && isOpenMossTts && (
          <div className="composer__capability-settings composer__openmoss-settings" aria-label="OpenMOSS voice settings">
            <label className="composer__image-setting composer__image-setting--mode">
              <span>Voice mode</span>
              <select
                value={openMossSettings.mode}
                onChange={event => {
                  const mode = event.target.value as OpenMossMode;
                  setOpenMossSettings(previous => ({ ...previous, mode }));
                  if (mode !== 'clone') setPendingAudioFiles([]);
                }}
                disabled={isBusy}
              >
                <option value="plain">Plain</option>
                <option value="describe">Describe voice</option>
                <option value="clone">Clone WAV sample</option>
              </select>
            </label>
            <label className="composer__openmoss-description">
              <span>
                {openMossSettings.mode === 'describe'
                  ? 'Voice description'
                  : openMossSettings.mode === 'clone'
                    ? 'Style note'
                    : 'Voice style'}
                <small>{openMossSettings.mode === 'clone' ? 'optional' : 'optional instruction'}</small>
              </span>
              <input
                type="text"
                value={openMossSettings.voiceDescription}
                onChange={event => setOpenMossSettings(previous => ({ ...previous, voiceDescription: event.target.value }))}
                placeholder={openMossSettings.mode === 'describe'
                  ? 'Warm low female voice, British accent…'
                  : openMossSettings.mode === 'clone'
                    ? 'Calm, conversational delivery…'
                    : 'Cheerful, whispering, dramatic…'}
                disabled={isBusy}
              />
            </label>
            <div
              className={`composer__openmoss-status${openMossDescribeUnavailable || (openMossSettings.mode === 'clone' && !openMossCloneModel) ? ' composer__openmoss-status--error' : ''}`}
              role="status"
              aria-live="polite"
            >
              {openMossSettings.mode === 'describe'
                ? openMossDescribeUnavailable
                  ? 'Install MOSS-VoiceGen to enable described voices.'
                  : openMossCloneModel
                    ? `Voice design: ${openMossVoiceDesignModel} → speech: ${openMossCloneModel}`
                    : `Using ${openMossVoiceDesignModel} directly for described speech.`
                : openMossSettings.mode === 'clone'
                  ? !openMossCloneModel
                    ? 'Install OpenMOSS-TTS to clone a WAV voice sample.'
                    : pendingAudioFiles.length > 0
                      ? `Voice sample ready: ${pendingAudioFiles[0].name}`
                      : 'Attach one WAV voice sample with the paperclip below.'
                  : 'The selected OpenMOSS model receives the optional voice style directly.'}
            </div>
          </div>
        )}
        {currentCapability === 'model3d' && (
          <div className="composer__capability-settings composer__model3d-settings" aria-label="3D generation settings">
            <label className="composer__image-setting composer__image-setting--mode">
              <span>Source</span>
              <select
                value={model3dSettings.sourceMode}
                onChange={e => {
                  const sourceMode = e.target.value as Model3DSourceMode;
                  setModel3dSettings(prev => ({ ...prev, sourceMode }));
                  if (sourceMode === 'text') setPendingImages([]);
                }}
                disabled={isBusy}
              >
                <option value="image">Image → 3D</option>
                <option value="text">Text → image → 3D</option>
              </select>
            </label>
            {model3dSettings.sourceMode === 'text' && (
              <label className="composer__image-setting composer__image-setting--model">
                <span>Image model</span>
                <select
                  value={model3dSettings.imageModel}
                  onChange={e => setModel3dSettings(prev => ({ ...prev, imageModel: e.target.value }))}
                  disabled={isBusy || imageGenerationModels.length === 0}
                >
                  {imageGenerationModels.length === 0 && <option value="">Download an image model first</option>}
                  {imageGenerationModels.map(name => <option key={name} value={name}>{name}</option>)}
                </select>
              </label>
            )}
            <label className="composer__image-setting">
              <span>Resolution</span>
              <select
                value={model3dSettings.resolution}
                onChange={e => setModel3dSettings(prev => ({ ...prev, resolution: Number(e.target.value) as 512 | 1024 | 1536 }))}
                disabled={isBusy}
              >
                <option value={512}>512 · fast</option>
                <option value={1024}>1024 · sharp</option>
                <option value={1536}>1536 · heavy</option>
              </select>
            </label>
            <label className="composer__image-setting">
              <span>Background</span>
              <select
                value={model3dSettings.backgroundRemoval}
                onChange={e => setModel3dSettings(prev => ({ ...prev, backgroundRemoval: e.target.value as 'birefnet' | 'threshold' }))}
                disabled={isBusy}
              >
                <option value="birefnet">Auto matte</option>
                <option value="threshold">Plain background</option>
              </select>
            </label>
            <label className="composer__image-setting">
              <span>Seed</span>
              <input
                type="number"
                min={-1}
                value={model3dSettings.seed}
                placeholder="-1"
                onChange={e => setModel3dSettings(prev => ({ ...prev, seed: seedFromInput(e.target.value) }))}
                disabled={isBusy}
              />
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
            disabled={!canAttach || !currentModel || isBusy || (!isOpenMossCloneMode && pendingImages.length >= MAX_IMAGES)}
            title={isOpenMossCloneMode
              ? 'Attach WAV voice sample'
              : currentCapability === 'model3d'
                ? 'Attach reference image'
                : canUseAudioInput
                  ? 'Attach image or audio'
                  : 'Attach image'}
            aria-label={isOpenMossCloneMode
              ? 'Attach WAV voice sample'
              : currentCapability === 'model3d'
                ? 'Attach reference image'
                : canUseAudioInput
                  ? 'Attach image or audio'
                  : 'Attach image'}
          >
            <Icon name="paperclip" size={16} />
          </button>
          <input
            ref={fileInputRef}
            type="file"
            accept={fileAccept}
            multiple={!isOpenMossCloneMode && !(currentCapability === 'audio' && !modeSupportsChatCompletions) && !(currentCapability === 'image' && imageMode === 'edit') && currentCapability !== 'model3d'}
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
            aria-label="Message"
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
      <div aria-live="assertive" aria-atomic="true" className="sr-only">
        {streamStatus}
      </div>
      <div aria-live="polite" aria-atomic="false" className="sr-only">
        {liveText}
      </div>
    </>
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
          : 'Connect to a server and load a chat, omni, image, audio, TTS, or 3D model to begin.'}
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
            const audioInput = modelSupportsChatAudioInput(customInfo || null, m);
            const modeLabel = modelModeLabel(cap, audioInput);
            const selectable = canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(cap);
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
                  <span className={`cap-badge cap-badge--${capabilityBadge(cap)}`}><ModelModeIcons capability={cap} audioInput={audioInput} size={13} /> {modeLabel}</span>
                </div>
                {isActive ? (
                  <span className="active-card__status">● Active {modeLabel} mode</span>
                ) : selectable ? (
                  <button className="active-card__action" onClick={() => onModelSelect(m.model_name)}>
                    Use in {modeLabel} mode ▸
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
  // Track which choice was selected per call index. Map key is the call's position in the array.
  const [selections, setSelections] = useState<Map<number, string>>(() => new Map());

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
            const selectedChoice = selections.get(i);
            const handleSelect = (choice: string) => {
              if (selectedChoice) return;
              setSelections(prev => new Map(prev).set(i, choice));
              onOptionSelect?.(choice);
            };
            return (
              <div key={i} className="options-block">
                {question && <div className="options-block__question">{question}</div>}
                <div className="options-block__choices">
                  {choices.map((choice: string, ci: number) => (
                    <button
                      key={ci}
                      className={`options-block__btn${selectedChoice === choice ? ' options-block__btn--selected' : ''}`}
                      disabled={!!selectedChoice && selectedChoice !== choice}
                      aria-pressed={selectedChoice === choice}
                      onClick={() => handleSelect(choice)}
                    >
                      {selectedChoice === choice ? '\u2713 ' : ''}{choice}
                    </button>
                  ))}
                </div>
                {selectedChoice && (
                  <div className="options-block__confirmation">\u2713 You chose: {selectedChoice}</div>
                )}
                {!selectedChoice && allowCustom && (
                  <div className="options-block__custom">
                    <input className="options-block__input" placeholder="Or type your own\u2026"
                      onKeyDown={e => { if (e.key === 'Enter' && (e.target as HTMLInputElement).value.trim()) { handleSelect((e.target as HTMLInputElement).value.trim()); } }} />
                    <button className="options-block__submit" onClick={e => {
                      const input = (e.target as HTMLElement).previousElementSibling as HTMLInputElement;
                      if (input?.value.trim()) handleSelect(input.value.trim());
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

const MessageBubble: React.FC<{ message: Message; activeModel: ModelSnapshot | null; userLabel: string; onOptionSelect?: (text: string) => void; onRetry?: () => void; onSpeak?: () => void; onEditUser?: (text: string) => void }> = ({ message, activeModel, userLabel, onOptionSelect, onRetry, onSpeak, onEditUser }) => {
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
            <summary>Reasoning{reasoningSummary(message.stats)}</summary>
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
            <a
              href={message.audioUrl}
              download={(message.audioName || `${displayModel?.name || 'lemonade-audio'}.wav`).replace(/[^a-z0-9._-]+/gi, '-')}
              className="message__action message__audio-download"
            >
              <Icon name="download" size={13} /> Download audio
            </a>
          </div>
        )}
        {message.model3dUrl && (
          <Suspense fallback={<div className="model3d-viewer model3d-viewer--loading" role="status">Preparing 3D result…</div>}>
            <Model3DResult src={message.model3dUrl} name={message.model3dName || displayModel?.name} />
          </Suspense>
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
          {onSpeak && (
            <button type="button" className="message__action" onClick={onSpeak}>
              <Icon name="tts" size={13} /> Read aloud
            </button>
          )}
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
