import { useState, useEffect, useRef, useCallback } from 'react';
import type { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';
import type { ToolCall, ToolResult } from '../tools/lemonadeTools';
import { useI18n, type TranslationParams } from '../i18n';

export type { ToolCall } from '../tools/lemonadeTools';

export type ChatThinkingMode = 'normal' | 'off';

export function chatThinkingParams(mode: ChatThinkingMode): Record<string, unknown> {
  return { enable_thinking: mode !== 'off' };
}


const MAX_TOOL_ROUNDS = 5;
const TOOL_FOLLOWUP_PROMPT = [
  "Use the tool result to answer the user's last request with concrete, specific details.",
  'Do not stop at a raw count such as "105 models" or at "success". Present the actual names/status/details returned by the tool.',
  'If a tool result has status=needs_choice, call ask_question with the returned choices/candidates. Otherwise answer now in natural language.',
  'Do not call the same tool with the same arguments again unless the result explicitly asks you to.',
].join(' ');

export interface ToolArtifact {
  type: 'image' | 'audio' | 'model3d';
  url: string;
  name?: string;
  mime?: string;
}

export interface ToolExecutionPayload extends ToolResult {
  displayResult?: string;
  artifacts?: ToolArtifact[];
  error?: boolean;
}

export interface ChatToolRuntime {
  tools: Record<string, unknown>[];
  execute: (call: ToolCall) => Promise<ToolExecutionPayload>;
  systemPrompt?: string;
}

/** Built-in Lemonade tools have structured result payloads that can be localized safely. */
type ChatTranslate = (key: string, params?: TranslationParams) => string;

export const TOOL_LABEL_KEYS: Record<string, string> = {
  list_models: 'toolLabels.list_models',
  get_model_info: 'toolLabels.get_model_info',
  load_model: 'toolLabels.load_model',
  unload_model: 'toolLabels.unload_model',
  get_loaded_models: 'toolLabels.get_loaded_models',
  get_server_health: 'toolLabels.get_server_health',
  pull_model: 'toolLabels.pull_model',
  delete_model: 'toolLabels.delete_model',
  get_system_info: 'toolLabels.get_system_info',
  list_backends: 'toolLabels.list_backends',
  install_backend: 'toolLabels.install_backend',
  ask_question: 'toolLabels.ask_question',
  generate_image: 'toolLabels.generate_image',
  edit_image: 'toolLabels.edit_image',
  generate_audio: 'toolLabels.generate_audio',
  text_to_speech: 'toolLabels.text_to_speech',
  transcribe_audio: 'toolLabels.transcribe_audio',
  generate_3d: 'toolLabels.generate_3d',
  analyze_image: 'toolLabels.analyze_image',
};

const STRUCTURED_MANAGEMENT_TOOL_NAMES = new Set([
  'list_models',
  'get_model_info',
  'load_model',
  'unload_model',
  'get_loaded_models',
  'get_server_health',
  'pull_model',
  'delete_model',
  'get_system_info',
  'list_backends',
  'install_backend',
  'ask_question',
]);

const LOCAL_MEDIA_RESULT_KEYS: Record<string, string> = {
  generate_image: 'toolResults.imageGenerated',
  edit_image: 'toolResults.imageEdited',
  generate_audio: 'toolResults.audioGenerated',
  text_to_speech: 'toolResults.speechGenerated',
  transcribe_audio: 'toolResults.audioTranscribed',
  generate_3d: 'toolResults.model3dGenerated',
  analyze_image: 'toolResults.imageAnalyzed',
};

function toolDisplayName(toolName: string, t: ChatTranslate): string {
  const key = TOOL_LABEL_KEYS[toolName];
  return key ? t(key) : toolName;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function compactModelResult(value: unknown): Record<string, unknown> {
  const item = asRecord(value) || {};
  const name = String(item.name || item.model || item.id || item.raw_name || '').trim();
  const recipes = Array.isArray(item.recipes)
    ? item.recipes.map(recipe => String(recipe)).filter(Boolean).slice(0, 8)
    : [];
  return {
    ...(name ? { name } : {}),
    ...(item.status ? { status: String(item.status) } : {}),
    ...(item.capability ? { capability: String(item.capability) } : {}),
    ...(item.size ? { size: String(item.size) } : {}),
    ...(item.recipe ? { recipe: String(item.recipe) } : {}),
    ...(recipes.length ? { recipes } : {}),
    ...(item.device ? { device: String(item.device) } : {}),
    ...(item.type ? { type: String(item.type) } : {}),
    ...(item.context_window ? { context_window: item.context_window } : {}),
    ...(item.checkpoint ? { checkpoint: String(item.checkpoint) } : {}),
  };
}

function compactBackendRecipes(value: unknown): Record<string, unknown> {
  const recipes = asRecord(value) || {};
  const compact: Record<string, unknown> = {};
  for (const [recipe, rawRecipe] of Object.entries(recipes).slice(0, 12)) {
    const recipeInfo = asRecord(rawRecipe) || {};
    const rawBackends = asRecord(recipeInfo.backends) || {};
    const backends: Record<string, unknown> = {};
    for (const [backend, rawBackend] of Object.entries(rawBackends).slice(0, 8)) {
      const backendInfo = asRecord(rawBackend) || {};
      backends[backend] = {
        ...(backendInfo.state ? { state: String(backendInfo.state) } : {}),
        ...(backendInfo.version ? { version: String(backendInfo.version) } : {}),
        ...(Array.isArray(backendInfo.devices)
          ? { devices: backendInfo.devices.map(device => String(device)).filter(Boolean).slice(0, 8) }
          : {}),
      };
    }
    compact[recipe] = {
      ...(recipeInfo.default_backend ? { default_backend: String(recipeInfo.default_backend) } : {}),
      backends,
    };
  }
  return compact;
}

function compactDeviceGroups(value: unknown): Record<string, unknown> {
  const groups = asRecord(value) || {};
  const compact: Record<string, unknown> = {};
  for (const [kind, rawItems] of Object.entries(groups).slice(0, 8)) {
    if (!Array.isArray(rawItems)) continue;
    compact[kind] = rawItems.slice(0, 4).map(rawItem => {
      const item = asRecord(rawItem) || {};
      return {
        ...(item.name ? { name: String(item.name) } : {}),
        ...(item.available === false ? { available: false } : {}),
        ...(Array.isArray(item.details)
          ? { details: item.details.map(detail => String(detail)).filter(Boolean).slice(0, 8) }
          : {}),
      };
    });
  }
  return compact;
}

function compactPullChoices(value: unknown): Record<string, unknown>[] {
  if (!Array.isArray(value)) return [];
  return value.slice(0, 8).map(item => {
    if (typeof item === 'string' || typeof item === 'number') return { name: String(item) };
    const record = asRecord(item) || {};
    const name = String(record.id || record.name || record.file || record.primary_file || '').trim();
    const file = String(record.file || record.primary_file || '').trim();
    const recipes = Array.isArray(record.recipes)
      ? record.recipes.map(recipe => String(recipe)).filter(Boolean).slice(0, 8)
      : [];
    return {
      ...(name ? { name } : {}),
      ...(file && file !== name ? { file } : {}),
      ...(record.downloads !== undefined ? { downloads: record.downloads } : {}),
      ...(record.status ? { status: String(record.status) } : {}),
      ...(record.capability ? { capability: String(record.capability) } : {}),
      ...(record.size ? { size: String(record.size) } : {}),
      ...(recipes.length ? { recipes } : {}),
    };
  }).filter(item => Boolean(item.name));
}

/**
 * Persist only the compact, presentation-relevant subset for built-in management
 * tools. This keeps chat history locale-independent without storing the full raw
 * API response (which can be much larger than the visible result).
 */
export function structuredToolResultData(toolName: string, data: Record<string, unknown>): Record<string, unknown> {
  if (data.error) return { error: String(data.error) };
  const anyData = data as any;
  switch (toolName) {
    case 'list_models':
      return {
        scope: anyData.scope,
        counts: asRecord(anyData.counts) || {},
        loaded: Array.isArray(anyData.loaded) ? anyData.loaded.slice(0, 8).map(compactModelResult) : [],
        downloaded: Array.isArray(anyData.downloaded) ? anyData.downloaded.slice(0, 8).map(compactModelResult) : [],
        registry: Array.isArray(anyData.registry) ? anyData.registry.slice(0, 8).map(compactModelResult) : [],
      };
    case 'get_model_info':
      return {
        ...compactModelResult(anyData),
        ...(anyData.context_window ? { context_window: anyData.context_window } : {}),
        ...(anyData.checkpoint ? { checkpoint: String(anyData.checkpoint) } : {}),
        collection_components: Array.isArray(anyData.collection_components)
          ? anyData.collection_components.slice(0, 8).map(compactModelResult)
          : [],
      };
    case 'load_model':
      return { model: anyData.model, options: asRecord(anyData.options) || {} };
    case 'unload_model':
    case 'delete_model':
      return { model: anyData.model };
    case 'get_loaded_models':
      return {
        loaded: Array.isArray(anyData.loaded) ? anyData.loaded.slice(0, 12).map(compactModelResult) : [],
      };
    case 'get_server_health':
      return {
        status: anyData.status,
        version: anyData.version,
        loaded_models: anyData.loaded_models,
        loaded: Array.isArray(anyData.loaded) ? anyData.loaded.slice(0, 12).map(compactModelResult) : [],
      };
    case 'pull_model':
      return {
        status: anyData.status,
        source: anyData.source,
        model: anyData.model,
        checkpoint: anyData.checkpoint,
        variant: anyData.variant,
        choices: compactPullChoices(anyData.choices).length
          ? compactPullChoices(anyData.choices)
          : compactPullChoices(anyData.candidates).length
            ? compactPullChoices(anyData.candidates)
            : compactPullChoices(anyData.variants),
        matches: Array.isArray(anyData.matches) ? anyData.matches.slice(0, 8).map(compactModelResult) : [],
      };
    case 'get_system_info':
      return {
        os: anyData.os,
        lemonade_version: anyData.lemonade_version,
        counts: asRecord(anyData.counts) || {},
        devices: compactDeviceGroups(anyData.devices),
        recipes: compactBackendRecipes(anyData.recipes),
      };
    case 'list_backends':
      return { recipes: compactBackendRecipes(anyData.recipes || anyData) };
    case 'install_backend':
      return {
        status: anyData.status,
        recipe: anyData.recipe,
        backend: anyData.backend,
        backend_state: anyData.backend_state,
      };
    case 'ask_question':
      return {};
    default:
      return {};
  }
}

function localizedToolStatus(value: unknown, t: ChatTranslate): string {
  const status = String(value || '').trim();
  const key = status === 'loaded' ? 'toolResults.statuses.loaded'
    : status === 'downloaded' ? 'toolResults.statuses.downloaded'
      : status === 'registry' ? 'toolResults.statuses.registry'
        : '';
  return key ? t(key) : status;
}

const TOOL_CAPABILITY_KEYS: Record<string, string> = {
  chat: 'toolResults.capabilities.chat',
  omni: 'toolResults.capabilities.omni',
  image: 'toolResults.capabilities.image',
  audio: 'toolResults.capabilities.audio',
  'audio-generation': 'toolResults.capabilities.audio-generation',
  tts: 'toolResults.capabilities.tts',
  model3d: 'toolResults.capabilities.model3d',
  embedding: 'toolResults.capabilities.embedding',
  reranking: 'toolResults.capabilities.reranking',
  classification: 'toolResults.capabilities.classification',
};

const BACKEND_STATE_KEYS: Record<string, string> = {
  installed: 'toolResults.backendStates.installed',
  installable: 'toolResults.backendStates.installable',
  unsupported: 'toolResults.backendStates.unsupported',
  update_required: 'toolResults.backendStates.update_required',
  update_available: 'toolResults.backendStates.update_available',
  action_required: 'toolResults.backendStates.action_required',
  unknown: 'toolResults.backendStates.unknown',
};

function localizedToolCapability(value: unknown, t: ChatTranslate): string {
  const capability = String(value || '').trim();
  const key = TOOL_CAPABILITY_KEYS[capability];
  return key ? t(key) : capability;
}

function localizedBackendState(value: unknown, t: ChatTranslate): string {
  const state = String(value || '').trim();
  const key = BACKEND_STATE_KEYS[state];
  return key ? t(key) : state;
}

function modelResultLine(value: unknown, t: ChatTranslate): string {
  const item = asRecord(value) || {};
  const name = String(item.name || item.model || item.id || '').trim();
  const parts = [
    name,
    localizedToolStatus(item.status, t),
    item.capability ? localizedToolCapability(item.capability, t) : '',
    item.size ? String(item.size) : '',
  ].filter(Boolean);
  if (item.recipe) parts.push(`${t('toolResults.fields.recipe')}: ${String(item.recipe)}`);
  if (item.device) parts.push(`${t('toolResults.fields.device')}: ${String(item.device)}`);
  if (item.type) parts.push(`${t('toolResults.fields.type')}: ${String(item.type)}`);
  if (Array.isArray(item.recipes) && item.recipes.length > 0) {
    parts.push(`${t('toolResults.fields.recipes')}: ${item.recipes.map(recipe => String(recipe)).join(', ')}`);
  }
  return parts.join(' · ');
}

function summarizeResult(toolName: string, data: Record<string, unknown>, t: ChatTranslate): string {
  const anyData = data as any;
  const field = (key: string, value: unknown) => `${t(`toolResults.fields.${key}`)}: ${String(value)}`;
  switch (toolName) {
    case 'list_models': {
      const counts = asRecord(anyData.counts) || {};
      const loaded = Number(counts.loaded || 0);
      const downloaded = Number(counts.downloaded || 0);
      const registry = Number(counts.registry || 0);
      const lines: string[] = [];
      const local = loaded + downloaded;
      if (local > 0) lines.push(t('toolResults.localModels', { count: local, loaded, downloaded }));
      if (registry > 0 && (anyData.scope === 'registry' || anyData.scope === 'all')) {
        lines.push(t('toolResults.registryModels', { count: registry }));
      }
      const models = [
        ...(Array.isArray(anyData.loaded) ? anyData.loaded : []),
        ...(Array.isArray(anyData.downloaded) ? anyData.downloaded : []),
        ...(Array.isArray(anyData.registry) ? anyData.registry : []),
      ].slice(0, 8);
      lines.push(...models.map((model: unknown) => modelResultLine(model, t)).filter(Boolean));
      return lines.join('\n') || t('toolResults.inventoryRetrieved');
    }
    case 'get_model_info': {
      const lines = [modelResultLine(anyData, t)].filter(Boolean);
      if (anyData.context_window) lines.push(field('context', anyData.context_window));
      if (anyData.checkpoint) lines.push(field('checkpoint', anyData.checkpoint));
      const components = Array.isArray(anyData.collection_components) ? anyData.collection_components : [];
      if (components.length > 0) {
        lines.push(`${t('toolResults.fields.components')}:`);
        lines.push(...components.map((component: unknown) => `- ${modelResultLine(component, t)}`).filter((line: string) => line !== '- '));
      }
      return lines.join('\n') || t('toolResults.inventoryRetrieved');
    }
    case 'load_model': {
      const model = String(anyData.model || '').trim();
      const lines = [model ? t('toolResults.modelLoadedNamed', { model }) : t('toolResults.modelLoaded')];
      const options = asRecord(anyData.options) || {};
      if (Object.keys(options).length > 0) lines.push(field('options', JSON.stringify(options)));
      return lines.join('\n');
    }
    case 'unload_model': {
      const model = String(anyData.model || '').trim();
      return model ? t('toolResults.modelUnloadedNamed', { model }) : t('toolResults.modelUnloaded');
    }
    case 'get_loaded_models': {
      const loaded = Array.isArray(anyData.loaded) ? anyData.loaded : [];
      if (loaded.length === 0) return t('toolResults.noModelsLoaded');
      return [
        t('toolResults.modelsLoaded', { count: loaded.length }),
        ...loaded.map((model: unknown) => modelResultLine(model, t)).filter(Boolean),
      ].join('\n');
    }
    case 'get_server_health': {
      const loaded = Array.isArray(anyData.loaded) ? anyData.loaded : [];
      const lines = [t('toolResults.serverHealth', {
        status: String(anyData.status || ''),
        count: Number(anyData.loaded_models || loaded.length || 0),
      })];
      if (anyData.version) lines.push(field('version', anyData.version));
      lines.push(...loaded.map((model: unknown) => modelResultLine(model, t)).filter(Boolean));
      return lines.join('\n');
    }
    case 'pull_model': {
      const choices = Array.isArray(anyData.choices) ? anyData.choices : [];
      const matches = Array.isArray(anyData.matches) ? anyData.matches : [];
      if (anyData.status === 'needs_choice') {
        const choiceNames = choices.map((choice: unknown) => {
          const item = asRecord(choice) || {};
          return String(item.name || '').trim();
        }).filter(Boolean);
        const lines = [choiceNames.length > 0
          ? t('toolResults.needsChoiceWithOptions', { choices: choiceNames.join(', ') })
          : t('toolResults.needsChoice')];
        if (matches.length > 0) {
          lines.push(...matches.map((model: unknown) => modelResultLine(model, t)).filter(Boolean));
        } else {
          lines.push(...choices.map((choice: unknown) => {
            const item = asRecord(choice) || {};
            const parts = [String(item.name || '').trim()];
            if (item.file) parts.push(`${t('toolResults.fields.file')}: ${String(item.file)}`);
            if (item.downloads !== undefined) parts.push(`${t('toolResults.fields.downloads')}: ${String(item.downloads)}`);
            if (item.status) parts.push(localizedToolStatus(item.status, t));
            if (item.capability) parts.push(localizedToolCapability(item.capability, t));
            if (item.size) parts.push(String(item.size));
            if (Array.isArray(item.recipes) && item.recipes.length > 0) {
              parts.push(`${t('toolResults.fields.recipes')}: ${item.recipes.map(recipe => String(recipe)).join(', ')}`);
            }
            return parts.filter(Boolean).join(' · ');
          }).filter(Boolean));
        }
        return lines.join('\n');
      }
      const lines = [anyData.model
        ? t('toolResults.downloadCompleteModel', { model: String(anyData.model) })
        : t('toolResults.downloadComplete')];
      if (anyData.source) lines.push(field('source', anyData.source));
      if (anyData.checkpoint) lines.push(field('checkpoint', anyData.checkpoint));
      if (anyData.variant) lines.push(field('variant', anyData.variant));
      return lines.join('\n');
    }
    case 'delete_model': {
      const model = String(anyData.model || '').trim();
      return model ? t('toolResults.modelDeletedNamed', { model }) : t('toolResults.modelDeleted');
    }
    case 'get_system_info': {
      const counts = asRecord(anyData.counts) || {};
      const recipeCount = Number(counts.recipes || 0);
      const installed = Number(counts.installed_backends || 0);
      const available = Number(counts.installable_or_update_backends || 0);
      const lines = [t('toolResults.systemBackendSummary', { recipes: recipeCount, installed, available })];
      if (anyData.os) lines.push(field('os', anyData.os));
      if (anyData.lemonade_version) lines.push(field('version', anyData.lemonade_version));
      const devices = asRecord(anyData.devices) || {};
      for (const [kind, rawItems] of Object.entries(devices)) {
        if (!Array.isArray(rawItems)) continue;
        const labels = rawItems.map(rawItem => {
          const item = asRecord(rawItem) || {};
          const details = Array.isArray(item.details) ? item.details.map(detail => String(detail)).join(', ') : '';
          return `${String(item.name || kind)}${details ? `: ${details}` : ''}`;
        }).filter(Boolean);
        if (labels.length > 0) lines.push(`${kind}: ${labels.join('; ')}`);
      }
      const backendRecipes = asRecord(anyData.recipes) || {};
      for (const [recipe, rawRecipe] of Object.entries(backendRecipes).slice(0, 12)) {
        const recipeInfo = asRecord(rawRecipe) || {};
        const backends = asRecord(recipeInfo.backends) || {};
        const backendText = Object.entries(backends).slice(0, 8).map(([backend, rawBackend]) => {
          const backendInfo = asRecord(rawBackend) || {};
          return `${backend}: ${localizedBackendState(backendInfo.state, t)}${backendInfo.version ? ` ${String(backendInfo.version)}` : ''}`.trim();
        }).filter(Boolean).join(', ');
        const defaultSuffix = recipeInfo.default_backend
          ? ` (${t('toolResults.backendDefault', { backend: String(recipeInfo.default_backend) })})`
          : '';
        lines.push(`${recipe}${defaultSuffix}: ${backendText || t('toolResults.noBackends')}`);
      }
      return lines.join('\n');
    }
    case 'list_backends': {
      const recipes = asRecord(anyData.recipes || anyData) || {};
      const names = Object.keys(recipes);
      const lines = [t('toolResults.backendRecipes', { count: names.length, names: names.slice(0, 5).join(', ') })];
      for (const [recipe, rawRecipe] of Object.entries(recipes).slice(0, 12)) {
        const recipeInfo = asRecord(rawRecipe) || {};
        const backends = asRecord(recipeInfo.backends) || {};
        const backendText = Object.entries(backends).slice(0, 8).map(([backend, rawBackend]) => {
          const backendInfo = asRecord(rawBackend) || {};
          return `${backend}: ${localizedBackendState(backendInfo.state, t)}${backendInfo.version ? ` ${String(backendInfo.version)}` : ''}`.trim();
        }).filter(Boolean).join(', ');
        const defaultSuffix = recipeInfo.default_backend
          ? ` (${t('toolResults.backendDefault', { backend: String(recipeInfo.default_backend) })})`
          : '';
        lines.push(`${recipe}${defaultSuffix}: ${backendText || t('toolResults.noBackends')}`);
      }
      return lines.join('\n');
    }
    case 'install_backend': {
      const lines = [t('toolResults.backendInstalled', {
        recipe: String(anyData.recipe || ''),
        backend: String(anyData.backend || ''),
      })];
      if (anyData.backend_state || anyData.status) lines.push(field('status', localizedBackendState(anyData.backend_state || anyData.status, t)));
      return lines.join('\n');
    }
    case 'ask_question':
      return t('toolResults.presentingChoices');
    default:
      return JSON.stringify(data).slice(0, 200);
  }
}

export function toolResultText(entry: Pick<ToolCallEntry, 'name' | 'result' | 'resultData' | 'errorMessage' | 'status'>, t: ChatTranslate): string {
  if (entry.errorMessage) return t('toolResults.error', { message: entry.errorMessage });
  if (entry.resultData && STRUCTURED_MANAGEMENT_TOOL_NAMES.has(entry.name)) {
    const error = entry.resultData.error;
    if (error) return t('toolResults.error', { message: String(error) });
    return summarizeResult(entry.name, entry.resultData, t);
  }
  const mediaResultKey = LOCAL_MEDIA_RESULT_KEYS[entry.name];
  if (mediaResultKey && entry.status === 'done') return t(mediaResultKey);
  return entry.result;
}

export interface ToolCallEntry {
  name: string;
  args: string;
  rawArgs?: string;
  result: string;
  resultData?: Record<string, unknown>;
  errorMessage?: string;
  status: 'running' | 'done' | 'error';
  artifacts?: ToolArtifact[];
}


interface StreamState {
  content: string;
  thinking: string;
  toolStatus?: string;
  toolCalls: ToolCallEntry[];
}

export interface ChatStreamingResult {
  activeStreams: Record<string, StreamState>;
  liveStats: Record<string, LiveStreamStats>;
  thinkingExpanded: boolean;
  setThinkingExpanded: React.Dispatch<React.SetStateAction<boolean>>;
  streamingConvoIds: Set<string>;
  getStream: (convoId: string) => StreamState | undefined;
  getLiveStats: (convoId: string) => LiveStreamStats | undefined;
  send: (
    convoId: string,
    model: string,
    messages: ChatMessage[],
    tools?: boolean | ChatToolRuntime | null,
    thinkingMode?: ChatThinkingMode,
  ) => Promise<void>;
  stop: (convoId: string) => { content: string; thinking?: string } | null;
}

export function useChatStreaming(
  onDone: (convoId: string, stats: ChatCompletionStats, toolCalls?: ToolCallEntry[]) => void,
  onError: (convoId: string, message: string) => void,
): ChatStreamingResult {
  const { t } = useI18n('chat');
  const [activeStreams, setActiveStreams] = useState<Record<string, StreamState>>({});
  const [liveStats, setLiveStats] = useState<Record<string, LiveStreamStats>>({});
  const [thinkingExpanded, setThinkingExpanded] = useState(false);

  const controllersRef = useRef<Map<string, AbortController>>(new Map());
  const tokenBufferRef = useRef<Record<string, StreamState>>({});
  const flushTimerRef = useRef<number | null>(null);

  // Do not keep a 50ms interval alive for the entire lifetime of the Chat view.
  // Schedule a flush only when a streaming token actually arrives.
  const flushTokenBuffer = useCallback(() => {
    flushTimerRef.current = null;
    const buf = tokenBufferRef.current;
    const keys = Object.keys(buf);
    if (keys.length === 0) return;
    setActiveStreams(prev => {
      let next = prev;
      for (const id of keys) {
        if (!prev[id]) continue;
        if (next === prev) next = { ...prev };
        next[id] = { ...next[id], ...buf[id] };
      }
      return next;
    });
    tokenBufferRef.current = {};
  }, []);

  const scheduleTokenFlush = useCallback(() => {
    if (flushTimerRef.current !== null) return;
    flushTimerRef.current = window.setTimeout(flushTokenBuffer, 50);
  }, [flushTokenBuffer]);

  useEffect(() => () => {
    if (flushTimerRef.current !== null) window.clearTimeout(flushTimerRef.current);
  }, []);

  const getStream = useCallback(
    (convoId: string) => activeStreams[convoId],
    [activeStreams],
  );

  const getLiveStats = useCallback(
    (convoId: string) => liveStats[convoId],
    [liveStats],
  );

  const send = useCallback(async (
    convoId: string,
    model: string,
    messages: ChatMessage[],
    tools: boolean | ChatToolRuntime | null = false,
    thinkingMode: ChatThinkingMode = 'normal',
  ) => {
    let runtime: ChatToolRuntime | null = tools && typeof tools === 'object' ? tools : null;
    if (tools === true) {
      const { LEMONADE_TOOLS, executeTool } = await import(
        /* webpackChunkName: "lemonade-tools" */ '../tools/lemonadeTools'
      );
      runtime = {
        tools: LEMONADE_TOOLS as unknown as Record<string, unknown>[],
        execute: executeTool as unknown as (call: ToolCall) => Promise<ToolExecutionPayload>,
      };
    }
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '', toolCalls: [] } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    let fullMessages = [...messages];

    let toolRound = 0;
    const allToolCalls: ToolCallEntry[] = [];

    const runCompletion = async (): Promise<void> => {
      const { default: api } = await import(/* webpackChunkName: "api-client" */ '../api');
      return new Promise<void>((resolve, reject) => {
        api.chatCompletion(model, fullMessages, {
          params: chatThinkingParams(thinkingMode),
          tools: runtime?.tools?.length ? runtime.tools : undefined,
          onReasoning: (_token, fullReasoning) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].thinking = fullReasoning;
            scheduleTokenFlush();
            if (!thinkingExpanded) setThinkingExpanded(true);
          },
          onToken: (_token, full) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].content = full;
            scheduleTokenFlush();
          },
          onStats: (stats) => {
            setLiveStats(prev => ({ ...prev, [convoId]: stats }));
          },
          onToolCalls: async (toolCalls) => {
            try {
              toolRound++;
              if (toolRound > MAX_TOOL_ROUNDS) {
                onError(convoId, t('errors.toolLoopLimit'));
                cleanup(convoId);
                resolve();
                return;
              }

              // Show tool status in the stream and add running entries
              const toolNames = toolCalls.map(tc => toolDisplayName(tc.function.name, t)).join(', ');
              const runningEntries: ToolCallEntry[] = toolCalls.map(tc => {
                let argsStr = '';
                try { const a = JSON.parse(tc.function.arguments || '{}'); argsStr = Object.entries(a).map(([k,v]) => `${k}: ${v}`).join(', '); } catch {}
                return { name: tc.function.name, args: argsStr, rawArgs: tc.function.arguments, result: '', status: 'running' as const };
              });
              allToolCalls.push(...runningEntries);
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: t('stream.callingTools', { tools: toolNames }), toolCalls: [...allToolCalls] },
              }));

              // Append the assistant's tool_calls message
              fullMessages = [
                ...fullMessages,
                { role: 'assistant' as const, content: '', tool_calls: toolCalls },
              ];

              // Execute all tool calls in parallel
              const results = await Promise.all(
                toolCalls.map(tc => runtime!.execute(tc as ToolCall))
              );

              // Update entries with results
              for (let j = 0; j < runningEntries.length; j++) {
                const r = results[j];
                const entry = runningEntries[j];
                entry.artifacts = r.artifacts;
                try {
                  const parsed = JSON.parse(r.content) as Record<string, unknown>;
                  const structuredManagement = STRUCTURED_MANAGEMENT_TOOL_NAMES.has(entry.name);
                  const mediaResultKey = LOCAL_MEDIA_RESULT_KEYS[entry.name];
                  const knownLocalTool = structuredManagement || Boolean(mediaResultKey);
                  if (structuredManagement) {
                    entry.resultData = structuredToolResultData(entry.name, parsed);
                  }
                  if (parsed.error) {
                    if (knownLocalTool) entry.errorMessage = String(parsed.error);
                    entry.result = r.displayResult || String(parsed.error);
                    entry.status = 'error';
                  } else if (structuredManagement) {
                    entry.result = r.displayResult || summarizeResult(entry.name, entry.resultData || parsed, t);
                    entry.status = r.error ? 'error' : 'done';
                  } else if (mediaResultKey) {
                    entry.result = r.displayResult || t(mediaResultKey);
                    entry.status = r.error ? 'error' : 'done';
                  } else {
                    entry.result = r.displayResult || JSON.stringify(parsed).slice(0, 200);
                    entry.status = r.error ? 'error' : 'done';
                  }
                } catch {
                  const mediaResultKey = LOCAL_MEDIA_RESULT_KEYS[entry.name];
                  entry.result = !r.error && mediaResultKey
                    ? t(mediaResultKey)
                    : (r.displayResult || r.content.slice(0, 200));
                  entry.status = r.error ? 'error' : 'done';
                }
              }

              // Append tool results, then remind the model to produce a real final
              // answer (or call a narrower tool) instead of echoing a broad count.
              for (const result of results) {
                fullMessages = [
                  ...fullMessages,
                  { role: 'tool' as const, content: result.content, tool_call_id: result.tool_call_id },
                ];
              }
              fullMessages = [
                ...fullMessages,
                { role: 'user' as const, content: TOOL_FOLLOWUP_PROMPT },
              ];

              // Clear tool status, keep accumulated tool call entries
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: undefined, toolCalls: [...allToolCalls] },
              }));

              // Re-run completion with tool results
              try {
                await runCompletion();
                resolve();
              } catch (err) {
                reject(err);
              }
            } catch (err: unknown) {
              // Mark any still-running tool call entries as errored so the user sees what happened.
              const msg = err instanceof Error ? err.message : String(err);
              for (const entry of allToolCalls) {
                if (entry.status === 'running') {
                  entry.errorMessage = msg;
                  entry.result = t('toolResults.error', { message: msg });
                  entry.status = 'error';
                }
              }
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: undefined, toolCalls: [...allToolCalls] },
              }));
              delete tokenBufferRef.current[convoId];
              onError(convoId, t('errors.toolExecutionFailed', { message: msg }));
              cleanup(convoId);
              resolve();
            }
          },
          onDone: (stats) => {
            delete tokenBufferRef.current[convoId];
            onDone(convoId, stats, allToolCalls.length > 0 ? [...allToolCalls] : undefined);
            cleanup(convoId);
            resolve();
          },
          onError: (err) => {
            if (err.name === 'AbortError') { resolve(); return; }
            delete tokenBufferRef.current[convoId];
            onError(convoId, err.message);
            cleanup(convoId);
            resolve();
          },
          signal: controller.signal,
        });
      });
    };

    const cleanup = (id: string) => {
      setActiveStreams(prev => {
        const next = { ...prev };
        delete next[id];
        return next;
      });
      setLiveStats(prev => {
        const next = { ...prev };
        delete next[id];
        return next;
      });
      controllersRef.current.delete(id);
    };

    await runCompletion();
  }, [onDone, onError, scheduleTokenFlush, t]);

  const stop = useCallback((convoId: string): { content: string; thinking?: string } | null => {
    const controller = controllersRef.current.get(convoId);
    if (!controller) return null;

    const buf = tokenBufferRef.current[convoId];
    const stream = activeStreams[convoId];
    const merged = stream && buf
      ? { content: buf.content || stream.content, thinking: buf.thinking || stream.thinking }
      : stream;
    delete tokenBufferRef.current[convoId];

    controller.abort();
    controllersRef.current.delete(convoId);

    setActiveStreams(prev => {
      const next = { ...prev };
      delete next[convoId];
      return next;
    });
    setLiveStats(prev => {
      const next = { ...prev };
      delete next[convoId];
      return next;
    });

    if (merged && (merged.content || merged.thinking)) {
      return { content: merged.content || t('stream.stopped'), thinking: merged.thinking || undefined };
    }
    return null;
  }, [activeStreams, t]);

  return {
    activeStreams,
    liveStats,
    thinkingExpanded,
    setThinkingExpanded,
    streamingConvoIds: new Set(Object.keys(activeStreams)),
    getStream,
    getLiveStats,
    send,
    stop,
  };
}
