import React, { Suspense, lazy, useState, useRef, useCallback, useEffect, useMemo } from 'react';
import type { ChatMessage, ChatCompletionStats, ConnectionStatus, LoadedModel, ModelInfo, RealtimeTranscriptionHandle } from '../api';
import { copyTextToClipboard } from '../clipboard';
import { Icon, CapabilityIcon } from './Icon';

import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { scheduleIdleWork } from '../startupScheduler';

const Model3DResult = lazy(() => import(/* webpackChunkName: "chat-model3d" */ './Model3DResult'));
const LogViewer = lazy(() => import(/* webpackChunkName: "chat-logs" */ './LogViewer'));
const EffectiveSettingsModal = lazy(() => import(/* webpackChunkName: "chat-effective-settings" */ './EffectiveSettingsModal'));
const LazyMarkdownMessage = lazy(() => import(/* webpackChunkName: "markdown-renderer" */ './MarkdownMessage'));
const MarkdownMessage: React.FC<React.ComponentProps<typeof LazyMarkdownMessage>> = props => (
  <Suspense fallback={<div className="message__content message__content--loading" aria-busy="true" />}>
    <LazyMarkdownMessage {...props} />
  </Suspense>
);
import { useChatStreaming, ToolCallEntry, ChatToolRuntime, ToolArtifact } from '../hooks/useChatStreaming';
import { useAudioCapture } from '../hooks/useAudioCapture';
import { useFocusTrap } from '../hooks/useFocusTrap';
import {
  canSelectInComposer,
  capabilityBadge,
  capabilityFromLoaded,
  capabilityFromModelInfo,
  capabilityLabel,
  modelDisplayName,
  modelInitial,
  modelSupportsChatAudioInput,
  modelSupportsChatImageInput,
  ModelCapability,
  ModelSnapshot,
  selectPreferredLoadedModel,
  snapshotFromLoaded,
  snapshotFromModelInfo,
  snapshotFromName,
} from '../modelCapabilities';
import { storageKey } from '../storage';
import { CHAT_HISTORY_PREFERENCE_EVENT, loadChatHistoryPreference } from '../features/chatHistory/historySettings';
import type { DownloadListItem } from '../features/downloadManager/downloadStore';
import { findModelInfoByName, getAudioTranscriptionComponent, getPrimaryChatComponent, getVisionChatComponent, isCollectionModel } from '../features/collections/collectionModels';
import { LEMONADE_MCP_SERVER_ID, LEMONADE_MCP_TOOL_COUNT, MAX_MCP_SERVER_SELECTION, type McpServerToolOption } from '../tools/mcpMetadata';
import type { ModelTuning } from '../modelConfiguration';
import { TTS_SETTINGS_EVENT, loadTtsPlaybackSettings, ttsVoiceFromRecipeOptions } from '../features/audio/ttsSettings';
import {
  LEMONADE_DEFAULT_CHAT_MODELS,
  lemonadeDefaultModel,
  lemonadeDefaultModelInfo,
  loadLastReadyModelName,
  loadPreferredDefaultModelName,
  modelInfoName,
  modelIsDownloaded,
  resolveLastReadyChatModel,
  saveLastReadyModelName,
  savePreferredDefaultModelName,
} from '../features/chatDefaultModels';
import {
  GLOBAL_MODEL_SETTINGS_EVENT,
  loadGlobalModelSettings,
  loadPinnedModelNames,
  loadWithGlobalModelPolicy,
} from '../features/modelSettings/globalModelSettings';

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
const STORAGE_VERSION = 3;

// Keep aligned with modelConfiguration.ts without pulling that feature module into the cold chat chunk.
const DEFAULT_CONTEXT_SIZE = 4096;

let apiClientPromise: Promise<(typeof import('../api'))['default']> | null = null;
function getApiClient(): Promise<(typeof import('../api'))['default']> {
  if (!apiClientPromise) {
    apiClientPromise = import(/* webpackChunkName: "api-client" */ '../api').then(module => module.default);
  }
  return apiClientPromise;
}

let downloadStoreModulePromise: Promise<typeof import('../features/downloadManager/downloadStore')> | null = null;
function getDownloadStoreModule(): Promise<typeof import('../features/downloadManager/downloadStore')> {
  if (!downloadStoreModulePromise) {
    downloadStoreModulePromise = import(
      /* webpackChunkName: "download-store" */ '../features/downloadManager/downloadStore'
    );
  }
  return downloadStoreModulePromise;
}

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

function loadChatLogsWidth(): number {
  if (typeof window === 'undefined') return CHAT_LOGS_DEFAULT_WIDTH;
  try {
    const stored = Number(window.localStorage.getItem(scopedKey(CHAT_LOGS_WIDTH_KEY)));
    const width = Number.isFinite(stored) ? stored : CHAT_LOGS_DEFAULT_WIDTH;
    return clampChatLogsWidth(width, true);
  } catch {
    return clampChatLogsWidth(CHAT_LOGS_DEFAULT_WIDTH, true);
  }
}

function generateId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function scopedKey(key: string): string {
  return storageKey(key);
}

function loadScopedStringArray(key: string): string[] | null {
  try {
    const raw = localStorage.getItem(scopedKey(key));
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.map(value => String(value)).filter(Boolean) : null;
  } catch {
    return null;
  }
}

function saveScopedStringArray(key: string, values: string[] | null): void {
  try {
    if (values === null) localStorage.removeItem(scopedKey(key));
    else localStorage.setItem(scopedKey(key), JSON.stringify([...new Set(values.filter(Boolean))]));
  } catch {
    // Non-critical UI preference persistence.
  }
}

