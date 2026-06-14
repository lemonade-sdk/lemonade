import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { ModelInfo, LoadedModel, PullCallbacks, PullVariantsResult, HFModelResult, searchHuggingFace, friendlyErrorMessage, DownloadProgressEvent } from '../api';
import { canSelectInComposer, capabilityFromLoaded, capabilityFromModelInfo, capabilityIcon, capabilityLabel, ModelCapability } from '../modelCapabilities';
import { CapabilityIcon, Icon, PresetIcon } from './Icon';
import type { AccountSession } from '../features/accounts/accountStore';
import { CUSTOM_CAPABILITIES, CustomModelCapability, customLoadOptions, customModelToModelInfo, customRegistrationOptions, deleteCustomModel, loadCustomModels, upsertCustomModel } from '../features/customModels/customModelStore';
import { collectionComponentLabel, getCollectionComponents, isCollectionModel, isCollectionFullyDownloaded, withVirtualLoadedCollections } from '../features/collections/collectionModels';
import { DEFAULT_CONTEXT_SIZE, DEFAULT_PRESET, PRESET_STORE_EVENT, Preset, STARTERS, effectivePresetParamPreviewLines, isCompatible, loadApplied, loadUserPresets, modelContextSize, presetHasApplicablePreviewOverrides, presetParamPreviewLines, saveApplied } from '../presetStore';

/* ── Helpers ─────────────────────────────────────────────────── */

function formatSize(gb: number): string {
  if (!Number.isFinite(gb) || gb <= 0) return 'size unknown';
  if (gb >= 1) return `${gb.toFixed(1)} GB`;
  if (gb >= 0.01) return `${(gb * 1000).toFixed(0)} MB`;
  return `< 1 MB`;
}

function positiveNumber(value: unknown): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : undefined;
}

function supportsContextDisplay(capability: ReturnType<typeof capabilityFromModelInfo> | ReturnType<typeof capabilityFromLoaded>): boolean {
  return capability === 'chat' || capability === 'omni' || capability === 'unknown';
}

function contextSizeForDisplay(model: ModelInfo | null | undefined, liveCtxSize?: unknown, fallbackCtxSize?: unknown): number | undefined {
  if (!model) return positiveNumber(liveCtxSize) ?? positiveNumber(fallbackCtxSize) ?? DEFAULT_CONTEXT_SIZE;
  const capability = capabilityFromModelInfo(model);
  if (!supportsContextDisplay(capability)) return undefined;
  return positiveNumber(liveCtxSize) ?? modelContextSize(model, fallbackCtxSize);
}

function contextLabel(ctx: number): string {
  return `${(ctx / 1024).toFixed(0)}K`;
}

function modelName(m: ModelInfo | null | undefined): string {
  if (!m) return '';
  const raw = (m as any).model_name ?? m.name ?? m.id ?? '';
  return String(raw).trim();
}

function objectRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : {};
}

function withLoadedRecipeOptions(info: ModelInfo | null | undefined, loaded: LoadedModel | null | undefined): ModelInfo | null {
  if (!info && !loaded) return null;
  if (!loaded) return info || null;
  const base = info || ({ id: loaded.model_name, name: loaded.model_name } as ModelInfo);
  const recipeOptions = {
    ...objectRecord((base as any).recipe_options),
    ...objectRecord(loaded.recipe_options),
  };
  return {
    ...base,
    model_name: (base as any).model_name || loaded.model_name,
    name: (base as any).name || loaded.model_name,
    checkpoint: (base as any).checkpoint || loaded.checkpoint,
    recipe: (base as any).recipe || loaded.recipe,
    type: (base as any).type || loaded.type,
    recipe_options: recipeOptions,
  } as ModelInfo;
}

function modelLabels(m: ModelInfo | null | undefined): string[] {
  const labels = (m as any)?.labels;
  if (!Array.isArray(labels)) return [];
  return labels.map(label => String(label).trim()).filter(Boolean);
}

function recipeBadgeText(recipe: string): string {
  const normalized = String(recipe || '').toLowerCase();
  switch (normalized) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'SD.cpp';
    case 'whispercpp': return 'Whisper';
    case 'moonshine': return 'Moonshine';
    case 'kokoro': return 'Kokoro';
    case 'collection.omni': return 'Omni';
    case 'collection': return 'Collection';
    default: return recipe || 'Backend';
  }
}

function recipeColor(recipe: string): string {
  const normalized = String(recipe || '').toLowerCase();
  switch (normalized) {
    case 'llamacpp': return '#facc15';
    case 'vllm': return '#60a5fa';
    case 'flm': return '#34d399';
    case 'ryzenai-llm': return '#f97316';
    case 'sd-cpp': return '#c084fc';
    case 'whispercpp': return '#38bdf8';
    case 'moonshine': return '#22d3ee';
    case 'kokoro': return '#f472b6';
    case 'collection.omni': return '#a78bfa';
    case 'collection': return '#94a3b8';
    default: return 'var(--text-tertiary)';
  }
}

const BackendBadge: React.FC<{ recipe: string; running?: boolean }> = ({ recipe, running = false }) => {
  const label = recipeLabel(recipe);
  return (
    <div
      className={`row__backend-badge${running ? ' row__backend-badge--running' : ''}`}
      style={{ '--backend-color': recipeColor(recipe) } as React.CSSProperties}
      title={label}
      aria-label={label}
    >
      <span>{recipeBadgeText(recipe)}</span>
    </div>
  );
};

function recipeLabel(recipe: string): string {
  const normalized = String(recipe || '').toLowerCase();
  switch (normalized) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FastFlowLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'Stable Diffusion';
    case 'whispercpp': return 'Whisper';
    case 'moonshine': return 'Moonshine';
    case 'kokoro': return 'Kokoro TTS';
    case 'collection.omni': return 'Omni Collection';
    case 'collection': return 'Collection';
    default: return recipe || 'Unknown';
  }
}

function modelType(m: ModelInfo): string {
  const cap = capabilityFromModelInfo(m);
  return cap === 'chat' || cap === 'unknown' ? 'llm' : cap;
}

function labelDisplay(label: string): string {
  const map: Record<string, string> = {
    'chat': 'Chat',
    'llm': 'Chat',
    'tool-calling': 'Tools',
    'tools': 'Tools',
    'vision': 'Vision',
    'image-input': 'Vision',
    'vlm': 'VLM',
    'omni': 'Omni',
    'multimodal': 'Multimodal',
    'multi-modal': 'Multimodal',
    'vision-language': 'Vision Language',
    'reasoning': 'Reasoning',
    'coding': 'Code',
    'code': 'Code',
    'hot': 'Popular',
    'popular': 'Popular',
    'mtp': 'MTP',
    'embedding': 'Embedding',
    'embeddings': 'Embeddings',
    'reranker': 'Reranking',
    'reranking': 'Reranking',
    'audio': 'Audio',
    'asr': 'ASR',
    'stt': 'STT',
    'speech-to-text': 'Speech to Text',
    'transcription': 'Transcription',
    'realtime-transcription': 'Realtime',
    'chat-transcription': 'Chat ASR',
    'tts': 'TTS',
    'speech': 'Speech',
    'text-to-speech': 'Text to Speech',
    'image': 'Image',
    'image-generation': 'Image',
    'diffusion': 'Image',
    'edit': 'Edit',
    'image-edit': 'Edit',
    'image-editing': 'Edit',
    'upscaling': 'Upscale',
    'custom': 'Custom',
  };
  const key = String(label || '').toLowerCase();
  return map[key] || label;
}

type CapabilityIconTarget = ModelCapability | 'all' | 'vision' | 'code' | 'transcription' | 'popular' | 'tools' | 'reasoning' | 'mtp';

function iconForCapabilityLabel(label: string): CapabilityIconTarget {
  const key = String(label || '').toLowerCase().trim();
  if (['all'].includes(key)) return 'all';
  if (['hot', 'popular'].includes(key)) return 'popular';
  if (['tool-calling', 'tools'].includes(key)) return 'tools';
  if (['reasoning'].includes(key)) return 'reasoning';
  if (['mtp'].includes(key)) return 'mtp';
  if (['chat', 'llm'].includes(key)) return 'chat';
  if (['omni', 'multimodal', 'multi-modal'].includes(key)) return 'omni';
  if (['vision', 'image-input', 'vlm', 'vision-language'].includes(key)) return 'vision';
  if (['coding', 'code'].includes(key)) return 'code';
  if (['image', 'image-generation', 'diffusion', 'edit', 'image-edit', 'image-editing', 'upscaling'].includes(key)) return 'image';
  if (['audio', 'transcription', 'realtime-transcription', 'chat-transcription', 'asr', 'stt', 'speech-to-text'].includes(key)) return 'transcription';
  if (['tts', 'speech', 'text-to-speech'].includes(key)) return 'tts';
  if (['embedding', 'embeddings'].includes(key)) return 'embedding';
  if (['reranking', 'reranker', 'rerank'].includes(key)) return 'reranking';
  return 'unknown';
}

function labelFromCapability(capability: ModelCapability): string | null {
  switch (capability) {
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'image': return 'image';
    case 'audio': return 'transcription';
    case 'tts': return 'tts';
    case 'embedding': return 'embedding';
    case 'reranking': return 'reranking';
    default: return null;
  }
}

function findModelInfoByDisplayName(models: ModelInfo[], name: string): ModelInfo | null {
  const needle = String(name || '').trim().toLowerCase();
  if (!needle) return null;
  return models.find(model => modelName(model).toLowerCase() === needle
    || String(model.display_name || '').trim().toLowerCase() === needle
    || String(model.id || '').trim().toLowerCase() === needle) || null;
}

function capabilityLabelsForModel(model: ModelInfo | null | undefined, allModels: ModelInfo[]): string[] {
  const labels: string[] = [];
  const addLabel = (raw: unknown) => {
    const label = String(raw || '').trim().toLowerCase();
    if (!label || ['llamacpp', 'custom'].includes(label)) return;
    labels.push(label);
  };

  modelLabels(model).forEach(addLabel);

  if (model && isCollectionModel(model)) {
    for (const componentName of getCollectionComponents(model)) {
      const component = findModelInfoByDisplayName(allModels, componentName);
      if (component) {
        modelLabels(component).forEach(addLabel);
        addLabel(labelFromCapability(capabilityFromModelInfo(component)));
      }
    }
  }

  if (model) addLabel(labelFromCapability(capabilityFromModelInfo(model)));

  const unique = new Map<string, string>();
  for (const label of labels) {
    const display = labelDisplay(label);
    const key = display.toLowerCase();
    if (!unique.has(key)) unique.set(key, label);
  }
  return [...unique.values()];
}

function hfUrl(checkpoint: string): string | null {
  if (!checkpoint) return null;
  const parts = checkpoint.split(':')[0];
  if (!parts.includes('/')) return null;
  return `https://huggingface.co/${parts}`;
}

function formatDownloads(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}K`;
  return String(n);
}

function formatBytes(bytes: number): string {
  if (bytes >= 1_073_741_824) return `${(bytes / 1_073_741_824).toFixed(1)} GB`;
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(0)} MB`;
  if (bytes >= 1_024) return `${(bytes / 1_024).toFixed(0)} KB`;
  return `${bytes} B`;
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

function safeFileName(value: string): string {
  return value.trim().replace(/[^a-zA-Z0-9._-]+/g, '-').replace(/-+/g, '-').replace(/^-|-$/g, '') || 'model';
}

function exportJsonFile(name: string, data: unknown): void {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `${safeFileName(name)}.json`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 0);
}

function implicitCustomModelName(displayName: string, checkpoint: string, fallback = 'custom-model'): string {
  const fromDisplay = displayName.trim();
  if (fromDisplay) return fromDisplay;
  const checkpointName = checkpoint.trim().split(/[\\/]/).pop()?.split(':')[0]?.trim();
  return checkpointName || fallback;
}

const CopyInlineButton: React.FC<{ text: string; title?: string }> = ({ text, title = 'Copy model name' }) => {
  const [copied, setCopied] = useState(false);
  const handleClick = async (event: React.MouseEvent<HTMLButtonElement>) => {
    event.preventDefault();
    event.stopPropagation();
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
      className={`copy-inline${copied ? ' copy-inline--copied' : ''}`}
      onClick={handleClick}
      title={copied ? 'Copied' : title}
      aria-label={copied ? 'Copied' : title}
    >
      {copied ? <Icon name="check" size={13} /> : <Icon name="copy" size={13} />}
    </button>
  );
};

const RECIPE_BADGES: Record<string, string> = {
  llamacpp: 'llama.cpp',
  vllm: 'vLLM',
  moonshine: 'Moonshine',
  'ryzenai-llm': 'RyzenAI',
  'collection.omni': 'Omni Collection',
};

type CustomRecipeOption = { value: string; label: string; hint: string };

const CHAT_RECIPE_OPTIONS: CustomRecipeOption[] = [
  { value: 'llamacpp', label: 'llama.cpp', hint: 'Local GGUF / llama.cpp backend' },
  { value: 'vllm', label: 'vLLM', hint: 'vLLM backend for compatible models' },
  { value: 'flm', label: 'FastFlowLM', hint: 'FLM backend for supported AMD NPU models' },
  { value: 'ryzenai-llm', label: 'RyzenAI', hint: 'RyzenAI LLM backend' },
];

const CUSTOM_RECIPE_OPTIONS: Record<CustomModelCapability, CustomRecipeOption[]> = {
  chat: CHAT_RECIPE_OPTIONS,
  omni: CHAT_RECIPE_OPTIONS,
  image: [{ value: 'sd-cpp', label: 'Stable Diffusion', hint: 'Stable Diffusion C++ backend' }],
  audio: [
    { value: 'whispercpp', label: 'Whisper', hint: 'Whisper C++ transcription backend' },
    { value: 'moonshine', label: 'Moonshine', hint: 'CPU streaming speech-to-text backend' },
  ],
  tts: [{ value: 'kokoro', label: 'Kokoro TTS', hint: 'Kokoro text-to-speech backend' }],
  embedding: [{ value: 'llamacpp', label: 'llama.cpp', hint: 'Embedding through llama.cpp-compatible model' }],
  reranking: [{ value: 'llamacpp', label: 'llama.cpp', hint: 'Reranking through llama.cpp-compatible model' }],
};

function recipeOptionsForCustomDraft(capability: CustomModelCapability, omniSource: 'single' | 'collection'): CustomRecipeOption[] {
  if (capability === 'omni' && omniSource === 'collection') {
    return [{ value: 'collection.omni', label: 'Omni Collection', hint: 'Virtual wrapper around selected component models' }];
  }
  return CUSTOM_RECIPE_OPTIONS[capability] || CHAT_RECIPE_OPTIONS;
}

/* ── Filter / search types ─────────────────────────────────── */

type FilterTab = 'all' | 'llm' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding';
type CustomFormMode = 'model' | 'omni-collection';
type OmniComponentRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';
type CustomModelDraftState = {
  name: string;
  displayName: string;
  checkpoint: string;
  recipe: string;
  capability: CustomModelCapability;
  maxContextWindow: string;
  labels: string;
  omniSource: 'single' | 'collection';
  llmComponent: string;
  visionComponent: string;
  imageComponent: string;
  editComponent: string;
  transcriptionComponent: string;
  speechComponent: string;
};

const FILTER_TABS: { key: FilterTab; label: string; icon: ModelCapability | 'all' }[] = [
  { key: 'all', label: 'All', icon: 'all' },
  { key: 'llm', label: 'LLM', icon: 'chat' },
  { key: 'omni', label: 'Omni', icon: 'omni' },
  { key: 'image', label: 'Image', icon: 'image' },
  { key: 'audio', label: 'Audio', icon: 'audio' },
  { key: 'tts', label: 'TTS', icon: 'tts' },
  { key: 'embedding', label: 'Embed', icon: 'embedding' },
];

function createEmptyCustomDraft(mode: CustomFormMode = 'model'): CustomModelDraftState {
  const isOmniCollection = mode === 'omni-collection';
  return {
    name: '',
    displayName: '',
    checkpoint: '',
    recipe: isOmniCollection ? 'collection.omni' : 'llamacpp',
    capability: isOmniCollection ? 'omni' : 'chat',
    maxContextWindow: '4096',
    labels: '',
    omniSource: isOmniCollection ? 'collection' : 'single',
    llmComponent: '',
    visionComponent: '',
    imageComponent: '',
    editComponent: '',
    transcriptionComponent: '',
    speechComponent: '',
  };
}

function loadedIsVirtualOmniCollection(model: LoadedModel): boolean {
  const recipe = String(model.recipe || '').toLowerCase();
  const components = model.recipe_options?.components;
  return (recipe === 'collection.omni' || recipe === 'collection')
    && model.recipe_options?.virtual_collection === true
    && Array.isArray(components)
    && components.some(component => typeof component === 'string' && component.trim().length > 0);
}

function canShowPresetHighlight(m: ModelInfo | null | undefined): boolean {
  if (!m) return true;
  const recipe = String((m as any).recipe || '').toLowerCase();
  if (recipe === 'collection.omni' || recipe === 'collection') return false;
  if (isCollectionModel(m)) return false;
  return capabilityFromModelInfo(m) !== 'omni';
}

type OmniComponentOptionSource = 'custom' | 'downloaded' | 'registered';

interface OmniComponentOption {
  id: string;
  label: string;
  detail: string;
  source: OmniComponentOptionSource;
  downloaded: boolean;
  custom: boolean;
  recipe: string;
  labels: string[];
}

const OMNI_COMPONENT_ROLE_CONFIG: Record<OmniComponentRole, { label: string; placeholder: string; help: string; required?: boolean }> = {
  llm: {
    label: 'Planner LLM',
    placeholder: 'Search downloaded, registry, or custom LLMs…',
    help: 'Required planner model for chat and tool calls.',
    required: true,
  },
  vision: {
    label: 'Vision',
    placeholder: 'Search vision/VLM components…',
    help: 'Optional model used for image analysis.',
  },
  image: {
    label: 'Image generation',
    placeholder: 'Search image generation components…',
    help: 'Optional model used to generate images.',
  },
  edit: {
    label: 'Image editing',
    placeholder: 'Search image edit components…',
    help: 'Optional model used to edit existing images.',
  },
  transcription: {
    label: 'Transcription',
    placeholder: 'Search Whisper/Moonshine/audio components…',
    help: 'Optional speech-to-text model.',
  },
  speech: {
    label: 'Text to speech',
    placeholder: 'Search TTS/speech components…',
    help: 'Optional text-to-speech model.',
  },
};

const NON_PLANNER_LABELS = new Set(['image', 'image-generation', 'edit', 'upscaling', 'speech', 'tts', 'text-to-speech', 'transcription', 'embeddings', 'embedding', 'reranking', 'reranker']);

function lowerLabels(m: ModelInfo): string[] {
  return (m.labels || []).map(label => label.toLowerCase().trim()).filter(Boolean);
}

function hasAnyLabel(m: ModelInfo, labels: string[]): boolean {
  const wanted = new Set(labels.map(label => label.toLowerCase()));
  return lowerLabels(m).some(label => wanted.has(label));
}

function isOmniComponentEligible(m: ModelInfo, role: OmniComponentRole): boolean {
  if (isCollectionModel(m)) return false;
  const cap = capabilityFromModelInfo(m);
  const labels = lowerLabels(m);

  switch (role) {
    case 'llm':
      return cap === 'chat' || cap === 'omni' || !labels.some(label => NON_PLANNER_LABELS.has(label));
    case 'vision':
      return cap === 'omni' || hasAnyLabel(m, ['vision', 'vlm', 'vision-language', 'image-input']);
    case 'image':
      return cap === 'image' || hasAnyLabel(m, ['image', 'image-generation', 'diffusion']);
    case 'edit':
      return hasAnyLabel(m, ['edit', 'image-edit', 'image-editing', 'upscaling']);
    case 'transcription':
      return cap === 'audio' || hasAnyLabel(m, ['audio', 'transcription', 'realtime-transcription', 'asr', 'stt', 'speech-to-text']);
    case 'speech':
      return cap === 'tts' || hasAnyLabel(m, ['tts', 'speech', 'text-to-speech']);
    default:
      return false;
  }
}

function omniComponentOptionFromModel(m: ModelInfo): OmniComponentOption {
  const id = modelName(m);
  const downloaded = Boolean((m as any).downloaded);
  const custom = Boolean((m as any).custom);
  const recipe = String((m as any).recipe || '');
  const labels = lowerLabels(m);
  const source: OmniComponentOptionSource = custom ? 'custom' : downloaded ? 'downloaded' : 'registered';
  const detailParts = [
    custom ? 'custom' : downloaded ? 'downloaded' : 'registered · will download when pulled',
    recipeLabel(recipe),
    labels.slice(0, 3).map(labelDisplay).join(', '),
  ].filter(Boolean);
  return {
    id,
    label: String(m.display_name || id),
    detail: detailParts.join(' · '),
    source,
    downloaded,
    custom,
    recipe,
    labels,
  };
}

function compareOmniComponentOptions(a: OmniComponentOption, b: OmniComponentOption): number {
  const sourceRank: Record<OmniComponentOptionSource, number> = { custom: 0, downloaded: 1, registered: 2 };
  const diff = sourceRank[a.source] - sourceRank[b.source];
  if (diff !== 0) return diff;
  return a.label.localeCompare(b.label);
}

interface OmniComponentPickerProps {
  role: OmniComponentRole;
  value: string;
  options: OmniComponentOption[];
  onChange: (value: string) => void;
  onHuggingFaceSearch?: (query: string) => void;
}