function loadPersistencePreference(): boolean {
  return loadChatHistoryPreference();
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

function loadConversations(persist: boolean): Conversation[] {
  if (!persist) return [];
  try {
    const raw = localStorage.getItem(scopedKey(STORAGE_KEY));
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    const list: unknown[] = Array.isArray(parsed) ? parsed : (Array.isArray(parsed?.conversations) ? parsed.conversations : []);
    return list.map(normalizeConversation).filter((c): c is Conversation => !!c);
  } catch { /* ignore */ }
  return [];
}

function saveConversations(convos: Conversation[], persist: boolean) {
  if (!persist) {
    try {
      localStorage.removeItem(scopedKey(STORAGE_KEY));
      localStorage.removeItem(scopedKey(ACTIVE_KEY));
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
  try { localStorage.setItem(scopedKey(STORAGE_KEY), JSON.stringify({ version: STORAGE_VERSION, conversations: stripped })); } catch { /* ignore */ }
}

function loadActiveId(persist: boolean): string | null {
  if (!persist) return null;
  try { return localStorage.getItem(scopedKey(ACTIVE_KEY)); } catch { return null; }
}

function saveActiveId(id: string | null, persist: boolean) {
  try {
    if (!persist) {
      localStorage.removeItem(scopedKey(ACTIVE_KEY));
    } else if (id) {
      localStorage.setItem(scopedKey(ACTIVE_KEY), id);
    } else {
      localStorage.removeItem(scopedKey(ACTIVE_KEY));
    }
  } catch { /* ignore */ }
}

/**
 * Chat only needs to know whether a model is blocked by a non-terminal
 * download. Progress/speed updates arrive every second and must not rerender
 * completed message markup while a user is selecting text to copy.
 */
function isDownloadTerminal(download: Pick<DownloadListItem, 'status' | 'running'>): boolean {
  return download.running !== true && (
    download.status === 'completed'
    || download.status === 'error'
    || download.status === 'cancelled'
  );
}

function downloadsForModel(downloads: DownloadListItem[], modelName: string): DownloadListItem[] {
  const target = modelName.trim().toLowerCase();
  return downloads.filter(download => {
    if (download.downloadType !== 'model') return false;
    const name = download.modelName.trim().toLowerCase();
    const id = download.id.trim().toLowerCase();
    return name === target || id === `model:${target}` || id.endsWith(`:${target}`);
  });
}

function activeDownloadForModel(downloads: DownloadListItem[], modelName: string): DownloadListItem | undefined {
  return downloadsForModel(downloads, modelName).find(download => !isDownloadTerminal(download));
}

function chatBlockingDownloads(downloads: DownloadListItem[]): DownloadListItem[] {
  return downloads.filter(download => !isDownloadTerminal(download));
}

function chatBlockingDownloadsKey(downloads: DownloadListItem[]): string {
  return downloads
    .map(download => `${download.downloadType}:${download.id}:${download.modelName}`)
    .sort()
    .join('|');
}

function titleFromInput(text: string, hasImages: boolean, audioFiles: File[] = []): string {
  const clean = text.trim();
  if (clean) return clean.slice(0, 50) + (clean.length > 50 ? '…' : '');
  if (audioFiles.length > 0) return `Audio: ${audioFiles[0].name}`.slice(0, 50);
  if (hasImages) return 'Image conversation';
  return 'New conversation';
}

function mcpToolNamesForServers(servers: McpServerToolOption[], serverIds: string[]): string[] {
  const selected = new Set(serverIds);
  return servers
    .filter(server => selected.has(server.id))
    .flatMap(server => server.toolOptions.map(tool => tool.runtimeName));
}

function isRouterRecipe(recipe?: string | null): boolean {
  const normalized = String(recipe || '').trim().toLowerCase();
  return normalized === 'collection.router' || normalized.startsWith('collection.router.');
}

function modelModeBadge(capability: ModelCapability, recipe?: string | null): string {
  return isRouterRecipe(recipe) ? 'router' : capabilityBadge(capability);
}

const ModelModeIcons: React.FC<{
  capability: ModelCapability;
  recipe?: string | null;
  audioInput?: boolean;
  size?: number;
}> = ({ capability, recipe, audioInput = false, size = 14 }) => {
  if (isRouterRecipe(recipe)) {
    return <Icon name="router" size={size} aria-hidden="true" />;
  }
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

function modelModeDisplayLabel(capability: ModelCapability, audioInput = false, recipe?: string | null): string {
  return isRouterRecipe(recipe) ? 'Router' : modelModeLabel(capability, audioInput);
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
  serverModels: ModelInfo[];
  connectionStatus: ConnectionStatus;
  onModelSelect: (model: string) => void;
  onOpenModelDetails: (model: string) => void;
  onRefresh: () => void | Promise<void>;
}

interface ModelPreparationState {
  modelName: string;
  phase: 'waiting' | 'downloading' | 'loading';
  percent?: number;
}

const MCP_ENABLED_KEY = 'mcp_enabled';
const MCP_SERVER_IDS_KEY = 'mcp_server_ids';
const MCP_TOOL_NAMES_KEY = 'mcp_tool_names';
const LEGACY_TOOLS_KEY = 'use_tools';
const DEFAULT_MCP_SERVER_IDS = [LEMONADE_MCP_SERVER_ID];
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

function imageDefaultsForModel(loadedModel: LoadedModel | null, modelInfo: ModelInfo | null, directRecipeOptions?: Record<string, unknown> | null): ImageGenerationSettings {
  const modelImageDefaults = partialImageSettingsFromSource(modelInfo?.image_defaults as Record<string, unknown> | undefined);
  const modelRecipeOptions = partialImageSettingsFromSource(modelInfo?.recipe_options as Record<string, unknown> | undefined);
  const loadedRecipeOptions = partialImageSettingsFromSource(loadedModel?.recipe_options);
  const directDefaults = partialImageSettingsFromSource(directRecipeOptions);
  return {
    ...DEFAULT_IMAGE_SETTINGS,
    ...modelImageDefaults,
    ...modelRecipeOptions,
    ...loadedRecipeOptions,
    ...directDefaults,
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

function friendlyErrorMessage(error: unknown): string {
  if (error && typeof error === 'object') {
    const value = error as { userMessage?: unknown; message?: unknown };
    if (typeof value.userMessage === 'string' && value.userMessage) return value.userMessage;
    if (typeof value.message === 'string' && value.message) return value.message;
  }
  return String(error || 'Unknown error');
}

function friendlyChatError(message: string): string {
  const cleaned = message.replace(/^Error:\s*/i, '').trim();
  if (!cleaned) return "I couldn't complete that request. Please check the server logs for details.";
  return `I couldn't complete that request.\n\n${cleaned}`;
}

const ChatView: React.FC<ChatViewProps> = ({ currentModel: selectedModel, loadedModels, serverModels, connectionStatus, onModelSelect, onOpenModelDetails, onRefresh }) => {
  const [fallbackModelOverride, setFallbackModelOverride] = useState<string | null>(null);
  const [preferredDefaultModelName, setPreferredDefaultModelName] = useState(() => loadPreferredDefaultModelName());
  const [lastReadyModelName, setLastReadyModelName] = useState<string | null>(() => loadLastReadyModelName());
  const [modelPreparations, setModelPreparations] = useState<Record<string, ModelPreparationState>>({});
  const [persistHistory, setPersistHistory] = useState(() => loadPersistencePreference());
  // Large persisted conversations are not required to draw the first usable
  // frame.  Hydrate them after paint instead of JSON-parsing the full history
  // synchronously inside the initial React render.
  const [historyHydrated, setHistoryHydrated] = useState(() => !loadPersistencePreference());
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [activeId, setActiveId] = useState<string | null>(null);
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
  const [capabilityBusyConvoIds, setCapabilityBusyConvoIds] = useState<Set<string>>(() => new Set());
  const [ttsPlaybackSettings, setTtsPlaybackSettings] = useState(() => loadTtsPlaybackSettings());
  const [globalModelSettings, setGlobalModelSettings] = useState(() => loadGlobalModelSettings());
  const [railExpanded, setRailExpanded] = useState(true);
  const [mobileSheetOpen, setMobileSheetOpen] = useState(false);
  const sheetHandleRef = useRef<HTMLDivElement>(null);
  const sheetTriggerRef = useRef<HTMLButtonElement>(null);
  const bottomSheetRef = useRef<HTMLDivElement>(null);
  const [useMcp, setUseMcp] = useState(() => {
    try {
      const explicit = localStorage.getItem(scopedKey(MCP_ENABLED_KEY));
      if (explicit !== null) return explicit === 'true';
      const legacy = localStorage.getItem(scopedKey(LEGACY_TOOLS_KEY));
      if (legacy !== null) return legacy === 'true';
      return true;
    } catch { return true; }
  });
  const [selectedMcpServerIds, setSelectedMcpServerIds] = useState<string[]>(() => loadScopedStringArray(MCP_SERVER_IDS_KEY) || DEFAULT_MCP_SERVER_IDS);
  const [selectedMcpToolNames, setSelectedMcpToolNames] = useState<string[] | null>(() => loadScopedStringArray(MCP_TOOL_NAMES_KEY));
  const [mcpPickerOpen, setMcpPickerOpen] = useState(false);
  const [mcpPickerTab, setMcpPickerTab] = useState<'lemonade' | 'external'>('lemonade');
  const [addMenuOpen, setAddMenuOpen] = useState(false);
  const [mcpOptions, setMcpOptions] = useState<McpServerToolOption[]>([]);
  const [mcpPickerLoading, setMcpPickerLoading] = useState(false);
  const [mcpPickerError, setMcpPickerError] = useState('');
  const [showInlineLogs, setShowInlineLogs] = useState(false);
  const [chatLogsWidth, setChatLogsWidth] = useState(() => loadChatLogsWidth());
  const [modelPickerOpen, setModelPickerOpen] = useState(false);
  const [modelPickerQuery, setModelPickerQuery] = useState('');
  const [modelPickerLoading, setModelPickerLoading] = useState<string | null>(null);
  const [modelPickerError, setModelPickerError] = useState<string | null>(null);
  const [modelPickerUnloading, setModelPickerUnloading] = useState<string | null>(null);
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>([]);
  const downloadAvailabilityKeyRef = useRef(chatBlockingDownloadsKey(downloadItems));
  const [unloadAnnouncement, setUnloadAnnouncement] = useState('');
  const [effectiveSettingsOpen, setEffectiveSettingsOpen] = useState(false);
  const [serverDefaultCtxSize, setServerDefaultCtxSize] = useState(DEFAULT_CONTEXT_SIZE);
  const [currentModelTuning, setCurrentModelTuning] = useState<ModelTuning | null>(null);
  const threadRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const modelPickerRef = useRef<HTMLDivElement>(null);
  const addMenuRef = useRef<HTMLDivElement>(null);
  const mcpReturnFocusEntryRef = useRef<'tools'>('tools');
  const mcpBackButtonRef = useRef<HTMLButtonElement | null>(null);
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
  const handleSendRef = useRef<(overrideText?: string) => Promise<void>>(async () => {});

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
    if (historyHydrated) return;
    if (!persistHistory) {
      setHistoryHydrated(true);
      return;
    }
    const cancelSchedule = scheduleIdleWork(() => {
      const storedConversations = loadConversations(true);
      const storedActiveId = loadActiveId(true);
      setConversations(current => {
        if (current.length === 0) return storedConversations;
        const existing = new Set(current.map(conversation => conversation.id));
        return [...current, ...storedConversations.filter(conversation => !existing.has(conversation.id))];
      });
      setActiveId(current => current || storedActiveId);
      setHistoryHydrated(true);
    }, 500);
    return cancelSchedule;
  }, [historyHydrated, persistHistory]);

  useEffect(() => {
    let cancelled = false;
    if (connectionStatus !== 'connected') {
      setServerDefaultCtxSize(DEFAULT_CONTEXT_SIZE);
      return () => { cancelled = true; };
    }
    void getApiClient()
      .then(api => api.getDefaultContextSize())
      .then(value => {
        if (!cancelled) setServerDefaultCtxSize(typeof value === 'number' ? value : DEFAULT_CONTEXT_SIZE);
      })
      .catch(() => {
        if (!cancelled) setServerDefaultCtxSize(DEFAULT_CONTEXT_SIZE);
      });
    return () => { cancelled = true; };
  }, [connectionStatus]);

  useEffect(() => {
    try {
      window.localStorage.setItem(scopedKey(CHAT_LOGS_WIDTH_KEY), String(chatLogsWidth));
    } catch {
      // Non-critical: inline log width persistence is best-effort only.
    }
  }, [chatLogsWidth]);

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

  const [customModelInfos, setCustomModelInfos] = useState<ModelInfo[]>([]);
  useEffect(() => {
    let cancelled = false;
    const cancelSchedule = scheduleIdleWork(() => {
      void import(/* webpackChunkName: "custom-model-store" */ '../features/customModels/customModelStore')
        .then(({ loadCustomModels, customModelToModelInfo }) => {
          if (!cancelled) setCustomModelInfos(loadCustomModels().map(customModelToModelInfo));
        })
        .catch(error => console.warn('Failed to hydrate chat custom models:', error));
    }, 650);
    return () => {
      cancelled = true;
      cancelSchedule();
    };
  }, []);
  const knownModelInfos = useMemo(
    () => {
      const seen = new Set<string>();
      const infos: ModelInfo[] = [];
      const defaultInfos = LEMONADE_DEFAULT_CHAT_MODELS.map(lemonadeDefaultModelInfo);
      [...customModelInfos, ...serverModels, ...defaultInfos].forEach(info => {
        const rawName = String((info as any).model_name || info.name || info.id || '').trim();
        const name = rawName.toLowerCase();
        if (!name || seen.has(name)) return;
        const configuredDefault = lemonadeDefaultModel(rawName);
        const syntheticDefaultInfo = configuredDefault ? lemonadeDefaultModelInfo(configuredDefault) : null;
        const normalizedInfo = syntheticDefaultInfo ? {
          ...syntheticDefaultInfo,
          ...info,
          labels: Array.from(new Set([
            ...(syntheticDefaultInfo.labels || []),
            ...(info.labels || []),
          ])),
        } : info;
        seen.add(name);
        infos.push(normalizedInfo);
      });
      return infos;
    },
    [customModelInfos, loadedModels, serverModels],
  );
  const lastReadyModelInfo = useMemo(
    () => resolveLastReadyChatModel(knownModelInfos, lastReadyModelName),
    [knownModelInfos, lastReadyModelName],
  );
  // A selected/loaded model always wins. With no active runtime, reuse the last
  // locally-ready chat model before falling back to the user's Lemonade default.
  const currentModel = fallbackModelOverride
    || selectedModel
    || modelInfoName(lastReadyModelInfo)
    || preferredDefaultModelName;
  const currentLoadedModel = useMemo(
    () => loadedModels.find(m => m.model_name.toLowerCase() === currentModel.toLowerCase()) || null,
    [loadedModels, currentModel],
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
  const currentDefaultModel = lemonadeDefaultModel(currentModel);

  useEffect(() => {
    if (!currentLoadedModel || (currentCapability !== 'chat' && currentCapability !== 'omni')) return;
    saveLastReadyModelName(currentLoadedModel.model_name);
    setLastReadyModelName(currentLoadedModel.model_name);
  }, [currentCapability, currentLoadedModel]);

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
        && (loadedNames.has(model.name.toLowerCase())
          || model.name === currentModel
          || (model.downloaded && !activeDownloadForModel(downloadItems, model.name))));
  }, [currentModel, downloadItems, knownModelInfos, loadedModels]);
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
      if (name && !activeDownloadForModel(downloadItems, name)) names.add(name);
    });
    return Array.from(names).sort((a, b) => a.localeCompare(b));
  }, [downloadItems, knownModelInfos, loadedModels]);

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
    const specialCapability = !['chat', 'omni', 'unknown'].includes(currentCapability);
    if (!modelPickerOpen && !specialCapability) return;

    let cancelled = false;
    let unsubscribe: (() => void) | undefined;
    const hydrate = () => {
      void getDownloadStoreModule().then(({ downloadStore }) => {
        if (cancelled) return;
        const update = (items: DownloadListItem[]) => {
          const blocking = chatBlockingDownloads(items);
          const nextKey = chatBlockingDownloadsKey(blocking);
          if (nextKey === downloadAvailabilityKeyRef.current) return;
          downloadAvailabilityKeyRef.current = nextKey;
          setDownloadItems(blocking);
        };
        update(downloadStore.snapshot());
        unsubscribe = downloadStore.subscribe(update);
      }).catch(error => console.warn('Failed to hydrate chat download state:', error));
    };

    // Opening the picker is an explicit interaction, so hydrate immediately.
    // Capability-specific background state can still wait for idle time.
    const cancelSchedule = modelPickerOpen
      ? (hydrate(), () => {})
      : scheduleIdleWork(hydrate, 650);
    return () => {
      cancelled = true;
      cancelSchedule();
      unsubscribe?.();
    };
  }, [currentCapability, modelPickerOpen]);

  useEffect(() => {
    // Initial state already read these settings in useState.  Do not perform the
    // same synchronous localStorage/JSON work again immediately after mount.
    const reloadTtsSettings = () => setTtsPlaybackSettings(loadTtsPlaybackSettings());
    window.addEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
    return () => window.removeEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
  }, []);

  useEffect(() => {
    const reloadGlobalModelSettings = () => setGlobalModelSettings(loadGlobalModelSettings());
    window.addEventListener(GLOBAL_MODEL_SETTINGS_EVENT, reloadGlobalModelSettings);
    return () => window.removeEventListener(GLOBAL_MODEL_SETTINGS_EVENT, reloadGlobalModelSettings);
  }, []);

  useEffect(() => {
    let cancelled = false;
    const needsTuning = !!currentModel && (
      currentCapability === 'image'
      || currentCapability === 'audio-generation'
      || currentCapability === 'tts'
      || isOpenMossTts
    );
    if (!needsTuning) {
      setCurrentModelTuning(null);
      return () => { cancelled = true; };
    }
    void import(/* webpackChunkName: "model-configuration" */ '../modelConfiguration')
      .then(({ loadModelTuning }) => {
        if (!cancelled) setCurrentModelTuning(loadModelTuning(currentModel || ''));
      })
      .catch(() => {
        if (!cancelled) setCurrentModelTuning(null);
      });
    return () => { cancelled = true; };
  }, [currentCapability, currentModel, isOpenMossTts]);

  useEffect(() => {
    if (currentCapability !== 'audio-generation') return;
    const recipeOptions = currentModelTuning?.recipe_options || {};
    setAudioGenerationSettings(prev => ({
      ...prev,
      duration: isAceStepAudio ? 150 : 10,
      steps: typeof recipeOptions.steps === 'number' ? recipeOptions.steps : 50,
      cfg: typeof recipeOptions.cfg_scale === 'number' ? recipeOptions.cfg_scale : 4.5,
      lyrics: '',
    }));
  }, [currentModel, currentCapability, currentModelTuning, isAceStepAudio]);

  useEffect(() => {
    if (!isOpenMossTts) return;
    setOpenMossSettings({
      mode: 'plain',
      voiceDescription: String(currentModelTuning?.recipe_options?.voice || ''),
    });
    setPendingAudioFiles([]);
  }, [currentModel, currentModelTuning, isOpenMossTts]);

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
  const supportsChatImageInput = useMemo(() => {
    if (currentCapability === 'omni') {
      return Boolean(getVisionChatComponent(currentKnownModelInfo, knownModelInfos));
    }
    return currentCapability === 'chat'
      && modelSupportsChatImageInput(currentKnownModelInfo, currentLoadedModel);
  }, [currentCapability, currentKnownModelInfo, currentLoadedModel, knownModelInfos]);

  const defaultImageSettings = useMemo(
    () => imageDefaultsForModel(
      currentLoadedModel,
      currentKnownModelInfo,
      currentCapability === 'image'
        ? (currentModelTuning?.recipe_options as Record<string, unknown> | undefined)
        : undefined,
    ),
    [currentLoadedModel, currentKnownModelInfo, currentModel, currentCapability, currentModelTuning],
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

  useEffect(() => {
    const keepsImageAttachments = supportsChatImageInput
      || (currentCapability === 'image' && imageMode === 'edit')
      || (currentCapability === 'model3d' && model3dSettings.sourceMode === 'image');
    if (!keepsImageAttachments) setPendingImages([]);
  }, [currentCapability, imageMode, model3dSettings.sourceMode, supportsChatImageInput]);

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
    recipe?: string;
    loaded: boolean;
    audioInput: boolean;
    info?: ModelInfo;
    detail: string;
    defaultTier?: 'tiny' | 'quality';
    defaultLabel?: string;
    defaultIcon?: 'minimize-2' | 'gem';
    deferredUntilSend?: boolean;
  };

  const modelPickerOptions = useMemo<ModelPickerOption[]>(() => {
    const seen = new Set<string>();
    const options: ModelPickerOption[] = [];
    const addOption = (option: ModelPickerOption) => {
      const key = option.name.toLowerCase();
      if (!option.name || seen.has(key)) return;
      if (!['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(option.capability)) return;
      const configuredDefault = lemonadeDefaultModel(option.name);
      seen.add(key);
      options.push(configuredDefault ? {
        ...option,
        defaultTier: configuredDefault.tier,
        defaultLabel: configuredDefault.label,
        defaultIcon: configuredDefault.icon,
      } : option);
    };

    selectableModels.forEach(model => addOption({
      name: model.model_name,
      capability: capabilityForLoaded(model),
      recipe: model.recipe,
      loaded: true,
      audioInput: audioInputForLoaded(model),
      detail: `Loaded · ${model.recipe || 'runtime'}${model.device ? ` · ${model.device}` : ''}`,
    }));

    knownModelInfos.forEach(info => {
      const name = String((info as any).model_name || info.name || info.id || '').trim();
      const capability = capabilityFromModelInfo(info);
      if (!modelIsDownloaded(info) || activeDownloadForModel(downloadItems, name)) return;
      const configuredDefault = lemonadeDefaultModel(name);
      addOption({
        name,
        capability,
        recipe: typeof info.recipe === 'string' ? info.recipe : undefined,
        loaded: false,
        audioInput: modelSupportsChatAudioInput(info, null),
        info,
        detail: configuredDefault ? 'Downloaded · loads when you send' : 'Downloaded · click to load',
        deferredUntilSend: Boolean(configuredDefault),
      });
    });

    LEMONADE_DEFAULT_CHAT_MODELS.forEach(model => addOption({
      name: model.name,
      capability: 'chat',
      loaded: false,
      audioInput: false,
      info: findModelInfoByName(knownModelInfos, model.name) || lemonadeDefaultModelInfo(model),
      detail: model.description,
      deferredUntilSend: true,
    }));

    const q = modelPickerQuery.trim().toLowerCase();
    const filtered = q
      ? options.filter(option => `${option.name} ${option.defaultLabel || ''} ${modelModeDisplayLabel(option.capability, option.audioInput, option.recipe)} ${option.detail}`.toLowerCase().includes(q))
      : options;
    return filtered.slice(0, 80);
  }, [audioInputForLoaded, capabilityForLoaded, downloadItems, knownModelInfos, modelPickerQuery, selectableModels]);

  const modeSupportsChatCompletions = currentCapability === 'chat' || currentCapability === 'omni';
  const modeSupportsMcp = modeSupportsChatCompletions;
  const canUseAudioInput = currentCapability === 'omni' || currentCapability === 'audio' || (currentCapability === 'chat' && supportsChatAudioInput);

  const persistMcpEnabled = useCallback((next: boolean) => {
    setUseMcp(next);
    try { localStorage.setItem(scopedKey(MCP_ENABLED_KEY), String(next)); } catch { /* ignore */ }
  }, []);

  const persistMcpSelection = useCallback((serverIds: string[], toolNames: string[] | null) => {
    const uniqueServerIds = [...new Set(serverIds.filter(Boolean))].slice(0, MAX_MCP_SERVER_SELECTION);
    const uniqueToolNames = toolNames === null ? null : [...new Set(toolNames.filter(Boolean))];
    setSelectedMcpServerIds(uniqueServerIds);
    setSelectedMcpToolNames(uniqueToolNames);
    saveScopedStringArray(MCP_SERVER_IDS_KEY, uniqueServerIds);
    saveScopedStringArray(MCP_TOOL_NAMES_KEY, uniqueToolNames);
  }, []);

  const startLemonadeToolPrompt = useCallback((text: string) => {
    persistMcpEnabled(true);
    persistMcpSelection([LEMONADE_MCP_SERVER_ID], null);
    setInputValue(text);
    inputRef.current?.focus();
  }, [persistMcpEnabled, persistMcpSelection]);

  const selectedMcpServerIdSet = useMemo(() => new Set(selectedMcpServerIds), [selectedMcpServerIds]);
  const selectedMcpToolNameSet = useMemo(() => selectedMcpToolNames === null ? null : new Set(selectedMcpToolNames), [selectedMcpToolNames]);
  const selectedMcpToolCount = useMemo(() => {
    if (selectedMcpToolNames !== null) return selectedMcpToolNames.length;
    const visibleToolCount = mcpToolNamesForServers(mcpOptions, selectedMcpServerIds).length;
    if (visibleToolCount > 0) return visibleToolCount;
    return selectedMcpServerIds.includes(LEMONADE_MCP_SERVER_ID) ? LEMONADE_MCP_TOOL_COUNT : 0;
  }, [mcpOptions, selectedMcpServerIds, selectedMcpToolNames]);
  const visibleMcpOptions = useMemo(
    () => mcpOptions.filter(server => mcpPickerTab === 'lemonade' ? server.transport === 'builtin' : server.transport !== 'builtin'),
    [mcpOptions, mcpPickerTab],
  );

  const loadMcpPickerOptions = useCallback(async () => {
    setMcpPickerLoading(true);
    setMcpPickerError('');
    try {
      const { listMcpServerToolOptions } = await import(/* webpackChunkName: "mcp-runtime" */ '../tools/mcpRuntime');
      setMcpOptions(await listMcpServerToolOptions());
    } catch (error) {
      setMcpPickerError(friendlyErrorMessage(error));
    } finally {
      setMcpPickerLoading(false);
    }
  }, []);

  useEffect(() => {
    if (!mcpPickerOpen || !modeSupportsMcp) return;
    void loadMcpPickerOptions();
  }, [loadMcpPickerOptions, mcpPickerOpen, modeSupportsMcp]);

  useEffect(() => {
    if (!addMenuOpen) return;
    const onPointerDown = (event: PointerEvent) => {
      const root = addMenuRef.current;
      if (!root || root.contains(event.target as Node)) return;
      setAddMenuOpen(false);
      setMcpPickerOpen(false);
    };
    window.addEventListener('pointerdown', onPointerDown);
    return () => window.removeEventListener('pointerdown', onPointerDown);
  }, [addMenuOpen]);

  const resetMcpSelection = useCallback(() => {
    const nextIds = DEFAULT_MCP_SERVER_IDS;
    persistMcpSelection(nextIds, null);
    persistMcpEnabled(nextIds.length > 0);
  }, [persistMcpEnabled, persistMcpSelection]);

  const handleMcpServerToggle = useCallback((server: McpServerToolOption) => {
    const selected = selectedMcpServerIdSet.has(server.id);
    const nextIds = selected
      ? selectedMcpServerIds.filter(id => id !== server.id)
      : [...selectedMcpServerIds, server.id].slice(0, MAX_MCP_SERVER_SELECTION);
    let nextToolNames = selectedMcpToolNames;
    if (nextToolNames !== null) {
      const serverToolNames = new Set(server.toolOptions.map(tool => tool.runtimeName));
      nextToolNames = selected
        ? nextToolNames.filter(name => !serverToolNames.has(name))
        : [...new Set([...nextToolNames, ...serverToolNames])];
    }
    persistMcpSelection(nextIds, nextToolNames);
    if (!useMcp && nextIds.length > 0) persistMcpEnabled(true);
  }, [persistMcpEnabled, persistMcpSelection, selectedMcpServerIdSet, selectedMcpServerIds, selectedMcpToolNames, useMcp]);

  const handleMcpToolToggle = useCallback((server: McpServerToolOption, runtimeName: string) => {
    const allVisibleSelectedTools = mcpToolNamesForServers(mcpOptions, selectedMcpServerIds);
    const base = selectedMcpToolNames === null ? allVisibleSelectedTools : selectedMcpToolNames;
    const selected = base.includes(runtimeName);
    const nextToolNames = selected
      ? base.filter(name => name !== runtimeName)
      : [...new Set([...base, runtimeName])];
    const nextIds = selectedMcpServerIdSet.has(server.id)
      ? selectedMcpServerIds
      : [...selectedMcpServerIds, server.id].slice(0, MAX_MCP_SERVER_SELECTION);
    persistMcpSelection(nextIds, nextToolNames);
    if (!useMcp) persistMcpEnabled(true);
  }, [mcpOptions, persistMcpEnabled, persistMcpSelection, selectedMcpServerIdSet, selectedMcpServerIds, selectedMcpToolNames, useMcp]);

  const openMcpPicker = useCallback(() => {
    mcpReturnFocusEntryRef.current = 'tools';
    setMcpPickerOpen(true);
  }, []);

  const closeMcpPicker = useCallback(() => {
    setMcpPickerOpen(false);
    requestAnimationFrame(() => {
      addMenuRef.current
        ?.querySelector<HTMLButtonElement>(`[data-mcp-entry="${mcpReturnFocusEntryRef.current}"]`)
        ?.focus();
    });
  }, []);

  useEffect(() => {
    if (!mcpPickerOpen) return;
    requestAnimationFrame(() => mcpBackButtonRef.current?.focus());
  }, [mcpPickerOpen]);

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
    const selectedStillUsable = selectedModel && loadedModels.some(m => m.model_name === selectedModel && canSelectInComposer(m));
    const selectedDefault = lemonadeDefaultModel(selectedModel);
    if (selectedStillUsable || selectedDefault || !selectedModel || loadedModels.length === 0) return;
    const preferred = selectPreferredLoadedModel(loadedModels);
    if (preferred && canSelectInComposer(preferred)) onModelSelect(preferred.model_name);
  }, [loadedModels, onModelSelect, selectedModel]);

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

  const loadModelWithPolicy = useCallback(async (
    modelName: string,
    info: ModelInfo | null,
    recipeOptions?: Record<string, unknown>,
  ) => {
    const api = await getApiClient();
    let currentLoaded = loadedModels;
    try {
      currentLoaded = (await api.health()).all_models_loaded || loadedModels;
    } catch {
      // The render snapshot is still sufficient when a health refresh is not
      // available (for example during a short reconnect window).
    }
    const target = info || findModelInfoByName(knownModelInfos, modelName) || null;
    return loadWithGlobalModelPolicy({
      loadedModels: currentLoaded,
      allModels: knownModelInfos,
      target,
      pinnedNames: loadPinnedModelNames(),
      settings: globalModelSettings,
      unload: name => api.unloadModel(name),
      load: () => api.loadModel(modelName, recipeOptions, target),
    });
  }, [globalModelSettings, knownModelInfos, loadedModels]);

  const waitForExistingModelDownload = useCallback(async (modelName: string, convoId: string): Promise<boolean> => {
    const [{ downloadStore }, api] = await Promise.all([getDownloadStoreModule(), getApiClient()]);
    let sawDownload = false;
    const startedAt = Date.now();

    while (Date.now() - startedAt < 60 * 60 * 1000) {
      await downloadStore.refresh();
      const matching = downloadsForModel(downloadStore.snapshot(), modelName);
      const active = activeDownloadForModel(matching, modelName);
      if (active) {
        sawDownload = true;
        if (active.status === 'paused') {
          throw new Error(`Download for ${modelName} is paused. Resume it in Downloads, then send again.`);
        }
        setModelPreparations(prev => ({
          ...prev,
          [convoId]: {
            modelName,
            phase: 'waiting',
            percent: Number.isFinite(active.percent) ? active.percent : undefined,
          },
        }));
        await new Promise(resolve => window.setTimeout(resolve, 750));
        continue;
      }

      const terminal = [...matching]
        .filter(isDownloadTerminal)
        .sort((a, b) => b.updatedAt - a.updatedAt)[0];
      // A terminal row that predates this send is history, not a reason to make
      // the fallback permanently un-retryable. Only interpret terminal state
      // after this wait loop actually observed the active download.
      if (!sawDownload) return false;
      if (terminal?.status === 'completed') return true;
      if (terminal?.status === 'error') {
        throw new Error(terminal.error || `Download failed for ${modelName}.`);
      }
      if (terminal?.status === 'cancelled') {
        throw new Error(`Download for ${modelName} was cancelled.`);
      }

      const freshModels = await api.models(true).catch(() => ({ data: [] as ModelInfo[] }));
      const freshInfo = findModelInfoByName(freshModels.data, modelName);
      if (modelIsDownloaded(freshInfo)) return true;
      throw new Error(`Download for ${modelName} disappeared before it became ready.`);
    }

    throw new Error(`Download for ${modelName} did not finish.`);
  }, []);

  const ensureChatModelReady = useCallback(async (
    modelName: string,
    initialInfo: ModelInfo | null,
    convoId: string,
  ): Promise<ModelSnapshot> => {
    const [{ downloadStore }, api] = await Promise.all([getDownloadStoreModule(), getApiClient()]);
    const loadedFrom = (models: LoadedModel[]) => models.find(
      model => model.model_name.toLowerCase() === modelName.toLowerCase(),
    ) || null;

    let health = await api.health();
    let loaded = loadedFrom(health.all_models_loaded || []);
    if (loaded) {
      saveLastReadyModelName(loaded.model_name);
      setLastReadyModelName(loaded.model_name);
      return snapshotFromLoaded(loaded) || snapshotFromModelInfo(initialInfo) || snapshotFromName(modelName, [loaded])!;
    }

    let freshModels = await api.models(true).catch(() => ({ data: knownModelInfos }));
    let info = findModelInfoByName(freshModels.data, modelName) || initialInfo;
    const existingFinished = await waitForExistingModelDownload(modelName, convoId);
    if (existingFinished) {
      freshModels = await api.models(true).catch(() => freshModels);
      info = findModelInfoByName(freshModels.data, modelName) || info;
    }

    if (!modelIsDownloaded(info)) {
      let pullError: Error | null = null;
      let pullCompleted = false;
      downloadStore.markLocal(modelName, 'downloading', 'model');
      setModelPreparations(prev => ({ ...prev, [convoId]: { modelName, phase: 'downloading', percent: 0 } }));

      await api.pullModel(modelName, {
        onProgress: data => {
          const item = downloadStore.upsertFromPull(modelName, data as Record<string, unknown>, 'model');
          setModelPreparations(prev => ({
            ...prev,
            [convoId]: {
              modelName,
              phase: 'downloading',
              percent: item?.percent ?? (typeof data.percent === 'number' ? data.percent : undefined),
            },
          }));
        },
        onComplete: data => {
          pullCompleted = true;
          downloadStore.upsertFromPull(modelName, {
            ...(data as Record<string, unknown>),
            status: 'completed',
            complete: true,
            percent: 100,
          }, 'model');
        },
        onError: error => {
          pullError = error;
          downloadStore.upsertFromPull(modelName, {
            status: 'error',
            error: friendlyErrorMessage(error),
          }, 'model');
        },
      });

      if (pullError) throw pullError;
      freshModels = await api.models(true).catch(() => freshModels);
      info = findModelInfoByName(freshModels.data, modelName) || info;
      if (!pullCompleted && !modelIsDownloaded(info)) {
        throw new Error(`Lemonade could not finish downloading ${modelName}.`);
      }
    }

    setModelPreparations(prev => ({ ...prev, [convoId]: { modelName, phase: 'loading', percent: 100 } }));
    await loadModelWithPolicy(modelName, info || initialInfo);
    await Promise.resolve(onRefresh());
    health = await api.health();
    loaded = loadedFrom(health.all_models_loaded || []);
    if (!loaded) throw new Error(`${modelName} was downloaded but did not become ready for chat.`);

    saveLastReadyModelName(loaded.model_name);
    setLastReadyModelName(loaded.model_name);
    setFallbackModelOverride(null);
    onModelSelect(loaded.model_name);
    return snapshotFromLoaded(loaded)
      || snapshotFromModelInfo(info || initialInfo)
      || snapshotFromName(modelName, [loaded])!;
  }, [knownModelInfos, loadModelWithPolicy, onModelSelect, onRefresh, waitForExistingModelDownload]);

  const speakWithPinnedTts = useCallback(async (text: string, source: 'assistant' | 'user', force = false) => {
    const trimmed = text.trim();
    const modelName = ttsPlaybackSettings.modelName;
    if (!trimmed || !modelName) return;
    if (!force) {
      if (ttsPlaybackSettings.playbackMode !== 'always') return;
      if (source === 'user' && !ttsPlaybackSettings.speakUserText) return;
    }
    try {
      const api = await getApiClient();
      const isLoaded = loadedModels.some(model => model.model_name.toLowerCase() === modelName.toLowerCase());
      if (!isLoaded) {
        await loadModelWithPolicy(modelName, findModelInfoByName(knownModelInfos, modelName) || null);
      }
      const modelInfo = findModelInfoByName(knownModelInfos, modelName);
      const modelRecipe = String(
        (modelInfo as any)?.recipe
        || ((Array.isArray(modelInfo?.recipes) && modelInfo?.recipes?.[0]) ? (modelInfo.recipes[0] as any).recipe : ''),
      ).toLowerCase();
      const { loadModelTuning } = await import(
        /* webpackChunkName: "model-configuration" */ '../modelConfiguration'
      );
      const directOptions = loadModelTuning(modelName)?.recipe_options || {};
      const voice = modelRecipe.includes('openmoss')
        ? String(directOptions.voice || '')
        : ttsVoiceFromRecipeOptions(directOptions);
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
  }, [knownModelInfos, loadModelWithPolicy, loadedModels, stopAutoSpeech, ttsPlaybackSettings.modelName, ttsPlaybackSettings.playbackMode, ttsPlaybackSettings.speakUserText]);

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
  const modelPreparation = activeId ? modelPreparations[activeId] || null : null;
  const capabilityBusy = activeId ? capabilityBusyConvoIds.has(activeId) : false;
  const isBusy = isStreaming || capabilityBusy || isLiveRecording || modelPreparation !== null;
  const streamingContent = currentStream?.content || '';
  const streamingThinking = currentStream?.thinking || '';
  const streamingToolStatus = currentStream?.toolStatus || '';
  const streamingToolCalls = currentStream?.toolCalls || [];
  const currentLiveStats = activeId ? streaming.getLiveStats(activeId) : undefined;

  const clearModelPreparation = useCallback((convoId: string) => {
    setModelPreparations(prev => {
      if (!prev[convoId]) return prev;
      const next = { ...prev };
      delete next[convoId];
      return next;
    });
  }, []);

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
    if (!historyHydrated) return;
    saveConversations(conversations, persistHistory);
    try { localStorage.setItem(storageKey('persist_conversations'), String(persistHistory)); } catch { /* ignore */ }
  }, [conversations, historyHydrated, persistHistory]);

  // Persist active conversation id only after the initial stored value has been
  // hydrated, otherwise an empty cold-start state could overwrite it.
  useEffect(() => {
    if (!historyHydrated) return;
    saveActiveId(activeId, persistHistory);
  }, [activeId, historyHydrated, persistHistory]);

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
      const api = await getApiClient();
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
    setCapabilityBusyConvoIds(prev => new Set(prev).add(convoId));
    try {
      const api = await getApiClient();
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
            await loadModelWithPolicy(model3dSettings.imageModel, imageInfo);
          }
          const references = await api.imageGeneration(
            model3dSettings.imageModel,
            `${text.trim()} -- ${MODEL3D_REFERENCE_PROMPT}`,
            { n: 1, size: '1024x1024' },
          );
          referenceImage = references[0];
          generatedReference = [referenceImage];
          await loadModelWithPolicy(model.name, findModelInfoByName(knownModelInfos, model.name) || null);
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
        const { loadModelTuning } = await import(
          /* webpackChunkName: "model-configuration" */ '../modelConfiguration'
        );
        let voice = ttsVoiceFromRecipeOptions(loadModelTuning(model.name)?.recipe_options || {});
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
                await loadModelWithPolicy(
                  openMossVoiceDesignModel,
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
            await loadModelWithPolicy(targetModel, findModelInfoByName(knownModelInfos, targetModel) || null);
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
      setCapabilityBusyConvoIds(prev => {
        if (!prev.has(convoId)) return prev;
        const next = new Set(prev);
        next.delete(convoId);
        return next;
      });
    }
  }, [
    appendAssistantMessage, audioGenerationSettings, imageMode, imageSettings,
    isOpenMossTts, knownModelInfos, loadedModels, model3dSettings, onRefresh,
    loadModelWithPolicy, openMossCloneModel, openMossSettings, openMossVoiceDesignModel,
    speakWithPinnedTts, trackGeneratedMediaUrl,
  ]);

  const startAssistantResponse = useCallback(async (
    convoId: string,
    modelSnapshot: ModelSnapshot,
    userMessage: Message,
    priorMessages: Message[],
    audioFiles: File[],
    appendUserToConversation: boolean,
  ) => {
    const api = await getApiClient();
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

    if (hasImages && modeSupportsChatCompletions && !collectionInfo && !supportsChatImageInput) {
      appendAssistantMessage(convoId, {
        content: friendlyChatError('The selected text model does not support image input. Choose a vision-capable model to send images.'),
        model: modelSnapshot,
        isError: true,
      });
      return;
    }

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

    let omniRuntime: ChatToolRuntime | null = null;
    if (collectionInfo) {
      const { buildOmniToolRuntime } = await import(
        /* webpackChunkName: "omni-tools" */ '../tools/omniTools'
      );
      omniRuntime = buildOmniToolRuntime(collectionInfo, knownModelInfos, {
        attachedImages: images || [],
        attachedAudioFiles: audioFiles,
        previousImages: collectConversationImages(priorMessages),
      });
    }

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
        const { buildSelectedMcpRuntime } = await import(
          /* webpackChunkName: "mcp-runtime" */ '../tools/mcpRuntime'
        );
        selectedMcpRuntime = await buildSelectedMcpRuntime(
          selectedMcpServerIds,
          {
            attachedImages: images || [],
            attachedAudioFiles: audioFiles,
            previousImages: collectConversationImages(priorMessages),
          },
          selectedMcpToolNames || undefined,
        );
      } catch (err) {
        // MCP availability must never dead-end the chat composer. Surface the
        // failure, switch MCP off for this chat, and continue the same request
        // as a normal model completion without tools.
        persistMcpEnabled(false);
        try { localStorage.setItem(scopedKey(MCP_ENABLED_KEY), 'false'); } catch { /* ignore */ }
        appendAssistantMessage(convoId, {
          content: friendlyChatError(`MCP setup failed and was switched off for this chat. Continuing without tools: ${friendlyErrorMessage(err)}`),
          model: modelSnapshot,
          isError: true,
        });
        selectedMcpRuntime = null;
      }
    }

    const activeToolRuntimes = [omniRuntime, selectedMcpRuntime].filter(
      (runtime): runtime is ChatToolRuntime => !!runtime && runtime.tools.length > 0,
    );
    let toolRuntime: ChatToolRuntime | null = activeToolRuntimes[0] || null;
    if (activeToolRuntimes.length > 1) {
      const { composeMcpRuntimes } = await import(
        /* webpackChunkName: "mcp-runtime" */ '../tools/mcpRuntime'
      );
      toolRuntime = composeMcpRuntimes(activeToolRuntimes);
    }

    // Build chat history from the conversation's messages before this user prompt.
    // Do not feed prior friendly UI error messages or generated media artifacts back as assistant context.
    const chatMessages: ChatMessage[] = [];

    const systemPrompts: string[] = [];
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
      if (m.images?.length && supportsChatImageInput) {
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
    modeSupportsMcp,
    canUseAudioInput,
    persistMcpEnabled,
    selectedMcpServerIds,
    selectedMcpToolNames,
    supportsChatImageInput,
    runCapabilityRequest,
    speakWithPinnedTts,
    streaming,
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
    if (!canSubmitContent || isBusy || !currentModelSnapshot) return;

    let convoId = activeId;
    const initialSnapshot = currentModelSnapshot;
    const currentMessages = (conversations.find(c => c.id === convoId)?.messages || []);
    const userMessage: Message = {
      role: 'user',
      content: text || (audioFiles[0] ? `Audio file: ${audioFiles[0].name}` : ''),
      images: hasImages ? [...pendingImages] : undefined,
      audioName: audioFiles[0]?.name,
      model: initialSnapshot,
    };

    if (!convoId) {
      const newConvo: Conversation = {
        id: generateId(),
        title: titleFromInput(text, hasImages, audioFiles),
        model: initialSnapshot,
        messages: [userMessage],
        updatedAt: Date.now(),
        schemaVersion: STORAGE_VERSION,
      };
      convoId = newConvo.id;
      setConversations(prev => [newConvo, ...prev]);
      setActiveId(convoId);
    } else {
      updateConversation(convoId, conversation => ({
        ...conversation,
        messages: [...conversation.messages, userMessage],
        model: initialSnapshot,
        title: conversation.messages.length === 0 ? titleFromInput(text, hasImages, audioFiles) : conversation.title,
        updatedAt: Date.now(),
      }));
    }

    setInputValue('');
    setPendingImages([]);
    setPendingAudioFiles([]);
    void speakWithPinnedTts(text, 'user');

    if (connectionStatus !== 'connected') {
      appendAssistantMessage(convoId, {
        content: friendlyChatError('Lemonade Server is not connected. Reconnect the server, then retry this message.'),
        model: initialSnapshot,
        isError: true,
      });
      requestAnimationFrame(() => inputRef.current?.focus());
      return;
    }

    try {
      const preparedSnapshot = await ensureChatModelReady(currentModel, currentKnownModelInfo, convoId);
      updateConversation(convoId, conversation => ({
        ...conversation,
        model: preparedSnapshot,
        messages: conversation.messages.map((message, index) => (
          index === conversation.messages.length - 1 && message.role === 'user'
            ? { ...message, model: preparedSnapshot }
            : message
        )),
        updatedAt: Date.now(),
      }));
      clearModelPreparation(convoId);
      await startAssistantResponse(
        convoId,
        preparedSnapshot,
        { ...userMessage, model: preparedSnapshot },
        currentMessages,
        audioFiles,
        false,
      );
    } catch (error) {
      appendAssistantMessage(convoId, {
        content: friendlyChatError(friendlyErrorMessage(error)),
        model: initialSnapshot,
        isError: true,
      });
    } finally {
      clearModelPreparation(convoId);
      requestAnimationFrame(() => inputRef.current?.focus());
    }
  };

  const handleRetryAssistant = useCallback(async (messageIndex: number) => {
    if (!activeId || isBusy) return;
    if (connectionStatus !== 'connected' || !currentModel || !currentModelSnapshot) return;
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
  }, [activeId, appendAssistantMessage, connectionStatus, conversations, currentModel, currentModelSnapshot, isBusy, startAssistantResponse]);

  const handleSpeakAssistantMessage = useCallback((text: string) => {
    void speakWithPinnedTts(text, 'assistant', true);
  }, [speakWithPinnedTts]);

  const canReadAssistantMessages = Boolean(ttsPlaybackSettings.modelName);

  const handleEditUserMessage = useCallback(async (messageIndex: number, revisedContent: string) => {
    const text = revisedContent.trim();
    if (!text || !activeId || isBusy) return;
    if (connectionStatus !== 'connected' || !currentModel || !currentModelSnapshot) return;
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
  }, [activeId, connectionStatus, conversations, currentModel, currentModelSnapshot, isBusy, startAssistantResponse]);

  // Keep option-button callbacks stable across unrelated Chat state updates. This
  // lets memoized completed Markdown messages retain their DOM and selection.
  handleSendRef.current = handleSend;

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  // ── Attachment handling ────────────────────────────────────

  const acceptsImageAttachments = supportsChatImageInput
    || (currentCapability === 'image' && imageMode === 'edit')
    || (currentCapability === 'model3d' && model3dSettings.sourceMode === 'image');
  const acceptsAudioAttachments = canUseAudioInput
    || (isOpenMossTts && openMossSettings.mode === 'clone');

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
    if (!acceptsImageAttachments) return;
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
    acceptsImageAttachments, canUseAudioInput, currentCapability, imageMode, isOpenMossTts,
    modeSupportsChatCompletions, model3dSettings.sourceMode,
    openMossSettings.mode, pendingImages.length,
  ]);

  const handlePaste = useCallback((e: React.ClipboardEvent) => {
    const items = e.clipboardData?.items;
    if (!items) return;
    const files: File[] = [];
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      if ((acceptsImageAttachments && item.type.startsWith('image/'))
        || (acceptsAudioAttachments && item.type.startsWith('audio/'))) {
        const file = item.getAsFile();
        if (file) files.push(file);
      }
    }
    if (files.length > 0) {
      e.preventDefault();
      addAttachments(files);
    }
  }, [acceptsAudioAttachments, acceptsImageAttachments, addAttachments]);

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

  useEffect(() => {
    const onPreferenceChange = (event: Event) => {
      setPersistHistory((event as CustomEvent<boolean>).detail);
    };
    window.addEventListener(CHAT_HISTORY_PREFERENCE_EVENT, onPreferenceChange);
    return () => window.removeEventListener(CHAT_HISTORY_PREFERENCE_EVENT, onPreferenceChange);
  }, []);

  const handleModelPickerUnload = useCallback(async (modelName: string, e: React.MouseEvent) => {
    e.stopPropagation();
    if (modelPickerUnloading) return;
    setModelPickerUnloading(modelName);
    try {
      const api = await getApiClient();
      await api.unloadModel(modelName);
      await Promise.resolve(onRefresh());
      setUnloadAnnouncement(`${modelName} unloaded`);
    } finally {
      setModelPickerUnloading(null);
    }
  }, [modelPickerUnloading, onRefresh]);

  const handleLoadedCardUnload = useCallback((modelName: string) => {
    if (modelPickerUnloading) return;
    setModelPickerUnloading(modelName);
    void (async () => {
      try {
        const api = await getApiClient();
        await api.unloadModel(modelName);
        await Promise.resolve(onRefresh());
        setUnloadAnnouncement(`${modelName} unloaded`);
      } finally {
        setModelPickerUnloading(null);
      }
    })();
  }, [modelPickerUnloading, onRefresh]);

  const handleModelPickerSelect = useCallback(async (option: ModelPickerOption) => {
    if (option.loaded) {
      setFallbackModelOverride(null);
      onModelSelect(option.name);
      setModelPickerOpen(false);
      setModelPickerQuery('');
      return;
    }

    if (option.deferredUntilSend) {
      const configuredDefault = lemonadeDefaultModel(option.name);
      setFallbackModelOverride(option.name);
      if (configuredDefault) {
        const preferred = savePreferredDefaultModelName(configuredDefault.name);
        setPreferredDefaultModelName(preferred);
      }
      onModelSelect(option.name);
      setModelPickerError(null);
      setModelPickerOpen(false);
      setModelPickerQuery('');
      return;
    }

    if (connectionStatus !== 'connected' || modelPickerLoading) return;
    const { downloadStore } = await getDownloadStoreModule();
    if (activeDownloadForModel(downloadStore.snapshot(), option.name)) {
      setModelPickerError(`${option.name} is still downloading. Wait for the download to finish before loading it.`);
      return;
    }
    setModelPickerError(null);
    setModelPickerLoading(option.name);
    setFallbackModelOverride(option.name);
    onModelSelect(option.name);
    try {
      await loadModelWithPolicy(option.name, option.info || null);
      await Promise.resolve(onRefresh());
      setFallbackModelOverride(null);
      onModelSelect(option.name);
      setModelPickerOpen(false);
      setModelPickerQuery('');
    } catch (err) {
      setFallbackModelOverride(null);
      if (currentModel) onModelSelect(currentModel);
      setModelPickerError(friendlyErrorMessage(err));
      setModelPickerOpen(true);
    } finally {
      setModelPickerLoading(null);
    }
  }, [connectionStatus, currentModel, loadModelWithPolicy, modelPickerLoading, onModelSelect, onRefresh]);

  // ── Option select from assistant messages ───────────────────

  const handleOptionSelect = useCallback((text: string) => {
    void handleSendRef.current(text);
  }, []);

  const hasMessages = messages.length > 0 || isStreaming || capabilityBusy || modelPreparation !== null;
  const isOpenMossCloneMode = isOpenMossTts && openMossSettings.mode === 'clone';
  const canAttach = acceptsImageAttachments || acceptsAudioAttachments;
  const imageAttachmentLimitReached = acceptsImageAttachments
    && !acceptsAudioAttachments
    && pendingImages.length >= MAX_IMAGES;
  const fileAccept = isOpenMossCloneMode
    ? 'audio/wav,audio/x-wav,.wav'
    : currentCapability === 'model3d'
      ? 'image/png,image/jpeg,image/bmp,image/gif,.png,.jpg,.jpeg,.bmp,.gif'
      : currentCapability === 'image'
        ? 'image/*'
        : acceptsImageAttachments && acceptsAudioAttachments
          ? 'image/*,audio/*'
          : acceptsImageAttachments
            ? 'image/*'
            : acceptsAudioAttachments
              ? 'audio/*'
              : '';
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
      : currentCapability === 'chat' && supportsChatImageInput && supportsChatAudioInput
        ? `Message ${currentModel} with text, images, or audio…`
      : currentCapability === 'chat' && supportsChatImageInput
        ? `Message ${currentModel} with text or images…`
      : currentCapability === 'chat' && supportsChatAudioInput
        ? `Message ${currentModel} with text or audio…`
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
  const composerHint = modelPreparation
    ? (modelPreparation.phase === 'loading'
      ? `Loading ${modelPreparation.modelName} for chat…`
      : `${modelPreparation.phase === 'waiting' ? 'Waiting for' : 'Downloading'} ${modelPreparation.modelName}${Number.isFinite(modelPreparation.percent) ? ` · ${Math.round(modelPreparation.percent!)}%` : ''}…`)
    : supportsChatAudioInput && modeSupportsChatCompletions
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
        data-startup-ready="chat"
      >
      {/* Conversation rail */}
      <aside className={`rail workspace-rail${railExpanded ? '' : ' is-collapsed'}`}>
        <WorkspaceRailHeader
          title="History"
          sidebarLabel="conversation"
          purpose="history"
          collapsed={!railExpanded}
          onToggle={handleRailToggle}
        />

        <div className="rail__new-wrap">
          <button type="button" className="btn btn--primary btn--medium workspace-action-button workspace-action-button--primary workspace-action-button--medium rail__new" onClick={handleNewChat} aria-label="New chat">
            <Icon name="compose" size={14} aria-hidden="true" />
            <span className="workspace-action-button__label">New chat</span>
          </button>
        </div>

        <ul className="rail__list" role="listbox" aria-label="Conversations" onKeyDown={handleRailKeyDown}>
          {conversations.map((c, idx) => {
            const badge = modelModeBadge(c.model?.capability || 'chat', c.model?.recipe);
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

      </aside>

      {/* Mobile bottom sheet for conversations */}
      {mobileSheetOpen && (
        <div className="bottom-sheet-backdrop" onClick={closeMobileSheet} aria-hidden="true" />
      )}
      <div
        ref={bottomSheetRef}
        id="conversation-history-panel"
        className={`bottom-sheet ${mobileSheetOpen ? 'bottom-sheet--open' : ''}`}
        role={mobileSheetOpen ? 'dialog' : undefined}
        aria-label="Conversations"
        aria-modal={mobileSheetOpen ? true : undefined}
        aria-hidden={!mobileSheetOpen}
      >
        <div className="bottom-sheet__handle" ref={sheetHandleRef} aria-hidden="true">
          <div className="bottom-sheet__handle-pill" />
        </div>
        <div className="bottom-sheet__header">
          <strong>Conversations</strong>
          <button type="button" className="btn btn--quiet btn--toolbar btn--icon-only workspace-action-button workspace-action-button--quiet workspace-action-button--toolbar workspace-action-button--icon-only" onClick={closeMobileSheet} aria-label="Close conversation history" title="Close panel">
            <Icon name="x" size={16} aria-hidden="true" />
          </button>
        </div>
        <button type="button" className="btn btn--primary btn--medium workspace-action-button workspace-action-button--primary workspace-action-button--medium bottom-sheet__new" onClick={() => { handleNewChat(); closeMobileSheet(); }}>
          <Icon name="compose" size={14} aria-hidden="true" />
          <span className="workspace-action-button__label">New chat</span>
        </button>
        <ul className="bottom-sheet__list rail__list" role="listbox" aria-label="Conversations" onKeyDown={handleSheetKeyDown}>
          {conversations.map((c, idx) => {
            const badge = modelModeBadge(c.model?.capability || 'chat', c.model?.recipe);
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
        <WorkspaceMobileMenuButton
          menuLabel="Open conversation history"
          panelId="conversation-history-panel"
          expanded={mobileSheetOpen}
          onClick={() => { if (mobileSheetOpen) closeMobileSheet(); else setMobileSheetOpen(true); }}
          triggerRef={sheetTriggerRef}
        />
        <div className="chat__inner">
          {!hasMessages ? (
            <EmptyState
              loadedModels={loadedModels}
              currentModel={currentModel}
              onModelSelect={onModelSelect}
              onOpenModelDetails={onOpenModelDetails}
              onUnloadModel={handleLoadedCardUnload}
              unloadingModel={modelPickerUnloading}
              onChipClick={startLemonadeToolPrompt}
              customModelInfos={customModelInfos}
            />
          ) : (
            <div className="thread">
              {messages.map((msg, i) => (
                <MessageBubble
                  key={i}
                  message={msg}
                  activeModel={currentModelSnapshot}
                  userLabel="You"
                  defaultThinkingOpen={!globalModelSettings.collapseThinkingByDefault}
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

              {modelPreparation && !isStreaming && (
                <article className="message message--assistant" data-model-preparation={modelPreparation.phase}>
                  <div className="message__avatar"><Icon name="download" size={16} /></div>
                  <div className="message__body">
                    <div className="message__author-row">
                      <div className="message__author">Lemonade</div>
                    </div>
                    <div className="message__content message__content--pending">
                      <span className="streaming-cursor" aria-hidden="true" />
                      {modelPreparation.phase === 'loading'
                        ? `Loading ${modelPreparation.modelName} for chat…`
                        : `${modelPreparation.phase === 'waiting' ? 'Waiting for' : 'Downloading'} ${modelPreparation.modelName}${Number.isFinite(modelPreparation.percent) ? ` · ${Math.round(modelPreparation.percent!)}%` : ''}…`}
                    </div>
                  </div>
                </article>
              )}

              {capabilityBusy && !isStreaming && !modelPreparation && (
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
          <Suspense fallback={<div className="view-loading view-loading--compact"><span className="spinner" aria-hidden="true" /></div>}>
            <LogViewer />
          </Suspense>
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
                onClick={() => { setModelPickerOpen(v => !v); setModelPickerError(null); }}
                aria-haspopup="listbox"
                aria-expanded={modelPickerOpen}
              >
                {currentLoadedModel ? (
                  isRouterRecipe(currentRecipe) ? (
                    <span className="composer__model-mode composer__model-mode--router">
                      <Icon name="router" size={14} aria-hidden="true" />
                      <span>Router</span>
                    </span>
                  ) : (
                    <span className={`composer__model-mode composer__model-mode--${capabilityBadge(currentCapability)}`}>
                      <ModelModeIcons capability={currentCapability} audioInput={supportsChatAudioInput} size={14} />
                      <span>{modelModeLabel(currentCapability, supportsChatAudioInput)}</span>
                    </span>
                  )
                ) : (
                  <ModelModeIcons capability={currentCapability} recipe={currentRecipe} audioInput={supportsChatAudioInput} size={14} />
                )}
                {!currentLoadedModel && currentDefaultModel && (
                  <span className="composer__model-default-icon" title={currentDefaultModel.label} aria-label={currentDefaultModel.label}>
                    <Icon name={currentDefaultModel.icon} size={13} />
                  </span>
                )}
                <span className="composer__model-button-name">{currentModel}</span>
                {currentLoadedModel && currentDefaultModel && (
                  <span className="composer__model-default-icon" title={currentDefaultModel.label} aria-label={currentDefaultModel.label}>
                    <Icon name={currentDefaultModel.icon} size={13} />
                  </span>
                )}
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
                      placeholder="Search ready or Lemonade default models…"
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
                          disabled={modelPickerLoading === option.name || modelPickerUnloading === option.name}
                          role="option"
                          aria-selected={option.name === currentModel}
                        >
                          <ModelModeIcons capability={option.capability} recipe={option.recipe} audioInput={option.audioInput} size={15} />
                          {option.defaultIcon && option.defaultLabel && (
                            <span className="composer__model-default-icon" title={option.defaultLabel} aria-label={option.defaultLabel}>
                              <Icon name={option.defaultIcon} size={14} />
                            </span>
                          )}
                          <span className="composer__model-option-text">
                            <strong>{option.name}</strong>
                            <span>{modelModeDisplayLabel(option.capability, option.audioInput, option.recipe)} · {option.detail}</span>
                          </span>
                          {modelPickerLoading === option.name && <span className="composer__model-option-loading">Loading…</span>}
                        </button>
                        {option.loaded && (
                          <button
                            type="button"
                            className="composer__model-option-unload"
                            onClick={(e) => handleModelPickerUnload(option.name, e)}
                            disabled={!!modelPickerUnloading || !!modelPickerLoading}
                            aria-label={`Eject model ${option.name}`}
                            title="Eject model"
                          >
                            {modelPickerUnloading === option.name ? '…' : <Icon name="eject" size={16} aria-hidden="true" />}
                          </button>
                        )}
                      </div>
                    ))}
                    {modelPickerOptions.length === 0 && <div className="composer__model-empty">No matching models</div>}
                  </div>
                  {modelPickerLoading && <div className="composer__model-loading-bar">Loading {modelPickerLoading}…</div>}
                  {modelPickerError && <div className="composer__model-error">{modelPickerError}</div>}
                </div>
              )}
            </div>
          )}
          {currentModel && currentCapability !== 'image' && (
            <button
              type="button"
              className="composer__tools-toggle composer__effective-settings"
              onClick={() => setEffectiveSettingsOpen(true)}
              title="Effective settings"
              aria-label="Effective settings"
            >
              <Icon name="sliders-horizontal" size={13} />
            </button>
          )}
          <button
            className={`composer__tools-toggle ${showInlineLogs ? 'composer__tools-toggle--active' : ''}`}
            onClick={() => setShowInlineLogs(v => !v)}
            aria-pressed={showInlineLogs}
            title="Show logs next to the chat"
          >
            <Icon name="logs" size={13} /> Logs
          </button>
        </div>
        {currentModel && effectiveSettingsOpen && (
          <Suspense fallback={null}>
            <EffectiveSettingsModal
            open={effectiveSettingsOpen}
            onClose={() => setEffectiveSettingsOpen(false)}
            modelName={currentModel}
            modelInfo={currentKnownModelInfo || currentCustomModelInfo || null}
            recipe={currentRecipe}
            mcpEnabled={useMcp}
            mcpServerIds={selectedMcpServerIds}
            fallbackCtxSize={serverDefaultCtxSize}
            loadedModel={currentLoadedModel}
            isModelLoaded={!!currentLoadedModel}
            onReload={async () => {
              const api = await getApiClient();
              await api.reloadModel(currentModel, undefined, currentKnownModelInfo || currentCustomModelInfo || null);
              await Promise.resolve(onRefresh());
            }}
            onLoad={async () => {
              const api = await getApiClient();
              await api.loadModel(currentModel, undefined, currentKnownModelInfo || currentCustomModelInfo || null);
              await Promise.resolve(onRefresh());
            }}
            />
          </Suspense>
        )}
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
          <div className="composer__add" ref={addMenuRef}>
            <button
              className="composer__attach composer__add-trigger"
              onClick={() => {
                setAddMenuOpen(open => {
                  const next = !open;
                  if (!next) setMcpPickerOpen(false);
                  return next;
                });
              }}
              disabled={!currentModel || isBusy || (!modeSupportsMcp && (!canAttach || imageAttachmentLimitReached))}
              title="Add files, photos, or tools"
              aria-label="Add files, photos, or tools"
              aria-haspopup={mcpPickerOpen ? 'dialog' : 'menu'}
              aria-expanded={addMenuOpen}
            >
              <Icon name="plus" size={18} />
            </button>
            {addMenuOpen && !mcpPickerOpen && (
              <div className="composer__add-menu" role="menu" aria-label="Add to chat">
                <button
                  type="button"
                  className="composer__add-row"
                  role="menuitem"
                  onClick={() => {
                    fileInputRef.current?.click();
                    setAddMenuOpen(false);
                  }}
                  disabled={!canAttach || !currentModel || isBusy || imageAttachmentLimitReached}
                >
                  <span className="composer__add-icon"><Icon name="paperclip" size={16} /></span>
                  <span className="composer__add-text">
                    <strong>Add photos & files</strong>
                    <small>{isOpenMossCloneMode
                      ? 'Upload a WAV voice sample'
                      : currentCapability === 'model3d'
                        ? 'Upload a reference image'
                        : acceptsImageAttachments && acceptsAudioAttachments
                          ? 'Upload images or audio'
                          : acceptsImageAttachments
                            ? 'Upload images'
                            : acceptsAudioAttachments
                              ? 'Upload audio'
                              : 'Not available for this model'}</small>
                  </span>
                </button>
                <button
                  type="button"
                  className={`composer__add-row${mcpPickerOpen ? ' is-active' : ''}`}
                  role="menuitem"
                  data-mcp-entry="tools"
                  onClick={openMcpPicker}
                  disabled={!modeSupportsMcp}
                  aria-label="Tools"
                  aria-haspopup="dialog"
                >
                  <span className="composer__add-icon"><Icon name="tools" size={16} /></span>
                  <span className="composer__add-text">
                    <strong>Tools</strong>
                    <small>{useMcp
                      ? `${selectedMcpToolCount} selected · Lemonade and external MCP`
                      : 'Lemonade tools and external MCP servers'}</small>
                  </span>
                </button>
              </div>
            )}
            {addMenuOpen && mcpPickerOpen && (
              <div
                className="composer__mcp-modal"
                onMouseDown={event => {
                  if (event.target === event.currentTarget) closeMcpPicker();
                }}
              >
                <div
                  className="composer__mcp-menu composer__mcp-menu--modal"
                  role="dialog"
                  aria-modal="true"
                  aria-labelledby="composer-mcp-dialog-title"
                  onKeyDown={event => {
                    if (event.key === 'Escape') {
                      event.preventDefault();
                      closeMcpPicker();
                    }
                  }}
                >
                  <button ref={mcpBackButtonRef} type="button" className="composer__mcp-back" onClick={closeMcpPicker} aria-label="Back to add to chat options">
                    <span aria-hidden="true">←</span>
                    <span>Back</span>
                  </button>
                  <div className="composer__mcp-header">
                    <label className="composer__mcp-master">
                      <input
                        type="checkbox"
                        checked={useMcp && modeSupportsMcp}
                        disabled={!modeSupportsMcp}
                        onChange={event => persistMcpEnabled(event.target.checked)}
                      />
                      <span>
                        <strong id="composer-mcp-dialog-title">Tools for this chat</strong>
                        <small>{selectedMcpServerIds.length} server{selectedMcpServerIds.length === 1 ? '' : 's'} · {selectedMcpToolCount} tool{selectedMcpToolCount === 1 ? '' : 's'}</small>
                      </span>
                    </label>
                    <button type="button" className="btn btn--ghost" onClick={resetMcpSelection}>Built-in default</button>
                  </div>
                  <div className="composer__mcp-tabs" role="tablist" aria-label="Tool providers">
                    <button
                      type="button"
                      role="tab"
                      aria-selected={mcpPickerTab === 'lemonade'}
                      className={`composer__mcp-tab${mcpPickerTab === 'lemonade' ? ' is-active' : ''}`}
                      onClick={() => setMcpPickerTab('lemonade')}
                    >
                      Lemonade tools
                    </button>
                    <button
                      type="button"
                      role="tab"
                      aria-selected={mcpPickerTab === 'external'}
                      className={`composer__mcp-tab${mcpPickerTab === 'external' ? ' is-active' : ''}`}
                      onClick={() => setMcpPickerTab('external')}
                    >
                      External MCP servers
                    </button>
                  </div>
                  {mcpPickerLoading ? (
                    <p className="composer__mcp-empty">Loading MCP tools…</p>
                  ) : mcpPickerError ? (
                    <div className="composer__mcp-error" role="alert">{mcpPickerError}</div>
                  ) : visibleMcpOptions.length === 0 ? (
                    <p className="composer__mcp-empty">
                      {mcpPickerTab === 'external' ? 'No external MCP servers are connected.' : 'No Lemonade tools available.'}
                    </p>
                  ) : (
                    <div className="composer__mcp-servers">
                      {visibleMcpOptions.map(server => {
                        const serverSelected = selectedMcpServerIdSet.has(server.id);
                        return (
                          <section key={server.id} className={`composer__mcp-server${serverSelected ? ' is-selected' : ''}`}>
                            <label className="composer__mcp-server-row">
                              <input
                                type="checkbox"
                                checked={serverSelected}
                                disabled={!useMcp || (!serverSelected && selectedMcpServerIds.length >= MAX_MCP_SERVER_SELECTION)}
                                onChange={() => handleMcpServerToggle(server)}
                              />
                              <span className={`composer__mcp-status${server.connected ? ' is-connected' : ''}`} aria-hidden="true" />
                              <span className="composer__mcp-server-text">
                                <strong>{server.name}</strong>
                                <small>{server.transport === 'builtin'
                                  ? 'Built in'
                                  : `${server.transport === 'streamable-http' ? 'HTTP endpoint' : 'Local process'} · ${server.status}`} · {server.toolOptions.length || server.tools} tool{(server.toolOptions.length || server.tools) === 1 ? '' : 's'}</small>
                              </span>
                            </label>
                            {serverSelected && (
                              <div className="composer__mcp-tools">
                                {server.toolOptions.length === 0 ? (
                                  <p className="composer__mcp-empty">No tools discovered for this server.</p>
                                ) : server.toolOptions.map(tool => {
                                  const toolSelected = selectedMcpToolNameSet === null || selectedMcpToolNameSet.has(tool.runtimeName);
                                  return (
                                    <label key={tool.runtimeName} className="composer__mcp-tool" title={tool.description || tool.title || tool.name}>
                                      <input
                                        type="checkbox"
                                        checked={toolSelected}
                                        disabled={!useMcp}
                                        onChange={() => handleMcpToolToggle(server, tool.runtimeName)}
                                      />
                                      <span>
                                        <strong>{tool.title || tool.name}</strong>
                                        {tool.runtimeName !== tool.name && <small>{tool.runtimeName}</small>}
                                      </span>
                                    </label>
                                  );
                                })}
                              </div>
                            )}
                          </section>
                        );
                      })}
                    </div>
                  )}
                  <div className="composer__mcp-footer">
                    <button type="button" className="btn btn--ghost" onClick={() => persistMcpSelection(selectedMcpServerIds, null)} disabled={selectedMcpToolNames === null}>Select all tools for selected servers</button>
                  </div>
                </div>
              </div>
            )}
          </div>
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
  onOpenModelDetails: (model: string) => void;
  onUnloadModel: (model: string) => void;
  unloadingModel: string | null;
  onChipClick: (text: string) => void;
  customModelInfos: ModelInfo[];
}

const EmptyState: React.FC<EmptyStateProps> = ({ loadedModels, currentModel, onModelSelect, onOpenModelDetails, onUnloadModel, unloadingModel, onChipClick, customModelInfos }) => (
  <>
    <div className="hero">
      <h1 className="hero__title">Get to know Lemonade</h1>
      <p className="hero__subtitle">
        {loadedModels.length > 0
          ? `${loadedModels.length} model${loadedModels.length > 1 ? 's' : ''} ready. Ask a question or explore what Lemonade can do.`
          : 'Ask a question to learn how Lemonade works and get started with your first model.'}
      </p>

      <div className="chips" role="list">
        <button className="chip" role="listitem" onClick={() => onChipClick('How do I get started with Lemonade?')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="info" size={16} /></span>
          How do I use Lemonade?
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('How do I download and load a model in Lemonade?')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="download" size={16} /></span>
          How do I add a model?
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('What are Lemonade tools, and how do I use them?')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="tools" size={16} /></span>
          What are Lemonade tools?
        </button>
        <button className="chip" role="listitem" onClick={() => onChipClick('What can my hardware run well with Lemonade?')}>
          <span className="chip__icon" aria-hidden="true"><Icon name="gauge" size={16} /></span>
          What can my hardware run?
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
            const modeLabel = modelModeDisplayLabel(cap, audioInput, m.recipe);
            const modeBadge = modelModeBadge(cap, m.recipe);
            const selectable = canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(cap);
            const isActive = currentModel === m.model_name;
            return (
              <article className="active-card" key={m.model_name}>
                <div className="active-card__head">
                  <div>
                    <div className="active-card__name-row">
                      <button
                        type="button"
                        className="active-card__name"
                        onClick={() => onOpenModelDetails(m.model_name)}
                        title={`Open ${m.model_name} in Models`}
                      >
                        {m.model_name}
                      </button>
                      <CopyInlineButton text={m.model_name} title="Copy model name" />
                    </div>
                    <div className="active-card__meta">{m.recipe || 'runtime'} · {m.checkpoint || 'default'}</div>
                  </div>
                  <span className="active-card__device">{m.device || 'device unknown'}</span>
                </div>
                <div className="active-card__badges">
                  <span className={`cap-badge cap-badge--${modeBadge}`}><ModelModeIcons capability={cap} recipe={m.recipe} audioInput={audioInput} size={13} /> {modeLabel}</span>
                </div>
                <div className="active-card__actions">
                  {isActive ? (
                    <span className="active-card__status">● Active {modeLabel} mode</span>
                  ) : selectable ? (
                    <button className="active-card__action" onClick={() => onModelSelect(m.model_name)}>
                      Use in {modeLabel}
                    </button>
                  ) : (
                    <span className="active-card__status active-card__status--muted">Utility model only</span>
                  )}
                </div>
                <div className="active-card__footer">
                  <button type="button" className="active-card__details" onClick={() => onOpenModelDetails(m.model_name)}>
                    View details
                  </button>
                  <button
                    type="button"
                    className="active-card__eject"
                    onClick={() => onUnloadModel(m.model_name)}
                    disabled={unloadingModel === m.model_name}
                    title={`Unload ${m.model_name}`}
                  >
                    {unloadingModel === m.model_name ? 'Unloading…' : 'Unload'}
                  </button>
                </div>
              </article>
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

const MessageBubble: React.FC<{ message: Message; activeModel: ModelSnapshot | null; userLabel: string; defaultThinkingOpen?: boolean; onOptionSelect?: (text: string) => void; onRetry?: () => void; onSpeak?: () => void; onEditUser?: (text: string) => void }> = ({ message, activeModel, userLabel, defaultThinkingOpen = false, onOptionSelect, onRetry, onSpeak, onEditUser }) => {
  const [thinkingOpen, setThinkingOpen] = useState(defaultThinkingOpen);
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