const OmniComponentPicker: React.FC<OmniComponentPickerProps> = ({ role, value, options, onChange, onHuggingFaceSearch }) => {
  const config = OMNI_COMPONENT_ROLE_CONFIG[role];
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const selected = options.find(option => option.id === value);
  const queryText = query.trim().toLowerCase();
  const visibleOptions = useMemo(() => {
    const filtered = queryText
      ? options.filter(option => {
        const haystack = `${option.label} ${option.id} ${option.detail} ${option.recipe} ${option.labels.join(' ')}`.toLowerCase();
        return haystack.includes(queryText);
      })
      : options;
    return filtered.slice(0, 40);
  }, [options, queryText]);
  const groups: Array<{ source: OmniComponentOptionSource; label: string; options: OmniComponentOption[] }> = [
    { source: 'custom' as OmniComponentOptionSource, label: 'Custom models', options: visibleOptions.filter(option => option.source === 'custom') },
    { source: 'downloaded' as OmniComponentOptionSource, label: 'Downloaded locally', options: visibleOptions.filter(option => option.source === 'downloaded') },
    { source: 'registered' as OmniComponentOptionSource, label: 'Registered registry models', options: visibleOptions.filter(option => option.source === 'registered') },
  ].filter(group => group.options.length > 0);

  return (
    <div className="omni-component-picker">
      <label className="omni-component-picker__label" title={config.help}>{config.label}{config.required ? ' *' : ''}</label>
      <div className="omni-component-picker__control">
        <input
          value={open ? query : (selected ? selected.label : '')}
          onFocus={() => { setOpen(true); setQuery(''); }}
          onChange={e => { setQuery(e.target.value); setOpen(true); }}
          onBlur={() => window.setTimeout(() => setOpen(false), 120)}
          placeholder={config.placeholder}
          aria-label={`${config.label} component`}
          autoComplete="off"
        />
        {value && !config.required && (
          <button
            type="button"
            className="omni-component-picker__clear"
            onMouseDown={e => e.preventDefault()}
            onClick={() => onChange('')}
            title={`Clear ${config.label}`}
          >×</button>
        )}
        <span className="omni-component-picker__chevron">⌄</span>
        {open && (
          <div className="omni-component-picker__menu" role="listbox">
            {groups.length > 0 ? groups.map(group => (
              <div className="omni-component-picker__group" key={group.source}>
                <div className="omni-component-picker__group-label">{group.label}</div>
                {group.options.map(option => (
                  <button
                    type="button"
                    key={option.id}
                    className={`omni-component-picker__option${option.id === value ? ' omni-component-picker__option--selected' : ''}`}
                    onMouseDown={e => e.preventDefault()}
                    onClick={() => { onChange(option.id); setQuery(''); setOpen(false); }}
                    role="option"
                    aria-selected={option.id === value}
                  >
                    <span className="omni-component-picker__option-name">{option.label}</span>
                    <span className="omni-component-picker__option-id">{option.id}</span>
                    <span className="omni-component-picker__option-detail">{option.detail}</span>
                  </button>
                ))}
              </div>
            )) : (
              <div className="omni-component-picker__empty">
                No compatible {config.label.toLowerCase()} model found. Use the main search or HuggingFace zone to download/register one first.
              </div>
            )}
            {queryText.length >= 2 && onHuggingFaceSearch && (
              <button
                type="button"
                className="omni-component-picker__hf-search"
                onMouseDown={e => e.preventDefault()}
                onClick={() => { onHuggingFaceSearch(query.trim()); setOpen(false); }}
              >
                Search HuggingFace for “{query.trim()}”
              </button>
            )}
          </div>
        )}
      </div>
      <div className="omni-component-picker__help">{selected ? selected.detail : config.help}</div>
    </div>
  );
};

/* ── Component ─────────────────────────────────────────────── */

interface ModelManagerProps {
  onModelSelect: (model: string) => void;
  selectedModel: string | null;
  accountSession: AccountSession;
}

const ModelManager: React.FC<ModelManagerProps> = ({ onModelSelect, selectedModel, accountSession }) => {
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);
  const [connectionStatus, setConnectionStatus] = useState(api.status);
  const [modelsLoading, setModelsLoading] = useState(api.isConnected && api.allModels.length === 0);
  const [loadingModel, setLoadingModel] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<{ modelName: string; message: string } | null>(null);
  const [pulling, setPulling] = useState<Record<string, number>>({});  // model → percent
  const pullAbortRef = useRef<Record<string, AbortController>>({});
  const [expandedModel, setExpandedModel] = useState<string | null>(null);
  const [searchQuery, setSearchQuery] = useState('');
  const [filterTab, setFilterTab] = useState<FilterTab>('all');
  const [showAllAvailable, setShowAllAvailable] = useState(false);
  const searchRef = useRef<HTMLInputElement>(null);

  // HuggingFace search state
  const [hfResults, setHfResults] = useState<HFModelResult[]>([]);
  const [hfLoading, setHfLoading] = useState(false);
  const [hfError, setHfError] = useState<string | null>(null);
  const [expandedHfModel, setExpandedHfModel] = useState<string | null>(null);
  const [pullingHf, setPullingHf] = useState<Record<string, number>>({}); // hf id → percent
  const pullHfAbortRef = useRef<Record<string, AbortController>>({});
  const [hfVariants, setHfVariants] = useState<Record<string, PullVariantsResult>>({}); // hf id → variants data
  const [hfVariantsLoading, setHfVariantsLoading] = useState<Record<string, boolean>>({}); // hf id → loading

  const [customModels, setCustomModels] = useState<ModelInfo[]>(() => loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  const [showCustomForm, setShowCustomForm] = useState(false);
  const [customError, setCustomError] = useState<string | null>(null);
  const [customDraft, setCustomDraft] = useState<CustomModelDraftState>(() => createEmptyCustomDraft());

  const [userPresets, setUserPresets] = useState<Preset[]>(loadUserPresets);
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(loadApplied);
  const [presetRailCollapsed, setPresetRailCollapsed] = useState(false);
  const [selectedRailPresetId, setSelectedRailPresetId] = useState<string>(DEFAULT_PRESET.id);
  const [presetRailHovered, setPresetRailHovered] = useState(false);
  const [hoveredRailPresetId, setHoveredRailPresetId] = useState<string | null>(null);
  const [presetNotice, setPresetNotice] = useState<string | null>(null);
  const [serverDefaultCtxSize, setServerDefaultCtxSize] = useState<number>(DEFAULT_CONTEXT_SIZE);
  const hasVisibleModelsRef = useRef(false);
  const modelsSnapshotRef = useRef<string>('');
  const loadedSnapshotRef = useRef<string>('');

  useEffect(() => {
    hasVisibleModelsRef.current = models.length > 0 || loadedModels.length > 0 || customModels.length > 0;
  }, [models.length, loadedModels.length, customModels.length]);

  const reloadCustomModels = useCallback(() => {
    setCustomModels(loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  }, [accountSession.storageScope]);

  useEffect(() => { reloadCustomModels(); }, [reloadCustomModels]);

  const reloadPresetState = useCallback(() => {
    setUserPresets(loadUserPresets());
    setAppliedPresets(loadApplied());
  }, [accountSession.storageScope]);

  useEffect(() => {
    reloadPresetState();
    window.addEventListener(PRESET_STORE_EVENT, reloadPresetState);
    return () => window.removeEventListener(PRESET_STORE_EVENT, reloadPresetState);
  }, [reloadPresetState]);

  const refresh = useCallback(async () => {
    if (!api.isConnected) {
      setModelsLoading(false);
      if (!hasVisibleModelsRef.current) {
        modelsSnapshotRef.current = '[]';
        loadedSnapshotRef.current = '[]';
        setModels([]);
        setLoadedModels([]);
      }
      return;
    }
    if (!hasVisibleModelsRef.current) setModelsLoading(true);
    try {
      const result = await api.refresh();
      if (result) {
        const nextModels = Array.isArray(result.models.data) ? result.models.data.filter((m): m is ModelInfo => !!m && !!modelName(m)) : [];
        const nextLoaded = Array.isArray(result.health.all_models_loaded) ? result.health.all_models_loaded.filter(m => !!m?.model_name) : [];
        const nextModelsSig = JSON.stringify(nextModels);
        const nextLoadedSig = JSON.stringify(nextLoaded);
        if (modelsSnapshotRef.current !== nextModelsSig) {
          modelsSnapshotRef.current = nextModelsSig;
          setModels(nextModels);
        }
        if (loadedSnapshotRef.current !== nextLoadedSig) {
          loadedSnapshotRef.current = nextLoadedSig;
          setLoadedModels(nextLoaded);
        }
      }
    } catch (err) {
      console.warn('Failed to refresh model list:', err);
    } finally {
      setModelsLoading(false);
    }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);

  const refreshServerDefaultCtxSize = useCallback(async () => {
    if (!api.isConnected) {
      setServerDefaultCtxSize(DEFAULT_CONTEXT_SIZE);
      return;
    }
    try {
      setServerDefaultCtxSize(await api.getDefaultContextSize() ?? DEFAULT_CONTEXT_SIZE);
    } catch {
      setServerDefaultCtxSize(DEFAULT_CONTEXT_SIZE);
    }
  }, []);

  useEffect(() => {
    if (connectionStatus === 'connected') refreshServerDefaultCtxSize();
    else setServerDefaultCtxSize(DEFAULT_CONTEXT_SIZE);
  }, [connectionStatus, refreshServerDefaultCtxSize]);

  // Re-fetch when server connection status changes (e.g. connects after initial mount)
  useEffect(() => {
    const unsub = api.onStatusChange((status) => {
      setConnectionStatus(status);
      if (status === 'connected') {
        refresh();
        refreshServerDefaultCtxSize();
      }
    });
    return unsub;
  }, [refresh, refreshServerDefaultCtxSize]);

  // Re-fetch when models are loaded/unloaded/deleted via any path (tools, other views)
  useEffect(() => {
    return api.onModelsChanged(() => { refresh(); });
  }, [refresh]);

  const applyServerDownloads = useCallback((downloads: DownloadProgressEvent[]) => {
    const active: Record<string, number> = {};
    let sawCompletedModel = false;
    downloads.forEach(download => {
      const type = String(download.type || '').toLowerCase();
      const id = String(download.id || '');
      if (type && type !== 'model') return;
      if (!type && id && !id.startsWith('model:')) return;
      const name = String(download.model_name || download.name || (id.startsWith('model:') ? id.slice('model:'.length) : '')).trim();
      if (!name) return;
      const status = String(download.status || '').toLowerCase();
      const isActive = download.running === true || status === 'downloading' || status === 'paused';
      if (isActive) active[name] = typeof download.percent === 'number' ? download.percent : 0;
      if (download.complete || status === 'completed' || status === 'error' || status === 'cancelled') sawCompletedModel = true;
    });
    setPulling(prev => {
      const next: Record<string, number> = {};
      Object.entries(prev).forEach(([name, value]) => {
        if (pullAbortRef.current[name]) next[name] = value;
      });
      return { ...next, ...active };
    });
    if (sawCompletedModel) refresh();
  }, [refresh]);

  useEffect(() => {
    if (connectionStatus !== 'connected') return;
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const tick = async () => {
      if (cancelled) return;
      try {
        applyServerDownloads(await api.downloads());
      } catch {
        // Older servers might not expose /downloads; normal SSE fallback still works.
      }
      if (!cancelled) timer = setTimeout(tick, 2000);
    };
    tick();
    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
    };
  }, [applyServerDownloads, connectionStatus]);

  /* ── HuggingFace debounced search ────────────────────────── */

  useEffect(() => {
    const q = searchQuery.trim();
    if (q.length < 2 || filterTab === 'omni') {
      setHfResults([]);
      setHfLoading(false);
      setHfError(null);
      setExpandedHfModel(null);
      return;
    }

    setHfLoading(true);
    setHfError(null);
    const ac = new AbortController();

    const timer = setTimeout(async () => {
      try {
        const results = await searchHuggingFace(q, ac.signal);
        setHfResults(results);
        setHfError(null);
      } catch (err) {
        if (!ac.signal.aborted) {
          setHfResults([]);
          setHfError(friendlyErrorMessage(err));
        }
      } finally {
        if (!ac.signal.aborted) setHfLoading(false);
      }
    }, 400);

    return () => {
      clearTimeout(timer);
      ac.abort();
    };
  }, [searchQuery, filterTab]);

  /* ── Actions ─────────────────────────────────────────────── */

  const findCurrentModel = (name: string): ModelInfo | null => {
    const target = name.toLowerCase();
    return allModels.find(mi => modelName(mi).toLowerCase() === target) || null;
  };

  const ensureCustomRegistration = async (model: ModelInfo | null) => {
    if (!model || !(model as any).custom) return;
    await api.pullModel(modelName(model), {}, customRegistrationOptions(model));
  };

  const ensureCustomCollectionComponentsRegistered = async (model: ModelInfo) => {
    if (!isCollectionModel(model)) return;
    for (const componentName of getCollectionComponents(model)) {
      const componentInfo = findCurrentModel(componentName);
      if (componentInfo && (componentInfo as any).custom) {
        await ensureCustomRegistration(componentInfo);
      }
    }
  };

  const loadModelRuntime = async (target: ModelInfo | string, visited = new Set<string>(), registered = new Set<string>()) => {
    const name = typeof target === 'string' ? target : modelName(target);
    if (!name) return;
    const key = name.toLowerCase();
    if (visited.has(key)) throw new Error(`Circular Omni collection reference: ${name}`);
    visited.add(key);

    const info = typeof target === 'string' ? findCurrentModel(name) : target;
    const components = info && isCollectionModel(info) ? getCollectionComponents(info) : [];

    if (components.length > 0) {
      // Mirror Lemonade main: collection models are registered as collection.omni
      // entries, but loading them means loading their concrete component models.
      // The collection itself stays the selected virtual model in the UI.
      if (!registered.has(key)) {
        await ensureCustomRegistration(info);
        registered.add(key);
      }
      for (const componentName of components) {
        const componentInfo = findCurrentModel(componentName);
        await loadModelRuntime(componentInfo || componentName, visited, registered);
      }
      visited.delete(key);
      return;
    }

    if (!registered.has(key)) {
      await ensureCustomRegistration(info);
      registered.add(key);
    }
    await api.loadModel(name, info ? customLoadOptions(info) : undefined, info);
    visited.delete(key);
  };

  const handleLoad = async (model: ModelInfo) => {
    if (loadingModel) return;
    const name = modelName(model);
    setLoadError(null);
    setLoadingModel(name);
    try {
      await loadModelRuntime(model);
      await refresh();
      onModelSelect(name);
    } catch (err) {
      console.error('Load failed:', err);
      const message = friendlyErrorMessage(err);
      setLoadError({ modelName: name, message });
      window.setTimeout(() => setLoadError(prev => prev?.modelName === name ? null : prev), 6000);
    }
    setLoadingModel(null);
  };

  const handleUnload = async (model: LoadedModel) => {
    setLoadingModel(model.model_name);
    const virtualComponents = Array.isArray(model.recipe_options?.components) && model.recipe_options?.virtual_collection === true
      ? model.recipe_options.components.filter((component): component is string => typeof component === 'string')
      : [];
    if (virtualComponents.length > 0) {
      await Promise.allSettled(virtualComponents.map(component => api.unloadModel(component)));
    } else {
      await api.unloadModel(model.model_name);
    }
    await refresh();
    setLoadingModel(null);
  };

  const handleDelete = async (model: ModelInfo) => {
    const name = modelName(model);
    if ((model as any).custom) {
      if (!confirm(`Delete custom model definition "${model.display_name || name}"? This does not remove external model files.`)) return;
      deleteCustomModel(accountSession.storageScope, String((model as any).id || name));
      reloadCustomModels();
      return;
    }
    if (!confirm(`Delete "${model.display_name || name}"? This removes the downloaded files. If the model is loaded, it will be unloaded first.`)) return;
    setLoadingModel(name);
    try {
      await api.deleteModel(name);
      await refresh();
    } catch (err: any) {
      console.error('Delete failed:', err);
    }
    setLoadingModel(null);
  };

  const handlePull = async (model: ModelInfo) => {
    const name = modelName(model);
    if (pulling[name] !== undefined) return;
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));

    await ensureCustomCollectionComponentsRegistered(model);

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        refresh();
      },
      onError: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(name, callbacks, customRegistrationOptions(model));
  };

  const handleCancelPull = async (name: string) => {
    pullAbortRef.current[name]?.abort();
    await api.controlDownload(`model:${name}`, 'cancel').catch(() => undefined);
    delete pullAbortRef.current[name];
    setPulling(p => { const next = { ...p }; delete next[name]; return next; });
  };

  const handlePullAndLoad = async (model: ModelInfo) => {
    const name = modelName(model);
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));

    await ensureCustomCollectionComponentsRegistered(model);

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: async () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        await refresh();
        setLoadingModel(name);
        try {
          await loadModelRuntime(model, new Set<string>(), new Set<string>([name.toLowerCase()]));
          await refresh();
          onModelSelect(name);
        } catch (err) {
          console.error('Load after pull failed:', err);
          const message = friendlyErrorMessage(err);
          setLoadError({ modelName: name, message });
          window.setTimeout(() => setLoadError(prev => prev?.modelName === name ? null : prev), 6000);
        }
        setLoadingModel(null);
      },
      onError: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(name, callbacks, customRegistrationOptions(model));
  };

  const handleHfPull = async (hfId: string, variantName: string, recipe: string) => {
    if (pullingHf[hfId] !== undefined) return;
    const vdata = hfVariants[hfId];
    const suggestedName = vdata?.suggested_name || hfId.split('/').pop() || hfId;
    const modelName = `user.${suggestedName}`;
    const checkpoint = `${hfId}:${variantName}`;
    const ac = new AbortController();
    pullHfAbortRef.current[hfId] = ac;
    setPullingHf(p => ({ ...p, [hfId]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPullingHf(p => ({ ...p, [hfId]: data.percent! }));
        }
      },
      onComplete: () => {
        delete pullHfAbortRef.current[hfId];
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
        refresh();
      },
      onError: (err) => {
        console.error('HF pull failed:', err);
        delete pullHfAbortRef.current[hfId];
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(modelName, callbacks, { checkpoint, recipe });
  };

  const handleCancelHfPull = async (hfId: string) => {
    pullHfAbortRef.current[hfId]?.abort();
    const vdata = hfVariants[hfId];
    const suggestedName = vdata?.suggested_name || hfId.split('/').pop() || hfId;
    await api.controlDownload(`model:user.${suggestedName}`, 'cancel').catch(() => undefined);
    delete pullHfAbortRef.current[hfId];
    setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
  };

  const fetchHfVariants = async (hfId: string) => {
    if (hfVariants[hfId] || hfVariantsLoading[hfId]) return;
    setHfVariantsLoading(prev => ({ ...prev, [hfId]: true }));
    try {
      const result = await api.pullVariants(hfId);
      setHfVariants(prev => ({ ...prev, [hfId]: result }));
    } catch (err) {
      console.error('Failed to fetch variants for', hfId, err);
    }
    setHfVariantsLoading(prev => ({ ...prev, [hfId]: false }));
  };


  const handleCustomDraftChange = (patch: Partial<CustomModelDraftState>) => {
    setCustomDraft(prev => ({ ...prev, ...patch }));
    setCustomError(null);
  };

  const openCustomForm = (mode: CustomFormMode = 'model') => {
    setCustomDraft(createEmptyCustomDraft(mode));
    setCustomError(null);
    setShowCustomForm(true);
  };

  const closeCustomForm = () => {
    setShowCustomForm(false);
    setCustomError(null);
  };

  const defaultRecipeForCapability = (capability: CustomModelCapability, omniSource: 'single' | 'collection' = customDraft.omniSource) => {
    if (capability === 'image') return 'sd-cpp';
    if (capability === 'audio') return 'whispercpp';
    if (capability === 'tts') return 'kokoro';
    if (capability === 'omni' && omniSource === 'collection') return 'collection.omni';
    return 'llamacpp';
  };

  const handleSaveCustomModel = (e: React.FormEvent) => {
    e.preventDefault();
    setCustomError(null);
    try {
      if (!customDraft.displayName.trim()) {
        throw new Error('Enter a model name.');
      }
      if (customDraft.capability === 'omni' && customDraft.omniSource === 'collection' && !customDraft.llmComponent.trim()) {
        throw new Error('Select a planner LLM for the Omni collection.');
      }
      const componentRoles = {
        llm: customDraft.llmComponent,
        vision: customDraft.visionComponent,
        image: customDraft.imageComponent,
        edit: customDraft.editComponent,
        transcription: customDraft.transcriptionComponent,
        speech: customDraft.speechComponent,
      };
      const components = customDraft.omniSource === 'collection'
        ? Object.values(componentRoles).map(v => v.trim()).filter(Boolean)
        : [];
      const saved = upsertCustomModel(accountSession.storageScope, {
        name: implicitCustomModelName(customDraft.displayName, customDraft.checkpoint, customDraft.capability === 'omni' ? 'omni-model' : 'custom-model'),
        displayName: customDraft.displayName,
        checkpoint: customDraft.checkpoint,
        recipe: recipeOptionsForCustomDraft(customDraft.capability, customDraft.omniSource).some(option => option.value === customDraft.recipe) ? customDraft.recipe : recipeOptionsForCustomDraft(customDraft.capability, customDraft.omniSource)[0]?.value || customDraft.recipe,
        capability: customDraft.capability,
        maxContextWindow: customDraft.capability === 'omni' ? undefined : (customDraft.maxContextWindow.trim() && Number.isFinite(Number(customDraft.maxContextWindow)) ? Number(customDraft.maxContextWindow) : undefined),
        labels: customDraft.labels.split(',').map(l => l.trim()).filter(Boolean),
        components,
        componentRoles,
      });
      reloadCustomModels();
      setShowCustomForm(false);
      setSearchQuery(saved.display_name || saved.name);
      setCustomDraft(createEmptyCustomDraft());
    } catch (err) {
      setCustomError(err instanceof Error ? err.message : 'Could not save custom model.');
    }
  };

  /* ── Derived data ────────────────────────────────────────── */

  const allModels = useMemo(() => {
    const seen = new Set<string>();
    const merged: ModelInfo[] = [];
    for (const m of customModels) {
      const name = modelName(m).toLowerCase();
      if (!name || seen.has(name)) continue;
      seen.add(name);
      merged.push(m);
    }
    for (const m of models) {
      const name = modelName(m).toLowerCase();
      if (!name || seen.has(name)) continue;
      seen.add(name);
      merged.push(m);
    }
    return merged;
  }, [customModels, models]);

  const allPresets = useMemo(() => [DEFAULT_PRESET, ...STARTERS, ...userPresets], [userPresets]);

  const activePresetForName = useCallback((name: string): Preset => {
    const presetId = appliedPresets[name] || DEFAULT_PRESET.id;
    return allPresets.find(p => p.id === presetId) || DEFAULT_PRESET;
  }, [allPresets, appliedPresets]);

  const focusedModelName = expandedModel || selectedModel || '';
  const focusedModelInfo = useMemo(() => {
    if (!focusedModelName) return null;
    const info = allModels.find(m => modelName(m) === focusedModelName) || ({ id: focusedModelName, name: focusedModelName } as ModelInfo);
    const loaded = loadedModels.find(m => m.model_name === focusedModelName);
    return withLoadedRecipeOptions(info, loaded);
  }, [allModels, focusedModelName, loadedModels]);
  const focusedPreset = focusedModelName ? activePresetForName(focusedModelName) : null;
  const selectedRailPreset = allPresets.find(p => p.id === selectedRailPresetId) || DEFAULT_PRESET;
  const railSummaryPreset = (focusedModelName && focusedPreset) ? focusedPreset : selectedRailPreset;
  const highlightedPresetId = presetRailHovered ? (hoveredRailPresetId || selectedRailPreset.id) : null;
  const assignedToRailSummaryPreset = useMemo(
    () => allModels.filter(m => canShowPresetHighlight(m) && activePresetForName(modelName(m)).id === railSummaryPreset.id),
    [allModels, activePresetForName, railSummaryPreset.id],
  );

  const handlePresetRailPick = useCallback((preset: Preset) => {
    setSelectedRailPresetId(preset.id);
    if (!focusedModelInfo || !focusedModelName) return;
    if (preset.id !== DEFAULT_PRESET.id && !isCompatible(preset, focusedModelInfo)) {
      setPresetNotice(`“${preset.name}” is not compatible with ${focusedModelName}.`);
      window.setTimeout(() => setPresetNotice(null), 2800);
      return;
    }
    setAppliedPresets(prev => {
      const next = { ...prev };
      if (preset.id === DEFAULT_PRESET.id) delete next[focusedModelName];
      else next[focusedModelName] = preset.id;
      saveApplied(next);
      return next;
    });
    setPresetNotice(`${focusedModelName} → ${preset.name}`);
    window.setTimeout(() => setPresetNotice(null), 2200);
  }, [focusedModelInfo, focusedModelName]);

  const omniComponentOptions = useMemo(() => {
    const roles: Record<OmniComponentRole, OmniComponentOption[]> = {
      llm: [],
      vision: [],
      image: [],
      edit: [],
      transcription: [],
      speech: [],
    };
    const seenByRole: Record<OmniComponentRole, Set<string>> = {
      llm: new Set<string>(),
      vision: new Set<string>(),
      image: new Set<string>(),
      edit: new Set<string>(),
      transcription: new Set<string>(),
      speech: new Set<string>(),
    };
    for (const m of allModels) {
      for (const role of Object.keys(roles) as OmniComponentRole[]) {
        if (!isOmniComponentEligible(m, role)) continue;
        const option = omniComponentOptionFromModel(m);
        const key = option.id.toLowerCase();
        if (seenByRole[role].has(key)) continue;
        seenByRole[role].add(key);
        roles[role].push(option);
      }
    }
    for (const role of Object.keys(roles) as OmniComponentRole[]) {
      roles[role].sort(compareOmniComponentOptions);
    }
    return roles;
  }, [allModels]);

  const displayLoadedModels = useMemo(
    () => withVirtualLoadedCollections(loadedModels, allModels),
    [loadedModels, allModels]
  );

  const loadedNames = useMemo(
    () => new Set(displayLoadedModels.map(m => m.model_name)),
    [displayLoadedModels]
  );

  const { downloaded, available } = useMemo(() => {
    const dl: ModelInfo[] = [];
    const av: ModelInfo[] = [];
    for (const m of allModels) {
      const name = modelName(m);
      if (loadedNames.has(name)) continue;
      const isDownloaded = isCollectionModel(m)
        ? isCollectionFullyDownloaded(m, allModels)
        : Boolean((m as any).downloaded);
      if (isDownloaded) dl.push(m);
      else av.push(m);
    }
    return { downloaded: dl, available: av };
  }, [allModels, loadedNames]);

  const applyFilter = useCallback((list: ModelInfo[]) => {
    let filtered = list;

    // Type filter. The Omni tab is intentionally collection-only: single VLMs
    // are useful chat/vision models, but they are not Omni collections.
    if (filterTab !== 'all') {
      filtered = filtered.filter(m => {
        if (filterTab === 'omni') return isCollectionModel(m);
        const type = modelType(m);
        if (filterTab === 'embedding') return type === 'embedding' || type === 'reranking';
        return type === filterTab;
      });
    }

    // Search
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      filtered = filtered.filter(m => {
        const name = modelName(m).toLowerCase();
        const labels = modelLabels(m).join(' ').toLowerCase();
        const recipe = ((m as any).recipe || '').toLowerCase();
        return name.includes(q) || labels.includes(q) || recipe.includes(q);
      });
    }

    return filtered;
  }, [filterTab, searchQuery]);

  const filteredDownloaded = useMemo(() => applyFilter(downloaded), [applyFilter, downloaded]);
  const filteredAvailable = useMemo(() => applyFilter(available), [applyFilter, available]);

  // Filter running models by search/type too
  const filteredRunning = useMemo(() => {
    if (filterTab === 'all' && !searchQuery.trim()) return displayLoadedModels;
    return displayLoadedModels.filter(m => {
      // Type filter
      if (filterTab !== 'all') {
        const info = allModels.find(mi => modelName(mi) === m.model_name);
        if (filterTab === 'omni') {
          if (!(info ? isCollectionModel(info) : loadedIsVirtualOmniCollection(m))) return false;
        } else {
          const cap = info ? capabilityFromModelInfo(info) : capabilityFromLoaded(m);
          const type = cap === 'chat' || cap === 'unknown' ? 'llm' : cap;
          if (filterTab === 'embedding') {
            if (type !== 'embedding' && type !== 'reranking') return false;
          } else if (type !== filterTab) return false;
        }
      }
      // Search
      if (searchQuery.trim()) {
        const q = searchQuery.toLowerCase();
        if (!String(m.model_name || '').toLowerCase().includes(q) && !String(m.recipe || '').toLowerCase().includes(q)) return false;
      }
      return true;
    });
  }, [displayLoadedModels, filterTab, searchQuery, allModels]);

  // Available zone: show first N unless expanded or searching
  const AVAILABLE_INITIAL = 20;
  const visibleAvailable = useMemo(() => {
    if (showAllAvailable || searchQuery.trim() || filterTab !== 'all') return filteredAvailable;
    return filteredAvailable.slice(0, AVAILABLE_INITIAL);
  }, [filteredAvailable, showAllAvailable, searchQuery, filterTab]);
  const hiddenAvailableCount = filteredAvailable.length - visibleAvailable.length;

  // HuggingFace results — exclude models already in local registry
  const filteredHfResults = useMemo(() => {
    if (hfResults.length === 0) return [];
    // The Omni filter is collection-only. HuggingFace search returns individual
    // GGUF checkpoints, so showing them here would leak LLMs into the Omni tab.
    if (filterTab === 'omni') return [];
    const localIds = new Set(allModels.map(m => modelName(m).toLowerCase()));
    return hfResults.filter(r => !localIds.has(r.id.toLowerCase()));
  }, [hfResults, allModels, filterTab]);

  /* ── Toggle detail ───────────────────────────────────────── */
  const toggleDetail = (name: string) => {
    setExpandedModel(prev => prev === name ? null : name);
  };

  /* ── Render helpers ──────────────────────────────────────── */

  const renderLabels = (labels: string[]) => {
    const normalizedLabels = labels.map(label => String(label).trim().toLowerCase()).filter(Boolean);
    if (normalizedLabels.length === 0) return null;
    const displayLabels = [...new Map(normalizedLabels
      .filter(l => l !== 'llamacpp' && l !== 'custom')
      .map(l => [labelDisplay(l).toLowerCase(), l] as const)).values()];
    if (displayLabels.length === 0) return null;
    return (
      <div className="row__labels">
        {displayLabels.map(l => (
          <span key={l} className="row__label row__label--with-icon">
            <CapabilityIcon capability={iconForCapabilityLabel(l)} size={10} />
            {labelDisplay(l)}
          </span>
        ))}
      </div>
    );
  };

  const renderModelDetail = (m: ModelInfo, liveCtxSize?: number) => {
    const name = modelName(m);
    const checkpoint = (m as any).checkpoint || '';
    const checkpoints = (m as any).checkpoints || {};
    const recipe = (m as any).recipe || '';
    const activePreset = activePresetForName(name);
    const capability = capabilityFromModelInfo(m);
    const isDefaultPassthrough = activePreset.id === DEFAULT_PRESET.id && !presetHasApplicablePreviewOverrides(activePreset, capability);
    const explicitCtx = positiveNumber(liveCtxSize) ?? positiveNumber((m as any).max_context_window);
    const displayCtx = contextSizeForDisplay(m, liveCtxSize, serverDefaultCtxSize);
    const activePresetLines = effectivePresetParamPreviewLines(activePreset, m, displayCtx);
    const showContext = Boolean(displayCtx);
    const compositeModels = Array.isArray((m as any).composite_models) ? (m as any).composite_models : [];
    const collectionComponents = getCollectionComponents(m);
    const detailCapabilityLabels = capabilityLabelsForModel(m, allModels);
    const url = hfUrl(checkpoint);
    const exportData = {
      ...m,
      ...(explicitCtx ? { max_context_window: explicitCtx } : {}),
    };

    return (
      <div className="row__detail">
        <div className="detail__grid">
          {/* Left column: metadata */}
          <div className="detail__meta">
            <div className="detail__field">
              <span className="detail__label">Backend</span>
              <span className="detail__value">{recipeLabel(recipe)}</span>
            </div>
            <div className="detail__field">
              <span className="detail__label">Active preset</span>
              <span className="detail__value detail__preset-value"><PresetIcon preset={activePreset} /> {activePreset.name}</span>
              <span className="detail__hint">{loadedNames.has(name) ? 'Runtime behavior applies now. Load options apply after reload.' : 'Applies on load. Backend remains auto by Lemonade.'}</span>
            </div>
            <div className="detail__field">
              <span className="detail__label">{isDefaultPassthrough ? 'Model defaults' : 'Preset settings'}</span>
              <span className="detail__value detail__param-lines">{activePresetLines.map(line => <span key={line}>{line}</span>)}</span>
              {isDefaultPassthrough && <span className="detail__hint">No preset override is sent; Lemonade uses the model's current defaults.</span>}
            </div>
            {m.size && (
              <div className="detail__field">
                <span className="detail__label">Size</span>
                <span className="detail__value">{formatSize(m.size)}</span>
              </div>
            )}
            {showContext && (
              <div className="detail__field">
                <span className="detail__label">Context</span>
                <span className="detail__value">{contextLabel(displayCtx!)} tokens</span>
              </div>
            )}
            {detailCapabilityLabels.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Capabilities</span>
                <div className="detail__caps">
                  {detailCapabilityLabels.map(l => (
                    <span key={l} className="detail__cap detail__cap--with-icon">
                      <CapabilityIcon capability={iconForCapabilityLabel(l)} size={12} />
                      {labelDisplay(l)}
                    </span>
                  ))}
                </div>
              </div>
            )}
          </div>

          {/* Right column: checkpoint / HF link */}
          <div className="detail__source">
            <div className="detail__field">
              <span className="detail__label">Checkpoint</span>
              <span className="detail__value detail__value--mono">{checkpoint || '—'}</span>
            </div>
            {Object.keys(checkpoints).length > 1 && (
              <div className="detail__field">
                <span className="detail__label">Components</span>
                {Object.entries(checkpoints).map(([k, v]) => (
                  <div key={k} className="detail__checkpoint">
                    <span className="detail__ck-key">{k}</span>
                    <span className="detail__ck-val">{String(v)}</span>
                  </div>
                ))}
              </div>
            )}
            {compositeModels.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Includes</span>
                <div className="detail__caps">
                  {compositeModels.map((c: string) => (
                    <span key={c} className="detail__cap">{c}</span>
                  ))}
                </div>
              </div>
            )}
            {collectionComponents.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Omni components</span>
                <div className="detail__caps">
                  {collectionComponents.map(component => (
                    <span key={component} className="detail__cap">{component}</span>
                  ))}
                </div>
              </div>
            )}
            {url && (
              <a className="detail__hf-link" href={url} target="_blank" rel="noopener noreferrer">
                View on Hugging Face
              </a>
            )}
            <button
              type="button"
              className="detail__json-export"
              onClick={(event) => { event.stopPropagation(); exportJsonFile(name, exportData); }}
            >
              Export JSON
            </button>
          </div>
        </div>
      </div>
    );
  };

  const renderRunningModel = (m: LoadedModel) => {
    if (!m?.model_name) return null;
    const info = allModels.find(mi => modelName(mi) === m.model_name);
    const cap = info ? capabilityFromModelInfo(info) : capabilityFromLoaded(m);
    const componentCount = Array.isArray(m.recipe_options?.components) ? m.recipe_options.components.length : 0;
    const runningCtx = supportsContextDisplay(cap)
      ? (positiveNumber(m.recipe_options?.ctx_size) ?? (info ? contextSizeForDisplay(info, undefined, serverDefaultCtxSize) : serverDefaultCtxSize))
      : undefined;
    const isActive = selectedModel === m.model_name;
    const selectable = canSelectInComposer(m) || (cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'tts');
    const activePreset = activePresetForName(m.model_name);
    const isPresetHighlighted = Boolean(highlightedPresetId
      && activePreset.id === highlightedPresetId
      && (info ? canShowPresetHighlight(info) : !loadedIsVirtualOmniCollection(m)));
    return (
      <div className={`row row--running${isActive ? ' row--active' : ''}${isPresetHighlighted ? ' row--preset-highlight' : ''}`} key={m.model_name}>
        <div className="row__summary">
          <button type="button" className="row__content" onClick={() => toggleDetail(m.model_name)} aria-expanded={expandedModel === m.model_name}>
            <div className="row__main">
              <BackendBadge recipe={m.recipe} running />
              <div className="row__text">
                <span className="row__name-wrap"><span className="row__name">{m.model_name}</span>{info && (info as any).custom && <span className="row__label row__label--custom">Custom</span>}</span>
                <span className="row__sub">
                  {recipeLabel(m.recipe)} · {(m.device || 'device').toUpperCase()}
                  {` · ${capabilityIcon(cap)} ${capabilityLabel(cap)}`}
                  {runningCtx ? ` · ${contextLabel(runningCtx)} ctx` : ''}
                  {componentCount > 0 ? ` · ${componentCount} components loaded` : ''}
                </span>
                <span className="row__preset-pill"><PresetIcon preset={activePreset} /> {activePreset.name}</span>
              </div>
            </div>
            <span className="row__expand">{expandedModel === m.model_name ? '▾' : '▸'}</span>
          </button>
          <div className="row__right">
            <CopyInlineButton text={m.model_name} />
            <span className="row__status-pill row__status-pill--running">
              <span className="row__pulse" /> {isActive ? `Active ${capabilityLabel(cap)} mode` : 'Running'}
            </span>
            {selectable && !isActive && (
              <button className="row__action" onClick={(e) => { e.stopPropagation(); onModelSelect(m.model_name); }}>
                Use in {capabilityLabel(cap)} mode
              </button>
            )}
            <button
              className="row__action row__action--unload"
              onClick={(e) => { e.stopPropagation(); handleUnload(m); }}
              disabled={loadingModel === m.model_name}
            >
              {loadingModel === m.model_name ? 'Working…' : 'Unload'}
            </button>
            <button
              className="row__action row__action--delete"
              onClick={(e) => {
                e.stopPropagation();
                const infoForDelete = allModels.find(mi => modelName(mi) === m.model_name);
                if (infoForDelete) handleDelete(infoForDelete);
              }}
              disabled={loadingModel === m.model_name}
              title={info && (info as any).custom ? 'Delete custom model definition' : 'Delete model files'}
            >
              <Icon name="x" size={14} />
            </button>
          </div>
        </div>

        {expandedModel === m.model_name && (() => {
          // find matching ModelInfo for detail
          const info = allModels.find(mi => modelName(mi) === m.model_name);
          if (!info) return null;
          const liveInfo = withLoadedRecipeOptions(info, m) || info;
          const liveCtx = m.recipe_options?.ctx_size as number | undefined;
          return (
            <>
              {renderModelDetail(liveInfo, liveCtx)}
              {m.recipe_options && Object.keys(m.recipe_options).length > 0 && (
                <div className="row__detail row__detail--live">
                  <div className="detail__field">
                    <span className="detail__label">Active Recipe Options</span>
                    <div className="detail__recipe-options">
                      {Object.entries(m.recipe_options).map(([k, v]) => (
                        <span key={k} className="detail__recipe-opt">
                          <span className="detail__ro-key">{k}</span>
                          <span className="detail__ro-val">{String(v)}</span>
                        </span>
                      ))}
                    </div>
                  </div>
                </div>
              )}
            </>
          );
        })()}
      </div>
    );
  };

  const renderModelRow = (m: ModelInfo, isDownloaded: boolean) => {
    const name = modelName(m);
    if (!name) return null;
    const isCollection = isCollectionModel(m);
    const isLoading = loadingModel === name;
    const pullPercent = pulling[name];
    const isPulling = pullPercent !== undefined;
    const activePreset = activePresetForName(name);
    const rowCtx = contextSizeForDisplay(m, undefined, serverDefaultCtxSize);
    const isPresetHighlighted = Boolean(highlightedPresetId
      && activePreset.id === highlightedPresetId
      && canShowPresetHighlight(m));

    return (
      <div className={`row${expandedModel === name ? ' row--expanded' : ''}${isPresetHighlighted ? ' row--preset-highlight' : ''}`} key={name}>
        <div className="row__summary">
          <button type="button" className="row__content" onClick={() => toggleDetail(name)} aria-expanded={expandedModel === name}>
            <div className="row__main">
              <BackendBadge recipe={String((m as any).recipe || '')} />
              <div className="row__text">
                <span className="row__name-wrap"><span className="row__name">{m.display_name || name}</span>{(m as any).custom && <span className="row__label row__label--custom">Custom</span>}</span>
                <span className="row__sub">
                  {recipeLabel((m as any).recipe || '')}
                  {isCollection ? ` · ${collectionComponentLabel(m)}` : ''}
                  {m.size ? ` · ${formatSize(m.size)}` : ''}
                  {rowCtx ? ` · ${contextLabel(rowCtx)} ctx` : ''}
                </span>
                {renderLabels(capabilityLabelsForModel(m, allModels))}
                <span className="row__preset-pill"><PresetIcon preset={activePreset} /> {activePreset.name}</span>
              </div>
            </div>
            <span className="row__expand">{expandedModel === name ? '▾' : '▸'}</span>
          </button>
          <div className="row__right">
            <CopyInlineButton text={name} />
            {isPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPercent}%` }} />
                </div>
                <span className="row__progress-text">{pullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(e) => { e.stopPropagation(); handleCancelPull(name); }}
                  title="Cancel download"
                ><Icon name="x" size={13} /></button>
              </div>
            ) : isDownloaded ? (
              <>
                <span className="row__status-pill row__status-pill--ready">Ready</span>
                <button
                  className="row__action"
                  onClick={(e) => { e.stopPropagation(); handleLoad(m); }}
                  disabled={isLoading}
                >
                  {isLoading ? 'Loading…' : <><Icon name="play" size={13} /> Load</>}
                </button>
                <button
                  className="row__action row__action--delete"
                  onClick={(e) => { e.stopPropagation(); handleDelete(m); }}
                  disabled={isLoading}
                  title={(m as any).custom ? 'Delete custom model definition' : 'Delete model files'}
                >
                  <Icon name="x" size={14} />
                </button>
              </>
            ) : (
              <>
                <button
                  className="row__action row__action--download"
                  onClick={(e) => { e.stopPropagation(); handlePull(m); }}
                  disabled={isPulling}
                >
                  <Icon name="download" size={13} /> Download
                </button>
                <button
                  className="row__action"
                  onClick={(e) => { e.stopPropagation(); handlePullAndLoad(m); }}
                  disabled={isPulling}
                >
                  <><Icon name="download" size={13} /><Icon name="play" size={13} /> Get & Load</>
                </button>
              </>
            )}
          </div>
        </div>

        {expandedModel === name && renderModelDetail(m)}
        {loadError?.modelName === name && (
          <div className="row__load-error">
            <Icon name="alert" size={13} /> {loadError.message}
          </div>
        )}
      </div>
    );
  };

  const renderHfRow = (r: HFModelResult) => {
    const isExpanded = expandedHfModel === r.id;
    const pipelineTag = r.pipeline_tag || '';
    const displayTags = (r.tags || [])
      .filter(t => t !== 'gguf' && t !== 'transformers' && t !== 'pytorch' && t !== 'safetensors')
      .slice(0, 5);
    const hfPullPercent = pullingHf[r.id];
    const isHfPulling = hfPullPercent !== undefined;

    // Variant data from the server (fetched on expand)
    const vdata = hfVariants[r.id];
    const isLoadingVariants = hfVariantsLoading[r.id] || false;
    const recipeBadge = vdata ? (RECIPE_BADGES[vdata.recipe] || vdata.recipe) : '';

    const handleExpand = () => {
      const next = isExpanded ? null : r.id;
      setExpandedHfModel(next);
      if (next) fetchHfVariants(r.id);
    };

    return (
      <div className={`row row--hf${isExpanded ? ' row--expanded' : ''}`} key={r.id}>
        <div className="row__summary">
          <button type="button" className="row__content" onClick={handleExpand} aria-expanded={isExpanded}>
            <div className="row__main">
              <div className="row__icon row__icon--hf"><Icon name="download" size={18} /></div>
              <div className="row__text">
                <span className="row__name-wrap"><span className="row__name">{r.id}</span></span>
                <span className="row__sub">
                  {recipeBadge ? `${recipeBadge} · ` : ''}{pipelineTag && `${pipelineTag} · `}
                  {formatDownloads(r.downloads)} downloads · {formatDownloads(r.likes)} likes
                </span>
                {displayTags.length > 0 && (
                  <div className="row__labels">
                    {displayTags.map(t => (
                      <span key={t} className="row__label row__label--hf">{t}</span>
                    ))}
                  </div>
                )}
              </div>
            </div>
            <span className="row__expand">{isExpanded ? '▾' : '▸'}</span>
          </button>
          <div className="row__right">
            <CopyInlineButton text={r.id} title="Copy repository name" />
            {isHfPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${hfPullPercent}%` }} />
                </div>
                <span className="row__progress-text">{hfPullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(e) => { e.stopPropagation(); handleCancelHfPull(r.id); }}
                  title="Cancel download"
                ><Icon name="x" size={13} /></button>
              </div>
            ) : (
              <button
                className="row__action row__action--download"
                onClick={(e) => { e.stopPropagation(); handleExpand(); }}
                title="Expand to pick a variant to download"
              >
                <Icon name="download" size={13} /> Download
              </button>
            )}
            <a
              className="row__action row__action--hf-link"
              href={`https://huggingface.co/${r.id}`}
              target="_blank"
              rel="noopener noreferrer"
              onClick={e => e.stopPropagation()}
            >
              View
            </a>
          </div>
        </div>

        {isExpanded && (
          <div className="row__detail row__detail--hf">
            <div className="detail__grid">
              <div className="detail__meta">
                <div className="detail__field">
                  <span className="detail__label">Repository</span>
                  <span className="detail__value detail__value--mono">{r.id}</span>
                </div>
                {pipelineTag && (
                  <div className="detail__field">
                    <span className="detail__label">Pipeline</span>
                    <span className="detail__value">{pipelineTag}</span>
                  </div>
                )}
                {vdata && (
                  <>
                    <div className="detail__field">
                      <span className="detail__label">Backend</span>
                      <span className="detail__value">{RECIPE_BADGES[vdata.recipe] || vdata.recipe}</span>
                    </div>
                    {vdata.suggested_labels.length > 0 && (
                      <div className="detail__field">
                        <span className="detail__label">Capabilities</span>
                        <span className="detail__value">{vdata.suggested_labels.join(', ')}</span>
                      </div>
                    )}
                  </>
                )}
                {r.createdAt && (
                  <div className="detail__field">
                    <span className="detail__label">Created</span>
                    <span className="detail__value">{new Date(r.createdAt).toLocaleDateString()}</span>
                  </div>
                )}
              </div>
              <div className="detail__source">
                {isLoadingVariants && (
                  <div className="detail__field">
                    <span className="detail__label">Loading variants…</span>
                  </div>
                )}
                {vdata && vdata.variants.length > 0 && (
                  <div className="detail__field">
                    <span className="detail__label">Variants — pick one to download</span>
                    <div className="hf-detail__gguf-list">
                      {vdata.variants.map(v => (
                        <button
                          key={v.name}
                          className="hf-detail__gguf-btn"
                          disabled={isHfPulling}
                          onClick={() => handleHfPull(r.id, v.name, vdata.recipe)}
                        >
                          <span className="hf-detail__gguf-name">
                            {v.name}{v.sharded ? ' (sharded)' : ''}
                          </span>
                          <span className="hf-detail__gguf-size">{formatBytes(v.size_bytes)}</span>
                          <span className="hf-detail__gguf-action">Download</span>
                        </button>
                      ))}
                    </div>
                  </div>
                )}
                <a
                  className="detail__hf-link"
                  href={`https://huggingface.co/${r.id}`}
                  target="_blank"
                  rel="noopener noreferrer"
                >
                  View on Hugging Face
                </a>
              </div>
            </div>
          </div>
        )}
      </div>
    );
  };

  const renderPresetRail = () => (
    <aside
      className={`context-rail context-rail--presets${presetRailCollapsed ? ' is-collapsed' : ''}`}
      aria-label="Preset rail"
      onMouseEnter={() => setPresetRailHovered(true)}
      onMouseLeave={() => { setPresetRailHovered(false); setHoveredRailPresetId(null); }}
    >
      <div className="context-rail__head">
        <button type="button" className="context-rail__toggle" onClick={() => setPresetRailCollapsed(v => !v)} aria-label="Toggle preset rail">☰</button>
        <div className="context-rail__title-wrap">
          <span className="context-rail__eyebrow">By model</span>
          <strong className="context-rail__title">{focusedModelName ? `For ${focusedModelName}` : 'Presets'}</strong>
        </div>
      </div>
      <div className="context-rail__body">
        <div className="preset-rail-summary">
          <span className="preset-rail-summary__label">Selected preset</span>
          <strong><PresetIcon preset={railSummaryPreset} /> {railSummaryPreset.name}</strong>
          <span>{focusedModelName ? 'Active for this model' : `${assignedToRailSummaryPreset.length} model${assignedToRailSummaryPreset.length === 1 ? '' : 's'} assigned`}</span>
          <span className="preset-param-lines">{effectivePresetParamPreviewLines(railSummaryPreset, focusedModelInfo, focusedModelInfo ? contextSizeForDisplay(focusedModelInfo, undefined, serverDefaultCtxSize) : serverDefaultCtxSize).map(line => <span key={line}>{line}</span>)}</span>
        </div>
        <p className="context-rail__hint">
          {focusedModelName ? 'Click a preset to assign it to this model. Backend selection stays automatic.' : 'Hover or pick a preset to outline matching models.'}
        </p>
        <div className="preset-rail-list">
          {allPresets.map(preset => {
            const isActive = focusedModelName ? focusedPreset?.id === preset.id : selectedRailPreset.id === preset.id;
            const disabled = Boolean(focusedModelInfo && preset.id !== DEFAULT_PRESET.id && !isCompatible(preset, focusedModelInfo));
            return (
              <button
                key={preset.id}
                type="button"
                className={`preset-rail-card${isActive ? ' is-active' : ''}${disabled ? ' is-disabled' : ''}`}
                onClick={() => handlePresetRailPick(preset)}
                onMouseEnter={() => setHoveredRailPresetId(preset.id)}
                onFocus={() => setHoveredRailPresetId(preset.id)}
                onBlur={() => setHoveredRailPresetId(null)}
                title={disabled ? 'Incompatible with selected model' : preset.description}
              >
                <span className="preset-rail-card__icon">{isActive ? <Icon name="check" size={13} /> : <PresetIcon preset={preset} />}</span>
                <span className="preset-rail-card__text">
                  <strong>{preset.name}</strong>
                  <span className="preset-rail-card__params preset-param-lines">{(focusedModelInfo ? effectivePresetParamPreviewLines(preset, focusedModelInfo, contextSizeForDisplay(focusedModelInfo, undefined, serverDefaultCtxSize)) : presetParamPreviewLines(preset, undefined, serverDefaultCtxSize)).map(line => <span key={line}>{line}</span>)}</span>
                </span>
              </button>
            );
          })}
        </div>
        {presetNotice && <div className="context-rail__notice">{presetNotice}</div>}
      </div>
    </aside>
  );


  /* ── Keyboard shortcut ───────────────────────────────────── */
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
        e.preventDefault();
        searchRef.current?.focus();
      }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, []);

  /* ── Stats ───────────────────────────────────────────────── */
  const showHuggingFaceZone = filterTab !== 'omni';
  const hasHuggingFaceActivity = showHuggingFaceZone
    && searchQuery.trim().length >= 2
    && (hfLoading || Boolean(hfError) || filteredHfResults.length > 0);
  const showManagerEmpty = !modelsLoading
    && filteredRunning.length === 0
    && filteredDownloaded.length === 0
    && filteredAvailable.length === 0
    && !hasHuggingFaceActivity;
  const isCustomOmniCollectionDraft = customDraft.capability === 'omni' && customDraft.omniSource === 'collection';
  const customFormTitle = isCustomOmniCollectionDraft ? 'Custom Omni collection' : 'Custom model';
  const customRecipeOptions = recipeOptionsForCustomDraft(customDraft.capability, customDraft.omniSource);
  const selectedCustomRecipe = customRecipeOptions.find(option => option.value === customDraft.recipe) || customRecipeOptions[0];
  const totalDownloaded = downloaded.length + displayLoadedModels.length;
  const totalPulling = Object.keys(pulling).length;
  const updateOmniComponent = (role: OmniComponentRole, value: string) => {
    if (role === 'llm') {
      const selectedInfo = allModels.find(m => modelName(m) === value);
      const plannerHasVision = !!selectedInfo && isOmniComponentEligible(selectedInfo, 'vision');
      const visionWasPlannerDefault = !customDraft.visionComponent || customDraft.visionComponent === customDraft.llmComponent;
      handleCustomDraftChange({
        llmComponent: value,
        ...(plannerHasVision && visionWasPlannerDefault ? { visionComponent: value } : {}),
        ...(!plannerHasVision && customDraft.visionComponent === customDraft.llmComponent ? { visionComponent: '' } : {}),
      });
      return;
    }
    if (role === 'image') {
      const selectedInfo = allModels.find(m => modelName(m) === value);
      const imageHasEdit = !!selectedInfo && isOmniComponentEligible(selectedInfo, 'edit');
      const editWasImageDefault = !customDraft.editComponent || customDraft.editComponent === customDraft.imageComponent;
      handleCustomDraftChange({
        imageComponent: value,
        ...(imageHasEdit && editWasImageDefault ? { editComponent: value } : {}),
        ...(!imageHasEdit && customDraft.editComponent === customDraft.imageComponent ? { editComponent: '' } : {}),
      });
      return;
    }
    handleCustomDraftChange({ [`${role}Component`]: value } as Partial<CustomModelDraftState>);
  };
  const searchHuggingFaceFromPicker = (query: string) => {
    setFilterTab('all');
    setSearchQuery(query);
    setShowAllAvailable(true);
  };


  const shouldPinHuggingFaceZone = showHuggingFaceZone && searchQuery.trim().length >= 2;
  const renderHuggingFaceZone = () => !showHuggingFaceZone ? null : (
    <section className="zone zone--hf zone--hf-compact">
      <div className="zone__head">
        <span className="zone__dot zone__dot--hf" />
        <span className="zone__title">HuggingFace</span>
        {!hfLoading && hfResults.length > 0 && <span className="zone__count">{filteredHfResults.length}</span>}
        <span className="zone__rule" />
      </div>
      {hfLoading ? (
        <div className="hf-zone__loading">
          <span className="hf-zone__spinner" />
          <span>Searching HuggingFace…</span>
        </div>
      ) : hfError ? (
        <div className="hf-zone__empty hf-zone__empty--error">
          <Icon name="alert" size={16} />
          <span>HuggingFace search is unavailable: {hfError}</span>
        </div>
      ) : searchQuery.trim().length >= 2 && filteredHfResults.length > 0 ? (
        filteredHfResults.map(r => renderHfRow(r))
      ) : (
        <div className="hf-zone__empty">
          <Icon name="download" size={16} />
          <span>{searchQuery.trim().length < 2 ? 'Type at least 2 characters to search HuggingFace' : 'No HuggingFace results for this query'}</span>
        </div>
      )}
    </section>
  );
  return (
    <div className={`manager manager--with-rail${presetRailCollapsed ? ' context-rail-collapsed' : ''}`}>
      {renderPresetRail()}
      <div className="manager__main">
      <div className="manager__head">
        <div className="manager__title">
          <h1>Models</h1>
          <div className="manager__stats">
            <span className="manager__stat">
              <span className="manager__stat-num">{displayLoadedModels.length}</span> running
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{totalDownloaded}</span> downloaded
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{allModels.length}</span> available
            </span>
            {totalPulling > 0 && (
              <>
                <span className="manager__stat-sep">·</span>
                <span className="manager__stat manager__stat--active">
                  <span className="manager__stat-num">{totalPulling}</span> downloading
                </span>
              </>
            )}
          </div>
        </div>

        {/* Search & filter bar */}
        <div className="manager__toolbar">
          <div className="manager__search">
            <span className="manager__search-icon">⌕</span>
            <input
              ref={searchRef}
              className="manager__search-input"
              type="text"
              placeholder="Search models… (Ctrl+K)"
              value={searchQuery}
              onChange={e => setSearchQuery(e.target.value)}
            />
            {searchQuery && (
              <button className="manager__search-clear" onClick={() => setSearchQuery('')}>×</button>
            )}
          </div>
          <div className="manager__custom-actions">
            <button className="btn btn--ghost manager__custom-btn" onClick={() => (showCustomForm && !isCustomOmniCollectionDraft) ? closeCustomForm() : openCustomForm('model')}>+ Custom model</button>
            <button className="btn btn--ghost manager__custom-btn manager__custom-btn--omni" onClick={() => openCustomForm('omni-collection')}>+ Omni collection</button>
          </div>
          <div className="manager__filters">
            {FILTER_TABS.map(tab => (
              <button
                key={tab.key}
                className={`manager__filter${filterTab === tab.key ? ' manager__filter--active' : ''}`}
                onClick={() => setFilterTab(tab.key)}
              >
                <span className="manager__filter-icon"><CapabilityIcon capability={tab.icon} size={13} /></span>
                {tab.label}
              </button>
            ))}
          </div>
        </div>
      </div>

      <div className="manager__body">
        {showCustomForm && (
          <section className="zone custom-model-form" aria-label={`Add ${customFormTitle}`}>
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">{customFormTitle}</span>
              {isCustomOmniCollectionDraft && <span className="zone__count">collection wrapper</span>}
              <span className="zone__rule" />
            </div>
            <form className="custom-model-form__grid" onSubmit={handleSaveCustomModel}>
              <label>Name
                <input value={customDraft.displayName} onChange={e => handleCustomDraftChange({ displayName: e.target.value })} placeholder="My custom model" />
              </label>
              <label>Capability
                <select value={customDraft.capability} onChange={e => {
                  const nextCapability = e.target.value as CustomModelCapability;
                  handleCustomDraftChange({
                    capability: nextCapability,
                    omniSource: nextCapability === 'omni' ? customDraft.omniSource : 'single',
                    recipe: defaultRecipeForCapability(nextCapability, nextCapability === 'omni' ? customDraft.omniSource : 'single'),
                  });
                }}>
                  {CUSTOM_CAPABILITIES.map(c => <option key={c.value} value={c.value}>{c.label} — {c.hint}</option>)}
                </select>
              </label>
              {customDraft.capability === 'omni' && (
                <label>Omni type
                  <select value={customDraft.omniSource} onChange={e => {
                    const omniSource = e.target.value as 'single' | 'collection';
                    handleCustomDraftChange({ omniSource, recipe: defaultRecipeForCapability('omni', omniSource) });
                  }}>
                    <option value="single">Single multimodal checkpoint</option>
                    <option value="collection">Collection wrapper from existing components</option>
                  </select>
                </label>
              )}
              <label>Recipe/backend
                <select value={selectedCustomRecipe?.value || customDraft.recipe} onChange={e => handleCustomDraftChange({ recipe: e.target.value })}>
                  {customRecipeOptions.map(option => <option key={option.value} value={option.value}>{option.label}</option>)}
                </select>
                {selectedCustomRecipe?.hint && <span className="custom-model-form__field-hint">{selectedCustomRecipe.hint}</span>}
              </label>
              <label className="custom-model-form__wide">{isCustomOmniCollectionDraft ? 'Optional collection checkpoint/alias' : 'Checkpoint, HF repo, or local path'}
                <input
                  value={customDraft.checkpoint}
                  onChange={e => handleCustomDraftChange({ checkpoint: e.target.value })}
                  placeholder={isCustomOmniCollectionDraft ? 'Optional; first component is used when left empty' : 'org/model:Q4_K_M.gguf or /path/to/model.gguf'}
                />
              </label>
              {customDraft.capability === 'omni' && customDraft.omniSource === 'collection' && (
                <>
                  <div className="custom-model-form__hint custom-model-form__wide">
                    Build a visible Omni collection from downloaded, registered, or custom models. Pick components from the searchable dropdowns; use the HuggingFace zone to download/register new components before selecting them here.
                  </div>
                  <OmniComponentPicker role="llm" value={customDraft.llmComponent} options={omniComponentOptions.llm} onChange={value => updateOmniComponent('llm', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="vision" value={customDraft.visionComponent} options={omniComponentOptions.vision} onChange={value => updateOmniComponent('vision', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="image" value={customDraft.imageComponent} options={omniComponentOptions.image} onChange={value => updateOmniComponent('image', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="edit" value={customDraft.editComponent} options={omniComponentOptions.edit} onChange={value => updateOmniComponent('edit', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="transcription" value={customDraft.transcriptionComponent} options={omniComponentOptions.transcription} onChange={value => updateOmniComponent('transcription', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="speech" value={customDraft.speechComponent} options={omniComponentOptions.speech} onChange={value => updateOmniComponent('speech', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                </>
              )}
              {customDraft.capability !== 'omni' && (
                <label>Context tokens
                  <input value={customDraft.maxContextWindow} onChange={e => handleCustomDraftChange({ maxContextWindow: e.target.value })} inputMode="numeric" placeholder="4096" />
                </label>
              )}
              <label>Extra labels
                <input value={customDraft.labels} onChange={e => handleCustomDraftChange({ labels: e.target.value })} placeholder="tool-calling, reasoning" />
              </label>
              {customError && <div className="custom-model-form__error"><Icon name="alert" size={14} /> {customError}</div>}
              <div className="custom-model-form__actions">
                <button className="btn btn--primary" type="submit">Save {isCustomOmniCollectionDraft ? 'Omni collection' : 'custom model'}</button>
                <button className="btn btn--ghost" type="button" onClick={closeCustomForm}>Cancel</button>
              </div>
            </form>
          </section>
        )}

        {modelsLoading && (
          <section className="zone" aria-label="Loading models">
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">Loading models…</span>
              <span className="zone__rule" />
            </div>
            <div className="manager__loading">Refreshing Lemonade model registry…</div>
          </section>
        )}

        {shouldPinHuggingFaceZone && renderHuggingFaceZone()}

        {/* Running zone */}
        {filteredRunning.length > 0 && (
          <section className="zone zone--running">
            <div className="zone__head">
              <span className="zone__dot zone__dot--running" />
              <span className="zone__title">Loaded Models</span>
              <span className="zone__count">{filteredRunning.length}</span>
              <span className="zone__rule" />
            </div>
            {filteredRunning.map(m => renderRunningModel(m))}
          </section>
        )}

        {/* Downloaded / Ready zone */}
        {filteredDownloaded.length > 0 && (
          <section className="zone zone--downloaded">
            <div className="zone__head">
              <span className="zone__dot zone__dot--ready" />
              <span className="zone__title">Downloaded</span>
              <span className="zone__count">{filteredDownloaded.length}</span>
              <span className="zone__rule" />
            </div>
            {filteredDownloaded.map(m => renderModelRow(m, true))}
          </section>
        )}

        {/* Available zone */}
        {filteredAvailable.length > 0 && (
          <section className="zone">
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">Lemonade Registry</span>
              <span className="zone__count">{filteredAvailable.length}</span>
              <span className="zone__rule" />
            </div>
            {visibleAvailable.map(m => renderModelRow(m, false))}
            {hiddenAvailableCount > 0 && (
              <button
                className="zone__show-more"
                onClick={() => setShowAllAvailable(true)}
              >
                Show {hiddenAvailableCount} more models
              </button>
            )}
          </section>
        )}

        {!shouldPinHuggingFaceZone && renderHuggingFaceZone()}

        <div className={`manager__empty${showManagerEmpty ? '' : ' manager__empty--hidden'}`} aria-hidden={!showManagerEmpty}>
          <span className="manager__empty-icon">{api.isConnected ? <Icon name="box" size={42} /> : <Icon name="plug" size={42} />}</span>
          <p>{api.isConnected
            ? 'No models found matching your search.'
            : 'Connect to a Lemonade server to see models.'
          }</p>
        </div>
      </div>
      </div>
    </div>
  );
};

export default ModelManager;
