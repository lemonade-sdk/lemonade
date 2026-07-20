import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { ModelInfo, LoadedModel, PullCallbacks, PullVariantsResult, HFModelResult, ModelRegistryProvider, searchHuggingFace, searchModelScope, friendlyErrorMessage } from '../api';
import { copyTextToClipboard } from '../clipboard';
import { canSelectInComposer, capabilityFromLoaded, capabilityFromModelInfo, capabilityIcon, capabilityLabel, modelMatchesCapabilityTags, ModelCapability } from '../modelCapabilities';
import { CapabilityIcon, Icon, PresetIcon } from './Icon';
import { scopedStorageKey, type AccountSession } from '../features/accounts/accountStore';
import { CUSTOM_CAPABILITIES, CustomModelCapability, CustomOmniToolDefinition, customLoadOptions, customModelToModelInfo, customRegistrationOptions, deleteCustomModel, exportCustomModelsPayload, importCustomModels, loadCustomModels, upsertCustomModel, type CustomOmniToolTargetType } from '../features/customModels/customModelStore';
import { collectionComponentLabel, getCollectionComponents, isCollectionModel, isCollectionFullyDownloaded, withVirtualLoadedCollections } from '../features/collections/collectionModels';
import { DEFAULT_CONTEXT_SIZE, DEFAULT_PRESET, PRESET_STORE_EVENT, Preset, STARTERS, effectivePresetParamPreviewLines, isCompatible, loadApplied, loadUserPresets, modelContextSize, presetHasApplicablePreviewOverrides, presetParamPreviewLines, saveApplied } from '../presetStore';
import { DownloadListItem, activeDownloadForModel, downloadStore } from '../features/downloadManager/downloadStore';
import { TTS_SETTINGS_EVENT, TtsPlaybackMode, loadTtsPlaybackSettings, saveActiveTtsModel, saveSpeakUserText, saveTtsPlaybackMode } from '../features/audio/ttsSettings';
import { ModelListPanel, modelIsCustom, modelMatchesBackend, modelMatchesFilter, modelMatchesTag } from './ModelListPanel';
import type { PrimaryFilter } from './ModelListPanel';
import { ModelNavRail } from './ModelNavRail';
import { ModelDetailPanel } from './ModelDetailPanel';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';
import { DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE } from '../tools/omniTools';
import { remoteResultAsModelInfo } from '../remoteModelCapabilities';
import RouterEditorPanel from './RouterEditorPanel';
import GlobalModelSettingsPanel, { type UpdateAllModelsResult } from './GlobalModelSettingsPanel';
import { ROUTER_RECIPE, type RouterPullRequest } from '../features/router/routerTypes';
import { deleteRouterRecord, loadRouterRecords, routerRecordToModelInfo } from '../features/router/routerStore';
import {
  GLOBAL_MODEL_SETTINGS_EVENT,
  automaticUpdateIsDue,
  loadGlobalModelSettings,
  loadPinnedModelNames,
  loadWithGlobalModelPolicy,
  saveGlobalModelSettings,
  savePinnedModelNames,
  type GlobalModelSettings,
} from '../features/modelSettings/globalModelSettings';

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
    case 'acestep': return 'ACE-Step';
    case 'thinksound': return 'ThinkSound';
    case 'openmoss': return 'OpenMOSS';
    case 'trellis': return 'TRELLIS';
    case 'collection.omni': return 'Omni';
    case 'collection.router': return 'Router';
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
    case 'acestep': return '#fb7185';
    case 'thinksound': return '#2dd4bf';
    case 'openmoss': return '#f9a8d4';
    case 'trellis': return '#818cf8';
    case 'collection.omni': return '#a78bfa';
    case 'collection.router': return '#22d3ee';
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
    case 'acestep': return 'ACE-Step';
    case 'thinksound': return 'ThinkSound';
    case 'openmoss': return 'OpenMOSS TTS';
    case 'trellis': return 'TRELLIS.2';
    case 'collection.omni': return 'Omni Collection';
    case 'collection.router': return 'Router';
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
    'audio-generation': 'Audio generation',
    'music-generation': 'Music generation',
    'sound-generation': 'Sound generation',
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
    '3d': '3D',
    '3d-generation': '3D',
    'image-to-3d': 'Image to 3D',
    'model3d': '3D',
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
  if (['audio-generation', 'music-generation', 'sound-generation', 'sfx'].includes(key)) return 'audio-generation';
  if (['audio', 'transcription', 'realtime-transcription', 'chat-transcription', 'asr', 'stt', 'speech-to-text'].includes(key)) return 'transcription';
  if (['tts', 'speech', 'text-to-speech', 'voice-design'].includes(key)) return 'tts';
  if (['3d', '3d-generation', 'image-to-3d', 'model3d'].includes(key)) return 'model3d';
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
    case 'audio-generation': return 'audio-generation';
    case 'tts': return 'tts';
    case 'model3d': return '3d';
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
  acestep: 'ACE-Step',
  thinksound: 'ThinkSound',
  openmoss: 'OpenMOSS',
  trellis: 'TRELLIS.2',
  'ryzenai-llm': 'RyzenAI',
  'collection.omni': 'Omni Collection',
  'collection.router': 'Router',
};

type CustomRecipeOption = { value: string; recipe: string; backend?: string; label: string; hint: string };
type CustomCheckpointExample = { key: string; label: string; checkpoint: string; note: string };

function supportsVisionProjectorField(capability: CustomModelCapability): boolean {
  return capability === 'chat' || capability === 'omni';
}
type CustomRecipeSuggestion = { checkpoint: string; note: string; extraCheckpoints?: CustomCheckpointExample[] };

const CUSTOM_LLM_RECIPE_ORDER = ['llamacpp', 'flm', 'ryzenai-llm', 'vllm'];

const CUSTOM_RECIPE_SUGGESTIONS: Record<string, CustomRecipeSuggestion> = {
  llamacpp: {
    checkpoint: 'unsloth/Qwen3-8B-GGUF:Q4_K_M',
    note: 'GGUF repo plus quantization suffix; Lemonade can download the selected quantization.',
    extraCheckpoints: [
      {
        key: 'mmproj',
        label: 'Vision projector (mmproj)',
        checkpoint: 'ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf',
        note: 'Optional for llama.cpp vision models; leave empty for text-only GGUFs.',
      },
    ],
  },
  vllm: {
    checkpoint: 'Qwen/Qwen3-4B',
    note: 'Upstream Hugging Face transformer checkpoint; vLLM loads the repo directly.',
  },
  flm: {
    checkpoint: 'qwen3-0.6b-FLM',
    note: 'FastFlowLM model id or local FastFlowLM artifact path for supported AMD NPU models.',
  },
  'ryzenai-llm': {
    checkpoint: 'amd/Llama-3.2-1B-Instruct-onnx-ryzenai-1.7-hybrid',
    note: 'RyzenAI-compatible ONNX/HF checkpoint or local path.',
  },
  'sd-cpp': {
    checkpoint: 'unsloth/FLUX.2-klein-9B-GGUF:flux-2-klein-9b-Q8_0.gguf',
    note: 'Primary diffusion checkpoint. Extra fields below cover multi-file image models.',
    extraCheckpoints: [
      {
        key: 'text_encoder',
        label: 'Text encoder checkpoint',
        checkpoint: 'unsloth/Qwen3-8B-GGUF:Qwen3-8B-Q8_0.gguf',
        note: 'Optional text encoder checkpoint for Flux/Qwen-style image pipelines.',
      },
      {
        key: 'vae',
        label: 'VAE checkpoint',
        checkpoint: 'Comfy-Org/vae-text-encorder-for-flux-klein-9b:split_files/vae/flux2-vae.safetensors',
        note: 'Optional VAE checkpoint for image models that split the VAE from the main model.',
      },
    ],
  },
  whispercpp: {
    checkpoint: 'ggerganov/whisper.cpp:ggml-base.bin',
    note: 'Whisper.cpp checkpoint repo plus .bin model file.',
  },
  moonshine: {
    checkpoint: 'UsefulSensors/moonshine-streaming:onnx/tiny',
    note: 'Moonshine streaming checkpoint repo plus ONNX variant path.',
  },
  kokoro: {
    checkpoint: 'mikkoph/kokoro-onnx',
    note: 'Kokoro ONNX model repository or local model path.',
  },
  acestep: {
    checkpoint: 'Serveurperso/ACE-Step-1.5-GGUF:acestep-v15-xl-sft-Q8_0.gguf',
    note: 'ACE-Step checkpoint for music generation.',
  },
  thinksound: {
    checkpoint: 'ilintar/thinksound-gguf',
    note: 'ThinkSound checkpoint for prompt-driven sound effects.',
  },
  openmoss: {
    checkpoint: 'ilintar/moss-tts-gguf:moss-tts-1.5-q8_0.gguf',
    note: 'OpenMOSS checkpoint for speech synthesis or voice design.',
  },
  trellis: {
    checkpoint: 'ilintar/trellis2-gguf',
    note: 'TRELLIS.2 checkpoint for image-to-3D reconstruction.',
  },
};

const InlineCheckpointExample: React.FC<{ checkpoint: string; note?: string }> = ({ checkpoint, note }) => (
  <span className="custom-model-form__inline-example" title={note || checkpoint}>
    Example: <code>{checkpoint}</code>
  </span>
);

function optionValue(recipe: string, backend?: string): string {
  return backend ? `${recipe}:${backend}` : recipe;
}

function optionRecipe(value: string): string {
  return String(value || '').split(':')[0] || value;
}

function customRecipeOption(recipe: string, label: string, hint: string, backend?: string): CustomRecipeOption {
  return { value: optionValue(recipe, backend), recipe, backend, label, hint };
}

const CHAT_RECIPE_OPTIONS: CustomRecipeOption[] = [
  customRecipeOption('llamacpp', 'llama.cpp', 'Local GGUF / llama.cpp backend'),
  customRecipeOption('flm', 'FastFlowLM', 'FastFlowLM backend for supported AMD NPU models'),
  customRecipeOption('ryzenai-llm', 'RyzenAI', 'RyzenAI LLM backend for compatible ONNX/quantized models'),
  customRecipeOption('vllm', 'vLLM', 'vLLM backend for compatible HF transformer checkpoints'),
];

const CUSTOM_RECIPE_OPTIONS: Record<CustomModelCapability, CustomRecipeOption[]> = {
  chat: CHAT_RECIPE_OPTIONS,
  omni: CHAT_RECIPE_OPTIONS,
  image: [customRecipeOption('sd-cpp', 'Stable Diffusion', 'Stable Diffusion C++ backend')],
  audio: [
    customRecipeOption('whispercpp', 'Whisper', 'Whisper C++ transcription backend'),
    customRecipeOption('moonshine', 'Moonshine', 'CPU streaming speech-to-text backend'),
  ],
  'audio-generation': [
    customRecipeOption('acestep', 'ACE-Step', 'Music generation backend'),
    customRecipeOption('thinksound', 'ThinkSound', 'Sound-effect generation backend'),
  ],
  tts: [
    customRecipeOption('openmoss', 'OpenMOSS TTS', 'OpenMOSS speech and voice-design backend'),
    customRecipeOption('kokoro', 'Kokoro TTS', 'Kokoro text-to-speech backend'),
  ],
  model3d: [customRecipeOption('trellis', 'TRELLIS.2', 'Image-to-3D reconstruction backend')],
  embedding: [customRecipeOption('llamacpp', 'llama.cpp', 'Embedding through llama.cpp-compatible model')],
  reranking: [customRecipeOption('llamacpp', 'llama.cpp', 'Reranking through llama.cpp-compatible model')],
};

function recipeOptionsForCustomDraft(capability: CustomModelCapability, omniSource: 'single' | 'collection'): CustomRecipeOption[] {
  if (capability === 'omni' && omniSource === 'collection') {
    return [customRecipeOption('collection.omni', 'Omni Collection', 'Virtual wrapper around selected component models')];
  }
  return CUSTOM_RECIPE_OPTIONS[capability] || CHAT_RECIPE_OPTIONS;
}

function dedupeRecipeOptions(options: CustomRecipeOption[]): CustomRecipeOption[] {
  const seen = new Set<string>();
  const out: CustomRecipeOption[] = [];
  for (const option of options) {
    const key = option.value.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(option);
  }
  return out;
}

const CUSTOM_RECIPE_CAPABILITIES: Record<string, CustomModelCapability[]> = {
  llamacpp: ['chat', 'omni', 'embedding', 'reranking'],
  vllm: ['chat', 'omni'],
  flm: ['chat', 'omni'],
  'ryzenai-llm': ['chat', 'omni'],
  'sd-cpp': ['image'],
  whispercpp: ['audio'],
  moonshine: ['audio'],
  kokoro: ['tts'],
  openmoss: ['tts'],
  acestep: ['audio-generation'],
  thinksound: ['audio-generation'],
  trellis: ['model3d'],
};

function recipeCapabilities(recipe: string, backendNames: string[]): CustomModelCapability[] {
  const explicit = CUSTOM_RECIPE_CAPABILITIES[recipe];
  if (explicit) return explicit;
  const key = recipe.toLowerCase();
  if (key.includes('trellis') || key.includes('3d')) return ['model3d'];
  if (key.includes('acestep') || key.includes('ace-step') || key.includes('thinksound') || key.includes('sound-generation') || key.includes('music-generation')) return ['audio-generation'];
  if (key.includes('sd') || key.includes('diffusion') || key.includes('image')) return ['image'];
  if (key.includes('whisper') || key.includes('moonshine') || key.includes('transcrib') || key.includes('speech-to-text')) return ['audio'];
  if (key.includes('kokoro') || key.includes('openmoss') || key.includes('tts') || key.includes('text-to-speech')) return ['tts'];
  if (key.includes('embed')) return ['embedding'];
  if (key.includes('rerank')) return ['reranking'];
  const backendText = backendNames.join(' ').toLowerCase();
  if (backendText.includes('trellis') || backendText.includes('3d')) return ['model3d'];
  if (backendText.includes('acestep') || backendText.includes('thinksound')) return ['audio-generation'];
  if (backendText.includes('sd') || backendText.includes('diffusion')) return ['image'];
  if (backendText.includes('whisper') || backendText.includes('moonshine')) return ['audio'];
  if (backendText.includes('kokoro') || backendText.includes('openmoss') || backendText.includes('tts')) return ['tts'];
  return ['chat', 'omni'];
}

function backendState(value: unknown): string {
  return String((value as any)?.state || '').trim().toLowerCase();
}

function backendIsExplicitlyUnsupported(info: unknown): boolean {
  return backendState(info) === 'unsupported';
}

function systemRecipeEntries(info: Record<string, unknown> | null): Array<[string, Record<string, any>]> | null {
  if (!info || typeof info !== 'object') return null;
  const recipes = (info as any).recipes;
  if (!recipes || typeof recipes !== 'object' || Array.isArray(recipes)) return null;
  return Object.entries(recipes as Record<string, any>)
    .filter(([, raw]) => raw && typeof raw === 'object') as Array<[string, Record<string, any>]>;
}

function rawBackendMap(raw: Record<string, any>): Record<string, unknown> {
  return raw.backends && typeof raw.backends === 'object' && !Array.isArray(raw.backends)
    ? raw.backends as Record<string, unknown>
    : {};
}

function visibleBackendEntries(raw: Record<string, any>): Array<[string, unknown]> {
  // This is intentionally the same core rule used by the Backends matrix:
  // start from /system-info.recipes[*].backends and remove only explicit
  // unsupported entries for this custom-model selector.
  return Object.entries(rawBackendMap(raw)).filter(([, info]) => !backendIsExplicitlyUnsupported(info));
}

function optionSortRank(option: CustomRecipeOption): number {
  const recipeRank = CUSTOM_LLM_RECIPE_ORDER.indexOf(option.recipe);
  return recipeRank === -1 ? 1000 : recipeRank * 100;
}

function summarizeRecipeAvailability(backends: Array<[string, unknown]>): string {
  const stateLabels = Array.from(new Set(
    backends
      .map(([, info]) => backendState(info))
      .filter(Boolean)
      .map(state => state.replace(/_/g, ' '))
  ));
  if (stateLabels.length === 0) return 'Available on this Lemonade server.';
  if (stateLabels.length === 1) return `Available on this Lemonade server (${stateLabels[0]}).`;
  return `Available on this Lemonade server (${stateLabels.join(', ')}).`;
}

function recipeOptionsFromSystemInfo(info: Record<string, unknown> | null): Partial<Record<CustomModelCapability, CustomRecipeOption[]>> {
  const entries = systemRecipeEntries(info);
  if (!entries) return {};
  const result: Partial<Record<CustomModelCapability, CustomRecipeOption[]>> = {};

  for (const [recipe, raw] of entries) {
    const visibleBackends = visibleBackendEntries(raw);
    const backends = visibleBackends.length
      ? visibleBackends
      : Object.keys(rawBackendMap(raw)).length === 0
        ? [['default', {}] as [string, unknown]]
        : [];
    if (!backends.length) continue;

    const backendNames = backends.map(([name]) => name);
    const capabilities = recipeCapabilities(recipe, backendNames);
    const option = customRecipeOption(
      recipe,
      recipeLabel(recipe),
      summarizeRecipeAvailability(backends),
    );

    for (const capability of capabilities) {
      result[capability] = dedupeRecipeOptions([...(result[capability] || []), option]);
    }
  }

  for (const [capability, options] of Object.entries(result) as Array<[CustomModelCapability, CustomRecipeOption[]]>) {
    result[capability] = [...options].sort((a, b) => optionSortRank(a) - optionSortRank(b) || a.label.localeCompare(b.label));
  }

  return result;
}

// Favorites are a DISTINCT client-local concept from Pinned (fl0rianr #2424).
// Pinned models float to the top of the middle list; favorites is a separate
// filter/count surfaced by the left-rail "Favorites" (star) entry. Stored under
// a separate `favorite_models` key so the two never alias.
function favoriteModelsKey(scope: string): string {
  return scopedStorageKey(scope, 'favorite_models');
}

function loadFavoriteModels(scope: string): string[] {
  try {
    const raw = localStorage.getItem(favoriteModelsKey(scope));
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed.map(v => String(v).trim()).filter(Boolean) : [];
  } catch { return []; }
}

function saveFavoriteModels(scope: string, names: string[]): void {
  try {
    localStorage.setItem(favoriteModelsKey(scope), JSON.stringify(Array.from(new Set(names.filter(Boolean)))));
  } catch {}
}

/* ── Filter / search types ─────────────────────────────────── */

type FilterTab = 'all' | 'llm' | 'omni' | 'image' | 'audio' | 'audio-generation' | 'tts' | 'model3d' | 'embedding';
type ProviderEnabledState = Record<ModelRegistryProvider, boolean>;

const REMOTE_SEARCH_CACHE = new Map<string, HFModelResult[]>();
const REMOTE_VARIANT_CACHE = new Map<string, PullVariantsResult | null>();
const MODELSCOPE_RESULT_LIMIT = 10;
const REMOTE_VARIANT_CONCURRENCY = 4;

const providerKey = (provider: ModelRegistryProvider, modelId: string): string => `${provider}:${modelId}`;
const providerSearchCacheKey = (provider: ModelRegistryProvider, query: string): string => `${provider}:${query.trim().toLowerCase()}`;

const PROVIDER_META: Record<ModelRegistryProvider, { label: string; compactLabel: string; url: (id: string) => string }> = {
  huggingface: {
    label: 'Hugging Face',
    compactLabel: 'HuggingFace',
    url: id => `https://huggingface.co/${id}`,
  },
  modelscope: {
    label: 'ModelScope',
    compactLabel: 'ModelScope',
    url: id => `https://modelscope.cn/models/${id}`,
  },
};

function remoteVariantCheckpoint(modelId: string, variantName: string, recipe: string): string {
  return String(recipe || '').toLowerCase() === 'llamacpp'
    ? `${modelId}:${variantName}`
    : modelId;
}

function remoteDefaultModelName(
  provider: ModelRegistryProvider,
  modelId: string,
  variants?: PullVariantsResult,
  variantName = '',
  recipe = variants?.recipe || '',
): string {
  const suggested = variants?.suggested_name || modelId.split('/').pop() || modelId;
  const providerScoped = provider === 'modelscope' ? `${suggested}-modelscope` : suggested;
  if (String(recipe || '').toLowerCase() !== 'llamacpp' || !variantName) return providerScoped;
  return `${providerScoped}-${variantName}`;
}

async function loadRemoteVariants(
  provider: ModelRegistryProvider,
  modelId: string,
  signal?: AbortSignal,
): Promise<PullVariantsResult | null> {
  const key = providerKey(provider, modelId);
  if (REMOTE_VARIANT_CACHE.has(key)) return REMOTE_VARIANT_CACHE.get(key) ?? null;
  try {
    const result = await api.pullVariants(modelId, provider, signal);
    REMOTE_VARIANT_CACHE.set(key, result);
    return result;
  } catch (error) {
    throw error;
  }
}
type CustomFormMode = 'model' | 'omni-collection';
type OmniComponentRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';
type OmniCustomToolPreset = 'generic' | 'coder' | 'reviewer' | 'vision' | 'image';
type OmniCustomToolDraft = {
  id: string;
  name: string;
  description: string;
  targetModel: string;
  targetType: CustomOmniToolTargetType;
  systemPrompt: string;
  promptTemplate: string;
  parametersJson: string;
  maxTokens: string;
};
type CustomModelDraftState = {
  name: string;
  displayName: string;
  checkpoint: string;
  mmproj: string;
  imageTextEncoder: string;
  imageVae: string;
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
  omniSystemPrompt: string;
  omniCustomTools: OmniCustomToolDraft[];
};


const DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS = {
  type: 'object',
  properties: {
    task: { type: 'string', description: 'The focused task to delegate to the target model.' },
    context: { type: 'string', description: 'Optional context, constraints, code, or review material for the target model.' },
  },
  required: ['task'],
  additionalProperties: false,
};

const DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON = JSON.stringify(DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS, null, 2);

const DEFAULT_CUSTOM_VISION_TOOL_PARAMETERS_JSON = JSON.stringify({
  type: 'object',
  properties: {
    question: { type: 'string', description: 'What the vision model should determine from the latest image.' },
    context: { type: 'string', description: 'Optional context or constraints for the analysis.' },
  },
  required: ['question'],
  additionalProperties: false,
}, null, 2);

const DEFAULT_CUSTOM_IMAGE_TOOL_PARAMETERS_JSON = JSON.stringify({
  type: 'object',
  properties: {
    prompt: { type: 'string', description: 'A detailed description of the image to generate.' },
    size: { type: 'string', description: 'Optional WIDTHxHEIGHT canvas size.' },
    steps: { type: 'integer', minimum: 1, maximum: 100 },
    cfg_scale: { type: 'number', minimum: 0 },
    seed: { type: 'integer' },
  },
  required: ['prompt'],
  additionalProperties: false,
}, null, 2);

function targetTypeForOmniToolPreset(preset: OmniCustomToolPreset): CustomOmniToolTargetType {
  if (preset === 'vision' || preset === 'image') return preset;
  return 'chat';
}

function defaultOmniToolParametersJson(targetType: CustomOmniToolTargetType): string {
  switch (targetType) {
    case 'vision': return DEFAULT_CUSTOM_VISION_TOOL_PARAMETERS_JSON;
    case 'image': return DEFAULT_CUSTOM_IMAGE_TOOL_PARAMETERS_JSON;
    default: return DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON;
  }
}

function sanitizeOmniToolName(value: string): string {
  return value
    .trim()
    .replace(/\s+/g, '_')
    .replace(/[^a-zA-Z0-9_-]/g, '_')
    .replace(/_+/g, '_')
    .replace(/^([^a-zA-Z_])/, '_$1')
    .slice(0, 64);
}

function nextOmniToolName(existing: OmniCustomToolDraft[], base: string): string {
  const used = new Set(existing.map(tool => tool.name.trim().toLowerCase()).filter(Boolean));
  let candidate = sanitizeOmniToolName(base) || 'ask_model';
  if (!used.has(candidate.toLowerCase())) return candidate;
  let i = 2;
  while (used.has(`${candidate}_${i}`.toLowerCase())) i += 1;
  return `${candidate}_${i}`;
}

function createOmniCustomToolDraft(existing: OmniCustomToolDraft[] = [], preset: OmniCustomToolPreset = 'generic', targetModel = ''): OmniCustomToolDraft {
  const id = `custom-tool-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
  const targetType = targetTypeForOmniToolPreset(preset);
  if (preset === 'coder') {
    const name = nextOmniToolName(existing, 'ask_coder');
    return {
      id,
      name,
      description: 'Delegate a focused implementation task to a coding model.',
      targetModel,
      targetType,
      systemPrompt: 'You are a focused coding assistant. Implement small, well-scoped tasks. Prefer concrete code and mention important assumptions or edge cases briefly.',
      promptTemplate: 'Task:\n{task}\n\nContext:\n{context}\n\nReturn the implementation guidance or code the planner should use.',
      parametersJson: DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON,
      maxTokens: '',
    };
  }
  if (preset === 'reviewer') {
    const name = nextOmniToolName(existing, 'ask_reviewer');
    return {
      id,
      name,
      description: 'Ask a reviewer model to check code, plans, or patches for bugs and regressions.',
      targetModel,
      targetType,
      systemPrompt: 'You are a strict but practical code reviewer. Look for correctness bugs, regressions, missing tests, and risky assumptions. Be concise and actionable.',
      promptTemplate: 'Review task:\n{task}\n\nMaterial to review:\n{context}\n\nReturn findings grouped by severity, plus a short verdict.',
      parametersJson: DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON,
      maxTokens: '',
    };
  }
  if (preset === 'vision') {
    return {
      id,
      name: nextOmniToolName(existing, 'inspect_image'),
      description: 'Ask a selected vision model to inspect the latest attached or generated image.',
      targetModel,
      targetType,
      systemPrompt: 'You are a precise vision specialist. Analyze only what is visible and clearly separate observations from uncertainty.',
      promptTemplate: 'Question:\n{question}\n\nAdditional context:\n{context}',
      parametersJson: DEFAULT_CUSTOM_VISION_TOOL_PARAMETERS_JSON,
      maxTokens: '',
    };
  }
  if (preset === 'image') {
    return {
      id,
      name: nextOmniToolName(existing, 'render_image'),
      description: 'Generate an image with a specifically selected image model.',
      targetModel,
      targetType,
      systemPrompt: '',
      promptTemplate: '',
      parametersJson: DEFAULT_CUSTOM_IMAGE_TOOL_PARAMETERS_JSON,
      maxTokens: '',
    };
  }
  const name = nextOmniToolName(existing, 'ask_model');
  return {
    id,
    name,
    description: 'Delegate a focused task to another local chat model and return its result to the planner.',
    targetModel,
    targetType,
    systemPrompt: 'You are a focused internal assistant tool. Complete the delegated task and return concise, actionable results.',
    promptTemplate: 'Task:\n{task}\n\nContext:\n{context}\n\nReturn a concise result for the planner model.',
    parametersJson: DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON,
    maxTokens: '',
  };
}

const FILTER_TABS: { key: FilterTab; label: string; icon: ModelCapability | 'all' }[] = [
  { key: 'all', label: 'All', icon: 'all' },
  { key: 'llm', label: 'LLM', icon: 'chat' },
  { key: 'omni', label: 'Omni', icon: 'omni' },
  { key: 'image', label: 'Image', icon: 'image' },
  { key: 'audio', label: 'Audio', icon: 'audio' },
  { key: 'audio-generation', label: 'Music & SFX', icon: 'audio-generation' },
  { key: 'tts', label: 'TTS', icon: 'tts' },
  { key: 'model3d', label: '3D', icon: 'model3d' },
  { key: 'embedding', label: 'Embed', icon: 'embedding' },
];

function createEmptyCustomDraft(mode: CustomFormMode = 'model'): CustomModelDraftState {
  const isOmniCollection = mode === 'omni-collection';
  return {
    name: '',
    displayName: '',
    checkpoint: '',
    mmproj: '',
    imageTextEncoder: '',
    imageVae: '',
    recipe: isOmniCollection ? 'collection.omni' : 'llamacpp',
    capability: isOmniCollection ? 'omni' : 'chat',
    maxContextWindow: '',
    labels: '',
    omniSource: isOmniCollection ? 'collection' : 'single',
    llmComponent: '',
    visionComponent: '',
    imageComponent: '',
    editComponent: '',
    transcriptionComponent: '',
    speechComponent: '',
    omniSystemPrompt: DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE,
    omniCustomTools: [],
  };
}

function customDraftFromModel(model: ModelInfo): CustomModelDraftState {
  const checkpoints = objectRecord((model as any).checkpoints);
  const roles = objectRecord((model as any).component_roles);
  const rawTools = Array.isArray((model as any).custom_tools) ? (model as any).custom_tools as CustomOmniToolDefinition[] : [];
  const components = getCollectionComponents(model);
  const collection = isCollectionModel(model);
  const capability = (collection ? 'omni' : String((model as any).type || 'chat')) as CustomModelCapability;
  const structuralLabels = new Set(['custom', 'omni', 'multimodal', 'vision-language']);
  return {
    name: modelName(model),
    displayName: String(model.display_name || modelName(model)),
    checkpoint: String((model as any).checkpoint || checkpoints.main || ''),
    mmproj: String((model as any).mmproj || checkpoints.mmproj || ''),
    imageTextEncoder: String(checkpoints.text_encoder || ''),
    imageVae: String(checkpoints.vae || ''),
    recipe: String((model as any).recipe || (collection ? 'collection.omni' : 'llamacpp')),
    capability,
    maxContextWindow: String((model as any).max_context_window || ''),
    labels: modelLabels(model).filter(label => !structuralLabels.has(label.toLowerCase())).join(', '),
    omniSource: collection ? 'collection' : 'single',
    llmComponent: String(roles.llm || components[0] || ''),
    visionComponent: String(roles.vision || ''),
    imageComponent: String(roles.image || ''),
    editComponent: String(roles.edit || ''),
    transcriptionComponent: String(roles.transcription || ''),
    speechComponent: String(roles.speech || ''),
    omniSystemPrompt: String((model as any).system_prompt || DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE),
    omniCustomTools: rawTools.map((tool, index) => ({
      id: String(tool.id || `custom-tool-${index + 1}`),
      name: String(tool.name || ''),
      description: String(tool.description || ''),
      targetModel: String(tool.target_model || ''),
      targetType: (['vision', 'image'].includes(String(tool.target_type || '')) ? String(tool.target_type) : 'chat') as CustomOmniToolTargetType,
      systemPrompt: String(tool.system_prompt || ''),
      promptTemplate: String(tool.prompt_template || ''),
      parametersJson: JSON.stringify(tool.parameters || DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS, null, 2),
      maxTokens: tool.max_tokens ? String(tool.max_tokens) : '',
    })),
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

const NON_PLANNER_LABELS = new Set(['image', 'image-generation', 'edit', 'upscaling', 'speech', 'tts', 'text-to-speech', 'transcription', 'audio-generation', 'music', 'music-generation', 'sound-generation', 'sfx', '3d', 'model3d', '3d-generation', 'image-to-3d', 'mesh', 'mesh-generation', 'embeddings', 'embedding', 'reranking', 'reranker']);

const MODEL_LIST_WIDTH_KEY = 'model_list_panel_width';
const MODEL_LIST_DEFAULT_WIDTH = 360;
const MODEL_LIST_MIN_WIDTH = 300;
const MODEL_LIST_MAX_WIDTH = 620;
const MODEL_NAV_RAIL_WIDTH = 232;
const MODEL_DETAIL_MIN_WIDTH = 420;

function maxModelListWidthForViewport(viewportWidth?: number): number {
  const width = viewportWidth
    ?? (typeof window !== 'undefined' ? window.innerWidth : MODEL_NAV_RAIL_WIDTH + MODEL_LIST_MAX_WIDTH + MODEL_DETAIL_MIN_WIDTH);
  const viewportMax = width - MODEL_NAV_RAIL_WIDTH - MODEL_DETAIL_MIN_WIDTH;
  return Math.max(MODEL_LIST_MIN_WIDTH, Math.min(MODEL_LIST_MAX_WIDTH, viewportMax));
}

function clampModelListWidth(width: number, viewportWidth?: number): number {
  return Math.max(MODEL_LIST_MIN_WIDTH, Math.min(maxModelListWidthForViewport(viewportWidth), Math.round(width)));
}

function loadModelListWidth(): number {
  if (typeof window === 'undefined') return MODEL_LIST_DEFAULT_WIDTH;
  try {
    const stored = Number(window.localStorage.getItem(MODEL_LIST_WIDTH_KEY));
    return clampModelListWidth(Number.isFinite(stored) ? stored : MODEL_LIST_DEFAULT_WIDTH, window.innerWidth);
  } catch {
    return clampModelListWidth(MODEL_LIST_DEFAULT_WIDTH, window.innerWidth);
  }
}

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
      if (cap !== 'unknown') return cap === 'chat' || cap === 'omni';
      return !labels.some(label => NON_PLANNER_LABELS.has(label));
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

function configuredOmniToolTargetIds(draft: CustomModelDraftState, targetType: CustomOmniToolTargetType): string[] {
  const candidates = targetType === 'chat'
    ? [draft.llmComponent, draft.visionComponent]
    : targetType === 'vision'
      ? [draft.visionComponent]
      : [draft.imageComponent];
  const seen = new Set<string>();
  return candidates
    .map(value => value.trim())
    .filter(value => {
      const key = value.toLowerCase();
      if (!key || seen.has(key)) return false;
      seen.add(key);
      return true;
    });
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
  const [activeIndex, setActiveIndex] = useState(-1);
  const inputId = `omni-picker-input-${role}`;
  const listboxId = `omni-picker-listbox-${role}`;
  const optionId = (index: number) => `omni-picker-option-${role}-${index}`;
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

  // Reset active index when the option list changes
  useEffect(() => { setActiveIndex(-1); }, [visibleOptions]);

  const groups: Array<{ source: OmniComponentOptionSource; label: string; options: OmniComponentOption[] }> = [
    { source: 'custom' as OmniComponentOptionSource, label: 'Custom models', options: visibleOptions.filter(option => option.source === 'custom') },
    { source: 'downloaded' as OmniComponentOptionSource, label: 'Downloaded locally', options: visibleOptions.filter(option => option.source === 'downloaded') },
    { source: 'registered' as OmniComponentOptionSource, label: 'Registered registry models', options: visibleOptions.filter(option => option.source === 'registered') },
  ].filter(group => group.options.length > 0);

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    const count = visibleOptions.length;
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (!open) { setOpen(true); setActiveIndex(count > 0 ? 0 : -1); return; }
      setActiveIndex(prev => count > 0 ? (prev + 1) % count : -1);
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (!open) { setOpen(true); setActiveIndex(count > 0 ? count - 1 : -1); return; }
      setActiveIndex(prev => count > 0 ? (prev <= 0 ? count - 1 : prev - 1) : -1);
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (open && activeIndex >= 0 && visibleOptions[activeIndex]) {
        onChange(visibleOptions[activeIndex].id);
        setQuery('');
        setOpen(false);
        setActiveIndex(-1);
      }
    } else if (e.key === 'Escape') {
      e.preventDefault();
      setOpen(false);
      setActiveIndex(-1);
    }
  };

  return (
    <div className="omni-component-picker">
      <label className="omni-component-picker__label" htmlFor={inputId} title={config.help}>{config.label}{config.required ? ' *' : ''}</label>
      <div className="omni-component-picker__control">
        <input
          id={inputId}
          role="combobox"
          aria-expanded={open}
          aria-controls={listboxId}
          aria-activedescendant={open && activeIndex >= 0 && visibleOptions[activeIndex] ? optionId(activeIndex) : undefined}
          aria-autocomplete="list"
          value={open ? query : (selected ? selected.label : '')}
          onFocus={() => { setOpen(true); setQuery(''); }}
          onChange={e => { setQuery(e.target.value); setOpen(true); setActiveIndex(-1); }}
          onBlur={() => window.setTimeout(() => { setOpen(false); setActiveIndex(-1); }, 120)}
          onKeyDown={handleKeyDown}
          placeholder={config.placeholder}
          autoComplete="off"
        />
        {value && !config.required && (
          <button
            type="button"
            className="omni-component-picker__clear"
            onMouseDown={e => e.preventDefault()}
            onClick={() => onChange('')}
            title={`Clear ${config.label}`}
            aria-label={`Clear ${config.label}`}
          >×</button>
        )}
        <span className="omni-component-picker__chevron" aria-hidden="true">⌄</span>
        {open && (
          <div className="omni-component-picker__menu">
            <div role="listbox" id={listboxId} aria-label={`${config.label} options`}>
              {groups.length > 0 ? groups.map(group => (
                <div className="omni-component-picker__group" key={group.source} role="group" aria-label={group.label}>
                  <div className="omni-component-picker__group-label" aria-hidden="true">{group.label}</div>
                  {group.options.map(option => {
                    const flatIdx = visibleOptions.indexOf(option);
                    return (
                      <div
                        key={option.id}
                        id={optionId(flatIdx)}
                        className={`omni-component-picker__option${option.id === value ? ' omni-component-picker__option--selected' : ''}${flatIdx === activeIndex ? ' omni-component-picker__option--focused' : ''}`}
                        onMouseDown={e => e.preventDefault()}
                        onClick={() => { onChange(option.id); setQuery(''); setOpen(false); setActiveIndex(-1); }}
                        role="option"
                        aria-selected={option.id === value}
                      >
                        <span className="omni-component-picker__option-name">{option.label}</span>
                        <span className="omni-component-picker__option-id">{option.id}</span>
                        <span className="omni-component-picker__option-detail">{option.detail}</span>
                      </div>
                    );
                  })}
                </div>
              )) : (
                <div className="omni-component-picker__empty">
                  No compatible {config.label.toLowerCase()} model found. Use the main search or HuggingFace zone to download/register one first.
                </div>
              )}
            </div>
            {queryText.length >= 2 && onHuggingFaceSearch && (
              <button
                type="button"
                className="omni-component-picker__hf-search"
                onMouseDown={e => e.preventDefault()}
                onClick={() => { onHuggingFaceSearch(query.trim()); setOpen(false); setActiveIndex(-1); }}
              >
                Search HuggingFace for "{query.trim()}"
              </button>
            )}
          </div>
        )}
      </div>
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
  const [pulling, setPulling] = useState<Record<string, number>>({});  // model -> percent
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  const pullAbortRef = useRef<Record<string, AbortController>>({});
  const [expandedModel, setExpandedModel] = useState<string | null>(null);
  const [selectedDetailModelId, setSelectedDetailModelId] = useState<string | null>(null);
  const [mobileDetailOpen, setMobileDetailOpen] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [filterTab, setFilterTab] = useState<FilterTab>('all');
  // Left nav-rail filter dimensions (client-local, derived from the model list).
  const [primaryFilter, setPrimaryFilter] = useState<PrimaryFilter>('all');
  const [backendFilter, setBackendFilter] = useState<string>('all');
  const [tagFilter, setTagFilter] = useState<string | null>(null);
  const mobileRail = useWorkspaceMobileRail();
  const [navRailCollapsed, setNavRailCollapsed] = useState(false);
  const [modelListWidth, setModelListWidth] = useState(loadModelListWidth);
  const [showAllAvailable, setShowAllAvailable] = useState(false);
  const searchRef = useRef<HTMLInputElement>(null);

  // Remote registry search state. Provider switches are intentionally separate
  // from rail/category state so those UI changes never retrigger online calls.
  const [providerEnabled, setProviderEnabled] = useState<ProviderEnabledState>({
    huggingface: true,
    modelscope: true,
  });
  const [hfResults, setHfResults] = useState<HFModelResult[]>([]);
  const [hfLoading, setHfLoading] = useState(false);
  const [hfError, setHfError] = useState<string | null>(null);
  const [modelScopeResults, setModelScopeResults] = useState<HFModelResult[]>([]);
  const [modelScopeLoading, setModelScopeLoading] = useState(false);
  const [modelScopeError, setModelScopeError] = useState<string | null>(null);
  const [expandedRemoteModel, setExpandedRemoteModel] = useState<string | null>(null);
  const [selectedRemoteModel, setSelectedRemoteModel] = useState<HFModelResult | null>(null);
  const [selectedRemoteProvider, setSelectedRemoteProvider] = useState<ModelRegistryProvider>('huggingface');
  const [pullingRemote, setPullingRemote] = useState<Record<string, { percent: number; modelName: string; checkpoint: string }>>({});
  const pullRemoteAbortRef = useRef<Record<string, AbortController>>({});
  const [remoteVariants, setRemoteVariants] = useState<Record<string, PullVariantsResult>>({});
  const [remoteVariantsLoading, setRemoteVariantsLoading] = useState<Record<string, boolean>>({});

  const [customModels, setCustomModels] = useState<ModelInfo[]>(() => loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  const [routerModels, setRouterModels] = useState<ModelInfo[]>(() => loadRouterRecords(accountSession.storageScope).map(routerRecordToModelInfo));
  const [showRouterEditor, setShowRouterEditor] = useState(false);
  const [routerEditorModel, setRouterEditorModel] = useState<ModelInfo | null>(null);
  const [showGlobalSettings, setShowGlobalSettings] = useState(false);
  const [showCustomForm, setShowCustomForm] = useState(false);
  const [editingCustomModelName, setEditingCustomModelName] = useState<string | null>(null);
  const [customError, setCustomError] = useState<string | null>(null);
  const [customJsonNotice, setCustomJsonNotice] = useState<string | null>(null);
  const [customDraft, setCustomDraft] = useState<CustomModelDraftState>(() => createEmptyCustomDraft());
  const [dynamicRecipeOptions, setDynamicRecipeOptions] = useState<Partial<Record<CustomModelCapability, CustomRecipeOption[]>>>({});
  const [customRecipeAvailabilityLoaded, setCustomRecipeAvailabilityLoaded] = useState(false);
  const [pinnedModels, setPinnedModels] = useState<string[]>(() => loadPinnedModelNames(accountSession.storageScope));
  const [favoriteModels, setFavoriteModels] = useState<string[]>(() => loadFavoriteModels(accountSession.storageScope));
  // Multi-select functional capability filter driven by the funnel popover.
  const [capabilityFilter, setCapabilityFilter] = useState<Set<string>>(() => new Set());
  // Real disk usage for the storage meter (null until/unless lemond exposes it).
  const [storageInfo, setStorageInfo] = useState<import('../api').StorageInfo | null>(null);
  const [ttsPlaybackSettings, setTtsPlaybackSettings] = useState(() => loadTtsPlaybackSettings(accountSession.storageScope));
  const [globalModelSettings, setGlobalModelSettings] = useState<GlobalModelSettings>(() => loadGlobalModelSettings(accountSession.storageScope));
  const automaticUpdateStartedRef = useRef(false);
  const customJsonInputRef = useRef<HTMLInputElement>(null);

  const [userPresets, setUserPresets] = useState<Preset[]>(loadUserPresets);
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(loadApplied);
  const [presetNotice, setPresetNotice] = useState<string | null>(null);
  const [serverDefaultCtxSize, setServerDefaultCtxSize] = useState<number>(DEFAULT_CONTEXT_SIZE);
  const hasVisibleModelsRef = useRef(false);
  const modelsSnapshotRef = useRef<string>('');
  const loadedSnapshotRef = useRef<string>('');

  useEffect(() => {
    try {
      window.localStorage.setItem(MODEL_LIST_WIDTH_KEY, String(modelListWidth));
    } catch {
      // Non-critical: width persistence is best-effort only.
    }
  }, [modelListWidth]);

  useEffect(() => {
    const clampToViewport = () => {
      setModelListWidth(width => clampModelListWidth(width, window.innerWidth));
    };
    clampToViewport();
    window.addEventListener('resize', clampToViewport);
    return () => window.removeEventListener('resize', clampToViewport);
  }, []);

  const modelDetailLayoutStyle = useMemo(() => ({
    '--model-list-panel-width': `${modelListWidth}px`,
  } as React.CSSProperties), [modelListWidth]);

  const handleModelListResizeStart = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    if (window.innerWidth <= 700) return;
    event.preventDefault();

    const startX = event.clientX;
    const startWidth = modelListWidth;
    const handle = event.currentTarget;
    try { handle.setPointerCapture(event.pointerId); } catch { /* ignore */ }

    const handlePointerMove = (moveEvent: PointerEvent) => {
      const nextWidth = clampModelListWidth(startWidth + moveEvent.clientX - startX, window.innerWidth);
      setModelListWidth(nextWidth);
    };

    const stopResize = () => {
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('pointerup', stopResize);
      window.removeEventListener('pointercancel', stopResize);
      document.body.classList.remove('is-resizing-model-list');
      try { handle.releasePointerCapture(event.pointerId); } catch { /* ignore */ }
    };

    document.body.classList.add('is-resizing-model-list');
    window.addEventListener('pointermove', handlePointerMove);
    window.addEventListener('pointerup', stopResize, { once: true });
    window.addEventListener('pointercancel', stopResize, { once: true });
  }, [modelListWidth]);

  const handleModelListResizeKeyDown = useCallback((event: React.KeyboardEvent<HTMLDivElement>) => {
    const largeStep = event.shiftKey ? 40 : 16;
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      setModelListWidth(width => clampModelListWidth(width - largeStep, window.innerWidth));
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      setModelListWidth(width => clampModelListWidth(width + largeStep, window.innerWidth));
    } else if (event.key === 'Home') {
      event.preventDefault();
      setModelListWidth(MODEL_LIST_MIN_WIDTH);
    } else if (event.key === 'End') {
      event.preventDefault();
      setModelListWidth(maxModelListWidthForViewport(window.innerWidth));
    }
  }, []);


  useEffect(() => {
    hasVisibleModelsRef.current = models.length > 0 || loadedModels.length > 0 || customModels.length > 0 || routerModels.length > 0;
  }, [models.length, loadedModels.length, customModels.length, routerModels.length]);

  useEffect(() => downloadStore.subscribe(setDownloadItems), []);

  const reloadCustomModels = useCallback(() => {
    setCustomModels(loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  }, [accountSession.storageScope]);

  useEffect(() => { reloadCustomModels(); }, [reloadCustomModels]);

  const reloadRouterModels = useCallback(() => {
    setRouterModels(loadRouterRecords(accountSession.storageScope).map(routerRecordToModelInfo));
  }, [accountSession.storageScope]);

  useEffect(() => { reloadRouterModels(); }, [reloadRouterModels]);


  useEffect(() => {
    setPinnedModels(loadPinnedModelNames(accountSession.storageScope));
    setFavoriteModels(loadFavoriteModels(accountSession.storageScope));
  }, [accountSession.storageScope]);

  // Fetch real model-storage disk stats. Returns null in the POC (lemond has no
  // disk endpoint yet) → the rail derives a graceful fallback. Re-runs when the
  // model set changes so the meter refreshes as downloads complete.
  useEffect(() => {
    let cancelled = false;
    api.getStorageInfo()
      .then(info => { if (!cancelled) setStorageInfo(info); })
      .catch(() => { if (!cancelled) setStorageInfo(null); });
    return () => { cancelled = true; };
  }, [models.length]);

  useEffect(() => {
    const reloadTtsSettings = () => setTtsPlaybackSettings(loadTtsPlaybackSettings(accountSession.storageScope));
    reloadTtsSettings();
    window.addEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
    return () => window.removeEventListener(TTS_SETTINGS_EVENT, reloadTtsSettings);
  }, [accountSession.storageScope]);

  useEffect(() => {
    const reloadGlobalSettings = () => setGlobalModelSettings(loadGlobalModelSettings(accountSession.storageScope));
    automaticUpdateStartedRef.current = false;
    reloadGlobalSettings();
    window.addEventListener(GLOBAL_MODEL_SETTINGS_EVENT, reloadGlobalSettings);
    return () => window.removeEventListener(GLOBAL_MODEL_SETTINGS_EVENT, reloadGlobalSettings);
  }, [accountSession.storageScope]);

  const refreshCustomRecipeAvailability = useCallback(async () => {
    if (!api.isConnected) {
      setDynamicRecipeOptions({});
      setCustomRecipeAvailabilityLoaded(false);
      return;
    }

    try {
      const info = await api.systemInfo();
      const hasRecipeData = Boolean(systemRecipeEntries(info));
      setDynamicRecipeOptions(hasRecipeData ? recipeOptionsFromSystemInfo(info) : {});
      setCustomRecipeAvailabilityLoaded(hasRecipeData);
    } catch {
      setDynamicRecipeOptions({});
      setCustomRecipeAvailabilityLoaded(false);
    }
  }, []);

  useEffect(() => {
    void refreshCustomRecipeAvailability();
  }, [connectionStatus, refreshCustomRecipeAvailability]);

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
      const defaultCtxSize = await api.getDefaultContextSize();
      setServerDefaultCtxSize(typeof defaultCtxSize === 'number' ? defaultCtxSize : DEFAULT_CONTEXT_SIZE);
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

  /* ── Remote registry search ────────────────────────────────
     Deliberately keyed ONLY by the text query and provider switch. Changing
     categories, primary modes, backend filters, tags, or capabilities merely
     filters cached results and never starts another online request. */

  useEffect(() => {
    const q = searchQuery.trim();
    if (q.length < 2 || !providerEnabled.huggingface) {
      setHfResults([]);
      setHfLoading(false);
      setHfError(null);
      return;
    }

    const cacheKey = providerSearchCacheKey('huggingface', q);
    const cached = REMOTE_SEARCH_CACHE.get(cacheKey);
    if (cached) {
      setHfResults(cached);
      const cachedVariants: Record<string, PullVariantsResult> = {};
      for (const result of cached) {
        const key = providerKey('huggingface', result.id);
        const variants = REMOTE_VARIANT_CACHE.get(key);
        if (variants) cachedVariants[key] = variants;
      }
      if (Object.keys(cachedVariants).length > 0) {
        setRemoteVariants(prev => ({ ...prev, ...cachedVariants }));
      }
      setHfLoading(false);
      setHfError(null);
      return;
    }

    setHfResults([]);
    setHfLoading(true);
    setHfError(null);
    const ac = new AbortController();
    const timer = window.setTimeout(async () => {
      try {
        const results = await searchHuggingFace(q, ac.signal);
        REMOTE_SEARCH_CACHE.set(cacheKey, results);
        setHfResults(results);
      } catch (err) {
        if (!ac.signal.aborted) {
          setHfResults([]);
          setHfError(friendlyErrorMessage(err));
        }
      } finally {
        if (!ac.signal.aborted) setHfLoading(false);
      }
    }, 500);

    return () => {
      window.clearTimeout(timer);
      ac.abort();
    };
  }, [searchQuery, providerEnabled.huggingface]);

  useEffect(() => {
    const q = searchQuery.trim();
    if (q.length < 2 || !providerEnabled.modelscope) {
      setModelScopeResults([]);
      setModelScopeLoading(false);
      setModelScopeError(null);
      return;
    }

    const cacheKey = providerSearchCacheKey('modelscope', q);
    const cached = REMOTE_SEARCH_CACHE.get(cacheKey);
    if (cached) {
      setModelScopeResults(cached);
      const cachedVariants: Record<string, PullVariantsResult> = {};
      for (const result of cached) {
        const key = providerKey('modelscope', result.id);
        const variants = REMOTE_VARIANT_CACHE.get(key);
        if (variants) cachedVariants[key] = variants;
      }
      if (Object.keys(cachedVariants).length > 0) {
        setRemoteVariants(prev => ({ ...prev, ...cachedVariants }));
      }
      setModelScopeLoading(false);
      setModelScopeError(null);
      return;
    }

    setModelScopeResults([]);
    setModelScopeLoading(true);
    setModelScopeError(null);
    const ac = new AbortController();
    const timer = window.setTimeout(async () => {
      try {
        const candidates = await searchModelScope(q, ac.signal);
        const validated: Array<{ result: HFModelResult; order: number }> = [];
        let next = 0;
        const publishValidated = () => {
          const ordered = [...validated]
            .sort((a, b) => a.order - b.order)
            .slice(0, MODELSCOPE_RESULT_LIMIT)
            .map(item => item.result);
          setModelScopeResults(ordered);
        };
        const worker = async () => {
          while (!ac.signal.aborted && validated.length < MODELSCOPE_RESULT_LIMIT && next < candidates.length) {
            const order = next;
            const candidate = candidates[next++];
            try {
              const variants = await loadRemoteVariants('modelscope', candidate.id, ac.signal);
              if (!variants?.variants?.length || validated.some(item => item.result.id === candidate.id)) continue;
              setRemoteVariants(prev => ({ ...prev, [providerKey('modelscope', candidate.id)]: variants }));
              validated.push({ result: candidate, order });
              publishValidated();
            } catch {
              // Registry metadata is only a candidate hint. Repositories without
              // usable GGUF variants are omitted, matching Lemonade main.
            }
          }
        };
        await Promise.all(Array.from({ length: Math.min(REMOTE_VARIANT_CONCURRENCY, candidates.length) }, worker));
        if (!ac.signal.aborted) {
          const finalResults = [...validated]
            .sort((a, b) => a.order - b.order)
            .slice(0, MODELSCOPE_RESULT_LIMIT)
            .map(item => item.result);
          REMOTE_SEARCH_CACHE.set(cacheKey, finalResults);
          setModelScopeResults(finalResults);
        }
      } catch (err) {
        if (!ac.signal.aborted) {
          setModelScopeResults([]);
          setModelScopeError(friendlyErrorMessage(err));
        }
      } finally {
        if (!ac.signal.aborted) setModelScopeLoading(false);
      }
    }, 400);

    return () => {
      window.clearTimeout(timer);
      ac.abort();
    };
  }, [searchQuery, providerEnabled.modelscope]);

  // Enrich HF rows with server-derived suggested_labels/mmproj metadata. The
  // module cache makes this a one-time probe per provider/repository, and these
  // probes only follow a new search result set — never a local filter change.
  useEffect(() => {
    if (!providerEnabled.huggingface || hfResults.length === 0) return;
    let cancelled = false;
    let next = 0;
    const candidates = hfResults.slice(0, 12);
    const worker = async () => {
      while (!cancelled && next < candidates.length) {
        const candidate = candidates[next++];
        const key = providerKey('huggingface', candidate.id);
        if (remoteVariants[key]) continue;
        try {
          const variants = await loadRemoteVariants('huggingface', candidate.id);
          if (variants && !cancelled) setRemoteVariants(prev => ({ ...prev, [key]: variants }));
        } catch {
          // Capability enrichment is best effort; the search result remains usable.
        }
      }
    };
    void Promise.all(Array.from({ length: Math.min(3, candidates.length) }, worker));
    return () => { cancelled = true; };
  }, [hfResults, providerEnabled.huggingface]);

  useEffect(() => {
    setExpandedRemoteModel(null);
    setSelectedRemoteModel(null);
  }, [searchQuery]);

  /* ── Actions ─────────────────────────────────────────────── */

  const findCurrentModel = (name: string): ModelInfo | null => {
    const target = name.toLowerCase();
    return allModels.find(mi => modelName(mi).toLowerCase() === target) || null;
  };

  const modelCheckpoint = (model: ModelInfo | null | undefined): string => {
    if (!model) return '';
    return String((model as any).checkpoint || (model as any).checkpoints?.main || '').trim();
  };

  const resolveRemoteModelName = (
    provider: ModelRegistryProvider,
    modelId: string,
    variantName: string,
    recipe: string,
    variants?: PullVariantsResult,
  ): string => {
    const checkpoint = remoteVariantCheckpoint(modelId, variantName, recipe);
    const defaultName = remoteDefaultModelName(provider, modelId, variants, variantName, recipe);
    const lookup = (bareName: string): ModelInfo | null => (
      findCurrentModel(`user.${bareName}`) || findCurrentModel(bareName)
    );
    const matchesCheckpoint = (info: ModelInfo | null): boolean => modelCheckpoint(info) === checkpoint;

    const existingDefault = lookup(defaultName);
    if (!existingDefault || matchesCheckpoint(existingDefault)) return `user.${defaultName}`;

    const owner = modelId.split('/')[0]?.trim() || '';
    const safeOwner = owner.replace(/[^A-Za-z0-9_.-]+/g, '-').replace(/^-+|-+$/g, '');
    const fallbackBase = safeOwner ? `${defaultName}-${safeOwner}` : defaultName;
    let candidate = fallbackBase;
    let suffix = 2;
    let existingCandidate = lookup(candidate);
    while (existingCandidate && !matchesCheckpoint(existingCandidate)) {
      candidate = `${fallbackBase}-${suffix}`;
      suffix += 1;
      existingCandidate = lookup(candidate);
    }
    return `user.${candidate}`;
  };

  const activeRemotePull = (
    provider: ModelRegistryProvider,
    modelId: string,
    variants?: PullVariantsResult,
  ): { modelName: string; percent: number; downloadId?: string } | null => {
    const key = providerKey(provider, modelId);
    const local = pullingRemote[key];
    if (local) {
      const download = activeDownloadForModel(downloadItems, local.modelName);
      return {
        modelName: local.modelName,
        percent: download?.percent ?? local.percent,
        downloadId: download?.id,
      };
    }

    const sourceMatches = (model: ModelInfo): boolean => {
      const source = String((model as any).registry_source || (model as any).source || '').toLowerCase();
      return !source || source === provider;
    };
    const registered = allModels.find(model => {
      const checkpoint = modelCheckpoint(model);
      return sourceMatches(model)
        && (checkpoint === modelId || checkpoint.startsWith(`${modelId}:`))
        && Boolean(activeDownloadForModel(downloadItems, modelName(model)));
    });
    if (registered) {
      const name = modelName(registered);
      const download = activeDownloadForModel(downloadItems, name);
      if (download) return { modelName: name, percent: download.percent, downloadId: download.id };
    }

    for (const variant of variants?.variants || []) {
      const name = resolveRemoteModelName(provider, modelId, variant.name, variants?.recipe || '', variants);
      const download = activeDownloadForModel(downloadItems, name);
      if (download) return { modelName: name, percent: download.percent, downloadId: download.id };
    }
    return null;
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

  const loadWithGlobalPolicy = async (model: ModelInfo): Promise<void> => {
    await loadWithGlobalModelPolicy({
      loadedModels,
      allModels,
      target: model,
      pinnedNames: pinnedModels,
      settings: globalModelSettings,
      unload: name => api.unloadModel(name),
      load: () => loadModelRuntime(model),
    });
  };

  const handleLoad = async (model: ModelInfo) => {
    if (loadingModel) return;
    const name = modelName(model);
    if (activeDownloadForModel(downloadStore.snapshot(), name)) {
      setLoadError({ modelName: name, message: `${name} is still downloading. Wait for the download to finish before loading it.` });
      window.setTimeout(() => setLoadError(prev => prev?.modelName === name ? null : prev), 6000);
      return;
    }
    setLoadError(null);
    setLoadingModel(name);
    try {
      await loadWithGlobalPolicy(model);
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

  // #2356 (simplified): a load-time preset change needs a real reload. The
  // detail panel already classifies live-vs-reload and rebinds the active
  // preset; here we just perform the reload (unload + load via api.reloadModel)
  // and refresh so the loaded-model snapshot reflects the reinitialization.
  // Live (request-time) changes never reach here — they are a pure client-local
  // rebind handled entirely in the panel (no server round-trip).
  const handleReloadModel = async (
    model: LoadedModel,
    recipeOptions?: Record<string, unknown>,
  ) => {
    const info = allModels.find(m => modelName(m) === model.model_name) ?? null;
    await api.reloadModel(model.model_name, recipeOptions, info);
    await refresh();
  };

  const handleDeleteRouterDefinition = async (name: string): Promise<void> => {
    if (api.isConnected) await api.deleteModel(name);
    deleteRouterRecord(accountSession.storageScope, name);
    reloadRouterModels();
    if (selectedDetailModelId === name) setSelectedDetailModelId(null);
  };

  const handleDelete = async (model: ModelInfo) => {
    const name = modelName(model);
    if (modelIsCustom(model) && String((model as any).recipe || '').toLowerCase() === ROUTER_RECIPE) {
      if (!confirm(`Delete router definition "${model.display_name || name}"?`)) return;
      try {
        await handleDeleteRouterDefinition(name);
      } catch (err) {
        console.error('Router delete failed:', err);
      }
      return;
    }
    if (modelIsCustom(model)) {
      if (!confirm(`Delete custom model definition "${model.display_name || name}"? This does not remove external model files.`)) return;
      try {
        if (api.isConnected) await api.deleteModel(name);
        deleteCustomModel(accountSession.storageScope, String((model as any).id || name));
        reloadCustomModels();
        await refresh();
      } catch (err) {
        console.error('Custom model delete failed:', err);
      }
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
    if (pulling[name] !== undefined || activeDownloadForModel(downloadStore.snapshot(), name)) return;
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));
    downloadStore.markLocal(name, 'downloading', 'model');

    await ensureCustomCollectionComponentsRegistered(model);

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        const item = downloadStore.upsertFromPull(name, data, 'model');
        setPulling(p => ({ ...p, [name]: item?.percent ?? (typeof data.percent === 'number' ? data.percent : p[name] ?? 0) }));
      },
      onComplete: (data) => {
        downloadStore.upsertFromPull(name, { ...data, status: 'completed', complete: true, percent: 100 }, 'model');
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        refresh();
      },
      onError: (err) => {
        downloadStore.upsertFromPull(name, { status: 'error', error: friendlyErrorMessage(err) }, 'model');
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    try {
      await api.pullModel(name, callbacks, customRegistrationOptions(model));
    } finally {
      delete pullAbortRef.current[name];
      setPulling(p => {
        if (p[name] === undefined) return p;
        const next = { ...p };
        delete next[name];
        return next;
      });
    }
  };

  const handleCancelPull = async (name: string) => {
    pullAbortRef.current[name]?.abort();
    await api.controlDownload(`model:${name}`, 'cancel').catch(() => undefined);
    delete pullAbortRef.current[name];
    downloadStore.markLocal(name, 'cancelled', 'model');
    setPulling(p => { const next = { ...p }; delete next[name]; return next; });
  };

  const handlePullAndLoad = async (model: ModelInfo) => {
    const name = modelName(model);
    if (pulling[name] !== undefined || activeDownloadForModel(downloadStore.snapshot(), name)) return;
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));
    downloadStore.markLocal(name, 'downloading', 'model');

    await ensureCustomCollectionComponentsRegistered(model);

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        const item = downloadStore.upsertFromPull(name, data, 'model');
        setPulling(p => ({ ...p, [name]: item?.percent ?? (typeof data.percent === 'number' ? data.percent : p[name] ?? 0) }));
      },
      onComplete: async (data) => {
        downloadStore.upsertFromPull(name, { ...data, status: 'completed', complete: true, percent: 100 }, 'model');
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
      onError: (err) => {
        downloadStore.upsertFromPull(name, { status: 'error', error: friendlyErrorMessage(err) }, 'model');
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    try {
      await api.pullModel(name, callbacks, customRegistrationOptions(model));
    } finally {
      delete pullAbortRef.current[name];
      setPulling(p => {
        if (p[name] === undefined) return p;
        const next = { ...p };
        delete next[name];
        return next;
      });
    }
  };

  const handleRemotePull = async (provider: ModelRegistryProvider, modelId: string, variantName: string, recipe: string) => {
    const key = providerKey(provider, modelId);
    const vdata = remoteVariants[key];
    if (activeRemotePull(provider, modelId, vdata)) return;
    const checkpoint = remoteVariantCheckpoint(modelId, variantName, recipe);
    const targetModelName = resolveRemoteModelName(provider, modelId, variantName, recipe, vdata);
    const ac = new AbortController();
    pullRemoteAbortRef.current[key] = ac;
    setPullingRemote(p => ({ ...p, [key]: { percent: 0, modelName: targetModelName, checkpoint } }));
    downloadStore.markLocal(targetModelName, 'downloading', 'model');

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        const item = downloadStore.upsertFromPull(targetModelName, data, 'model');
        setPullingRemote(p => ({
          ...p,
          [key]: {
            percent: item?.percent ?? (typeof data.percent === 'number' ? data.percent : p[key]?.percent ?? 0),
            modelName: targetModelName,
            checkpoint,
          },
        }));
      },
      onComplete: (data) => {
        downloadStore.upsertFromPull(targetModelName, { ...data, status: 'completed', complete: true, percent: 100 }, 'model');
        delete pullRemoteAbortRef.current[key];
        setPullingRemote(p => { const next = { ...p }; delete next[key]; return next; });
        refresh();
      },
      onError: (err) => {
        downloadStore.upsertFromPull(targetModelName, { status: 'error', error: friendlyErrorMessage(err) }, 'model');
        console.error(`${PROVIDER_META[provider].label} pull failed:`, err);
        delete pullRemoteAbortRef.current[key];
        setPullingRemote(p => { const next = { ...p }; delete next[key]; return next; });
      },
      signal: ac.signal,
    };

    const labels = new Set(vdata?.suggested_labels || []);
    if (vdata?.mmproj_files?.length) labels.add('vision');
    try {
      await api.pullModel(targetModelName, callbacks, {
        checkpoint,
        recipe,
        source: provider,
        mmproj: vdata?.mmproj_files?.[0],
        labels: [...labels],
        vision: labels.has('vision'),
        embedding: labels.has('embeddings'),
        reranking: labels.has('reranking'),
      });
    } finally {
      delete pullRemoteAbortRef.current[key];
      setPullingRemote(p => {
        if (!p[key]) return p;
        const next = { ...p };
        delete next[key];
        return next;
      });
    }
  };

  const handleCancelRemotePull = async (provider: ModelRegistryProvider, modelId: string) => {
    const key = providerKey(provider, modelId);
    pullRemoteAbortRef.current[key]?.abort();
    const vdata = remoteVariants[key];
    const active = activeRemotePull(provider, modelId, vdata);
    if (active) {
      await api.controlDownload(active.downloadId || `model:${active.modelName}`, 'cancel').catch(() => undefined);
      downloadStore.markLocal(active.modelName, 'cancelled', 'model');
    }
    delete pullRemoteAbortRef.current[key];
    setPullingRemote(p => { const next = { ...p }; delete next[key]; return next; });
  };

  const fetchRemoteVariants = async (provider: ModelRegistryProvider, modelId: string) => {
    const key = providerKey(provider, modelId);
    if (remoteVariants[key] || remoteVariantsLoading[key]) return;
    setRemoteVariantsLoading(prev => ({ ...prev, [key]: true }));
    try {
      const result = await loadRemoteVariants(provider, modelId);
      if (result) setRemoteVariants(prev => ({ ...prev, [key]: result }));
    } catch (err) {
      console.error(`Failed to fetch ${PROVIDER_META[provider].label} variants for`, modelId, err);
    }
    setRemoteVariantsLoading(prev => ({ ...prev, [key]: false }));
  };


  const handleCustomDraftChange = (patch: Partial<CustomModelDraftState>) => {
    setCustomDraft(prev => ({ ...prev, ...patch }));
    setCustomError(null);
  };

  const openCustomForm = (mode: CustomFormMode = 'model') => {
    setShowRouterEditor(false);
    setShowGlobalSettings(false);
    setRouterEditorModel(null);
    setEditingCustomModelName(null);
    setCustomDraft(createEmptyCustomDraft(mode));
    setCustomError(null);
    setShowCustomForm(true);
    void refreshCustomRecipeAvailability();
  };

  const openCustomCollectionEditor = (model: ModelInfo) => {
    setShowRouterEditor(false);
    setShowGlobalSettings(false);
    setRouterEditorModel(null);
    const name = modelName(model);
    setSelectedDetailModelId(name);
    setMobileDetailOpen(true);
    setEditingCustomModelName(name);
    setCustomDraft(customDraftFromModel(model));
    setCustomError(null);
    setShowCustomForm(true);
    void refreshCustomRecipeAvailability();
  };

  const closeCustomForm = () => {
    setShowCustomForm(false);
    setEditingCustomModelName(null);
    setCustomError(null);
  };

  const openRouterEditor = (model?: ModelInfo | null) => {
    closeCustomForm();
    setShowGlobalSettings(false);
    const routerModel = model && String((model as any).recipe || '').toLowerCase() === ROUTER_RECIPE ? model : null;
    setRouterEditorModel(routerModel);
    setShowRouterEditor(true);
    setMobileDetailOpen(true);
  };

  const closeRouterEditor = () => {
    setShowRouterEditor(false);
    setRouterEditorModel(null);
  };

  const openGlobalSettings = () => {
    closeCustomForm();
    closeRouterEditor();
    setShowGlobalSettings(true);
    setMobileDetailOpen(true);
  };

  const closeGlobalSettings = () => {
    setShowGlobalSettings(false);
  };

  const pullRegistrationOrThrow = async (
    modelNameValue: string,
    options: Record<string, unknown> | undefined,
  ): Promise<void> => {
    let failure: unknown = null;
    await api.pullModel(modelNameValue, {
      onError: errorValue => { failure = errorValue; },
    }, options);
    if (failure) throw failure instanceof Error ? failure : new Error(String(failure));
  };

  const handleRegisterRouter = async (request: RouterPullRequest): Promise<void> => {
    const registered = new Set<string>();
    const registering = new Set<string>();
    const registerCustomDependency = async (component: ModelInfo): Promise<void> => {
      const name = modelName(component);
      const key = name.toLowerCase();
      if (!name || registered.has(key) || !(component as any).custom) return;
      if (registering.has(key)) throw new Error(`Circular custom component reference: ${name}`);
      registering.add(key);
      if (isCollectionModel(component)) {
        for (const nestedName of getCollectionComponents(component)) {
          const nested = findCurrentModel(nestedName);
          if (nested) await registerCustomDependency(nested);
        }
      }
      await pullRegistrationOrThrow(name, customRegistrationOptions(component));
      registering.delete(key);
      registered.add(key);
    };

    for (const componentName of request.components) {
      const component = findCurrentModel(componentName);
      if (component) await registerCustomDependency(component);
    }
    await pullRegistrationOrThrow(request.model_name, {
      version: request.version,
      recipe: request.recipe,
      components: request.components,
      routing: request.routing,
    });
    await refresh();
  };

  const handleRouterSaved = (model: ModelInfo) => {
    reloadRouterModels();
    setRouterEditorModel(model);
    setPrimaryFilter('my-models');
    setSearchQuery('');
    setSelectedDetailModelId(modelName(model));
  };

  const recipeOptionsForDraft = useCallback((capability: CustomModelCapability, omniSource: 'single' | 'collection') => {
    if (capability === 'omni' && omniSource === 'collection') {
      return recipeOptionsForCustomDraft(capability, omniSource);
    }

    const dynamic = dynamicRecipeOptions[capability] || [];
    if (dynamic.length > 0) return dynamic;

    // Fallback only when /system-info is unavailable. If /system-info.recipes
    // was loaded and has no non-unsupported backend for this capability, keep
    // the selector empty instead of re-introducing unsupported static options.
    if (customRecipeAvailabilityLoaded) return [];

    return recipeOptionsForCustomDraft(capability, omniSource);
  }, [customRecipeAvailabilityLoaded, dynamicRecipeOptions]);

  const defaultRecipeForCapability = (capability: CustomModelCapability, omniSource: 'single' | 'collection' = customDraft.omniSource) => {
    const options = recipeOptionsForDraft(capability, omniSource);
    return options[0]?.value || 'llamacpp';
  };

  const handleExportCustomModels = () => {
    exportJsonFile('lemonade-custom-models', exportCustomModelsPayload(accountSession.storageScope));
    setCustomJsonNotice('Exported custom model JSON.');
    window.setTimeout(() => setCustomJsonNotice(null), 2200);
  };

  const handleImportCustomModels = async (file: File | null | undefined) => {
    if (!file) return;
    setCustomError(null);
    try {
      const payload = JSON.parse(await file.text());
      const result = importCustomModels(accountSession.storageScope, payload);
      reloadCustomModels();
      setCustomJsonNotice(`Imported ${result.imported} custom model${result.imported === 1 ? '' : 's'}${result.skipped ? `, skipped ${result.skipped}` : ''}.`);
      if (result.errors.length) setCustomError(result.errors.slice(0, 3).join(' '));
      window.setTimeout(() => setCustomJsonNotice(null), 3200);
    } catch (err) {
      setCustomError(`Could not import JSON: ${err instanceof Error ? err.message : String(err)}`);
    } finally {
      if (customJsonInputRef.current) customJsonInputRef.current.value = '';
    }
  };

  const togglePinnedModel = (name: string) => {
    setPinnedModels(prev => {
      const exists = prev.some(item => item.toLowerCase() === name.toLowerCase());
      const next = exists ? prev.filter(item => item.toLowerCase() !== name.toLowerCase()) : [name, ...prev];
      savePinnedModelNames(accountSession.storageScope, next);
      return next;
    });
  };

  const toggleFavoriteModel = (name: string) => {
    setFavoriteModels(prev => {
      const exists = prev.some(item => item.toLowerCase() === name.toLowerCase());
      const next = exists ? prev.filter(item => item.toLowerCase() !== name.toLowerCase()) : [name, ...prev];
      saveFavoriteModels(accountSession.storageScope, next);
      return next;
    });
  };

  const activeTtsModelName = ttsPlaybackSettings.modelName;

  const toggleTtsSpeechModel = (name: string) => {
    const next = activeTtsModelName === name ? null : name;
    saveActiveTtsModel(accountSession.storageScope, next);
    setTtsPlaybackSettings(loadTtsPlaybackSettings(accountSession.storageScope));
  };

  const setSpeakUserText = (enabled: boolean) => {
    saveSpeakUserText(accountSession.storageScope, enabled);
    setTtsPlaybackSettings(loadTtsPlaybackSettings(accountSession.storageScope));
  };

  const setTtsPlaybackMode = (mode: TtsPlaybackMode) => {
    saveTtsPlaybackMode(accountSession.storageScope, mode);
    setTtsPlaybackSettings(loadTtsPlaybackSettings(accountSession.storageScope));
  };

  const renderPinAndSpeechControl = (name: string, isPinned: boolean, capability: ModelCapability) => {
    const pinButton = (
      <button
        type="button"
        className={`row__pin${isPinned ? ' row__pin--active' : ''}`}
        onClick={(e) => { e.stopPropagation(); togglePinnedModel(name); }}
        title={isPinned ? `Unpin ${name}` : `Pin ${name}`}
        aria-label={isPinned ? `Unpin ${name}` : `Pin ${name}`}
        aria-pressed={isPinned}
      ><Icon name="pin" size={13} /></button>
    );

    if (capability !== 'tts') return pinButton;
    const isSpeechActive = activeTtsModelName === name;
    return (
      <span className="row__tts-actions" title="TTS playback controls">
        {pinButton}
        <button
          type="button"
          className={`row__speech${isSpeechActive ? ' row__speech--active' : ''}`}
          onClick={(e) => { e.stopPropagation(); toggleTtsSpeechModel(name); }}
          title={isSpeechActive ? `Stop using ${name} for spoken replies` : `Read assistant chat messages with ${name}`}
          aria-label={isSpeechActive ? `Disable spoken replies using ${name}` : `Use ${name} for spoken replies`}
          aria-pressed={isSpeechActive}
        ><Icon name="speech" size={13} /></button>
      </span>
    );
  };

  const handleSaveCustomModel = async (e: React.FormEvent) => {
    e.preventDefault();
    setCustomError(null);
    try {
      if (!customDraft.displayName.trim()) {
        throw new Error('Enter a model name.');
      }
      if (customDraft.capability === 'omni' && customDraft.omniSource === 'collection' && !customDraft.llmComponent.trim()) {
        throw new Error('Select a planner LLM for the Omni collection.');
      }
      const isOmniCollection = customDraft.capability === 'omni' && customDraft.omniSource === 'collection';
      const componentRoles = {
        llm: customDraft.llmComponent,
        vision: customDraft.visionComponent,
        image: customDraft.imageComponent,
        edit: customDraft.editComponent,
        transcription: customDraft.transcriptionComponent,
        speech: customDraft.speechComponent,
      };
      const builtinToolNames = new Set(['generate_image', 'edit_image', 'text_to_speech', 'transcribe_audio', 'analyze_image']);
      const seenCustomToolNames = new Set<string>();
      const customTools = isOmniCollection
        ? customDraft.omniCustomTools.map((tool, index): CustomOmniToolDefinition | null => {
          const hasAnyValue = [tool.name, tool.description, tool.targetModel, tool.systemPrompt, tool.promptTemplate, tool.parametersJson, tool.maxTokens].some(value => String(value || '').trim());
          if (!hasAnyValue) return null;
          const name = sanitizeOmniToolName(tool.name);
          if (!name) throw new Error(`Custom tool ${index + 1}: enter a tool name.`);
          if (builtinToolNames.has(name)) throw new Error(`Custom tool ${name} conflicts with a built-in Omni tool.`);
          if (seenCustomToolNames.has(name.toLowerCase())) throw new Error(`Custom tool ${name} is duplicated.`);
          seenCustomToolNames.add(name.toLowerCase());
          const targetModel = tool.targetModel.trim();
          if (!targetModel) throw new Error(`Custom tool ${name}: select a target model.`);
          const configuredTargets = configuredOmniToolTargetIds(customDraft, tool.targetType);
          if (!configuredTargets.some(candidate => candidate.toLowerCase() === targetModel.toLowerCase())) {
            throw new Error(`Custom tool ${name}: target ${targetModel} is not configured in this Omni collection.`);
          }
          const description = tool.description.trim();
          if (!description) throw new Error(`Custom tool ${name}: enter a description so the planner knows when to call it.`);
          let parameters: Record<string, unknown>;
          try {
            const parsed = JSON.parse(tool.parametersJson || defaultOmniToolParametersJson(tool.targetType));
            if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) throw new Error('schema must be a JSON object');
            parameters = parsed as Record<string, unknown>;
          } catch (err) {
            throw new Error(`Custom tool ${name}: invalid JSON parameter schema (${err instanceof Error ? err.message : String(err)}).`);
          }
          const maxTokens = Number(tool.maxTokens);
          return {
            id: tool.id,
            name,
            description,
            target_model: targetModel,
            target_type: tool.targetType,
            system_prompt: tool.systemPrompt.trim() || undefined,
            prompt_template: tool.promptTemplate.trim() || undefined,
            parameters,
            max_tokens: Number.isFinite(maxTokens) && maxTokens > 0 ? Math.floor(maxTokens) : undefined,
          };
        }).filter((tool): tool is CustomOmniToolDefinition => tool !== null)
        : [];
      const components = customDraft.omniSource === 'collection'
        ? Array.from(new Set(Object.values(componentRoles).map(v => v.trim()).filter(Boolean)))
        : [];
      const omniSystemPrompt = customDraft.omniSystemPrompt.trim();
      const availableRecipeOptions = recipeOptionsForDraft(customDraft.capability, customDraft.omniSource);
      const selectedRecipeOption = availableRecipeOptions.find(option => option.value === customDraft.recipe)
        || availableRecipeOptions[0];
      if (!selectedRecipeOption) {
        setCustomError('No compatible recipe/backend is available for this capability on the connected Lemonade server.');
        return;
      }
      const selectedRecipe = selectedRecipeOption.recipe;
      const checkpoint = customDraft.checkpoint.trim();
      const extraCheckpoints: Record<string, string> = {};
      if (selectedRecipe === 'llamacpp' && supportsVisionProjectorField(customDraft.capability) && customDraft.mmproj.trim()) {
        extraCheckpoints.mmproj = customDraft.mmproj.trim();
      }
      if (selectedRecipe === 'sd-cpp') {
        if (customDraft.imageTextEncoder.trim()) extraCheckpoints.text_encoder = customDraft.imageTextEncoder.trim();
        if (customDraft.imageVae.trim()) extraCheckpoints.vae = customDraft.imageVae.trim();
      }
      const checkpoints = checkpoint && Object.keys(extraCheckpoints).length > 0
        ? { main: checkpoint, ...extraCheckpoints }
        : undefined;
      const saved = upsertCustomModel(accountSession.storageScope, {
        name: editingCustomModelName || implicitCustomModelName(customDraft.displayName, customDraft.checkpoint, customDraft.capability === 'omni' ? 'omni-model' : 'custom-model'),
        displayName: customDraft.displayName,
        checkpoint,
        checkpoints,
        mmproj: selectedRecipe === 'llamacpp' ? customDraft.mmproj : undefined,
        recipe: selectedRecipe || customDraft.recipe,
        recipeOptions: undefined,
        system_prompt: isOmniCollection && omniSystemPrompt && omniSystemPrompt !== DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE
          ? omniSystemPrompt
          : undefined,
        capability: customDraft.capability,
        maxContextWindow: undefined,
        labels: customDraft.labels.split(',').map(l => l.trim()).filter(Boolean),
        components,
        componentRoles,
        customTools,
      });
      reloadCustomModels();

      // A custom collection is a server model definition, not merely UI state.
      // Register custom component definitions first, then synchronously persist
      // the collection itself so /models exposes it immediately and restart does
      // not depend on this WebView's localStorage.
      if (isOmniCollection) {
        const savedInfo = customModelToModelInfo(saved);
        for (const componentName of getCollectionComponents(savedInfo)) {
          const componentInfo = findCurrentModel(componentName);
          if (componentInfo && (componentInfo as any).custom) {
            await api.registerModelDefinition(modelName(componentInfo), customRegistrationOptions(componentInfo));
          }
        }
        await api.registerModelDefinition(saved.name, customRegistrationOptions(savedInfo));
        const persistedModels = await api.models(true);
        const persisted = persistedModels.data.some(model => modelName(model).toLowerCase() === saved.name.toLowerCase());
        if (!persisted) {
          throw new Error(`Lemond acknowledged the collection but did not expose ${saved.name} through /api/v1/models.`);
        }
        await refresh();
      }

      setShowCustomForm(false);
      setEditingCustomModelName(null);
      setPrimaryFilter('my-models');
      setSearchQuery('');
      setSelectedDetailModelId(saved.name);
      setMobileDetailOpen(true);
      setCustomDraft(createEmptyCustomDraft());
    } catch (err) {
      setCustomError(err instanceof Error ? err.message : 'Could not save custom model.');
    }
  };

  /* ── Derived data ────────────────────────────────────────── */

  const allModels = useMemo(() => {
    const seen = new Set<string>();
    const merged: ModelInfo[] = [];
    for (const m of routerModels) {
      const name = modelName(m).toLowerCase();
      if (!name || seen.has(name)) continue;
      seen.add(name);
      merged.push(m);
    }
    for (const m of customModels) {
      const name = modelName(m).toLowerCase();
      if (!name || seen.has(name)) continue;
      seen.add(name);
      merged.push(m);
    }
    const loadedNames = new Set(loadedModels.map(lm => lm.model_name.toLowerCase()));
    for (const m of models) {
      const name = modelName(m).toLowerCase();
      if (!name || seen.has(name)) continue;
      // Hide models explicitly marked as not suggested, unless they are downloaded or loaded
      if ((m as any).suggested === false && !(m as any).downloaded && !loadedNames.has(name)) continue;
      seen.add(name);
      merged.push(m);
    }
    return merged;
  }, [routerModels, customModels, models, loadedModels]);

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

  const omniCustomToolTargetOptions = useMemo<Record<CustomOmniToolTargetType, OmniComponentOption[]>>(() => {
    const byId = new Map(allModels.map(model => [modelName(model).toLowerCase(), model] as const));
    const optionsFor = (targetType: CustomOmniToolTargetType): OmniComponentOption[] => configuredOmniToolTargetIds(customDraft, targetType).map(id => {
      const model = byId.get(id.toLowerCase());
      if (model) return omniComponentOptionFromModel(model);
      return {
        id,
        label: id,
        detail: 'configured in this collection',
        source: 'registered',
        downloaded: false,
        custom: false,
        recipe: '',
        labels: [],
      };
    });
    return {
      chat: optionsFor('chat'),
      vision: optionsFor('vision'),
      image: optionsFor('image'),
    };
  }, [allModels, customDraft.llmComponent, customDraft.visionComponent, customDraft.imageComponent]);

  const defaultCustomToolTarget = (targetType: CustomOmniToolTargetType): string => {
    const options = omniCustomToolTargetOptions[targetType];
    if (targetType === 'vision' && customDraft.visionComponent) return customDraft.visionComponent;
    if (targetType === 'image' && customDraft.imageComponent) return customDraft.imageComponent;
    if (targetType === 'chat' && customDraft.llmComponent) return customDraft.llmComponent;
    return options[0]?.id || '';
  };

  const addOmniCustomTool = (preset: OmniCustomToolPreset = 'generic') => {
    const targetType = targetTypeForOmniToolPreset(preset);
    const next = createOmniCustomToolDraft(customDraft.omniCustomTools, preset, defaultCustomToolTarget(targetType));
    handleCustomDraftChange({ omniCustomTools: [...customDraft.omniCustomTools, next] });
  };

  const addCoderReviewerPair = () => {
    const targetModel = defaultCustomToolTarget('chat');
    const coder = createOmniCustomToolDraft(customDraft.omniCustomTools, 'coder', targetModel);
    const reviewer = createOmniCustomToolDraft([...customDraft.omniCustomTools, coder], 'reviewer', targetModel);
    handleCustomDraftChange({ omniCustomTools: [...customDraft.omniCustomTools, coder, reviewer] });
  };

  const changeOmniCustomToolTargetType = (tool: OmniCustomToolDraft, targetType: CustomOmniToolTargetType) => {
    const knownDefaults = new Set([
      DEFAULT_CUSTOM_LLM_TOOL_PARAMETERS_JSON,
      DEFAULT_CUSTOM_VISION_TOOL_PARAMETERS_JSON,
      DEFAULT_CUSTOM_IMAGE_TOOL_PARAMETERS_JSON,
    ]);
    const targetOptions = omniCustomToolTargetOptions[targetType];
    const currentTargetIsCompatible = targetOptions.some(option => option.id.toLowerCase() === tool.targetModel.trim().toLowerCase());
    updateOmniCustomTool(tool.id, {
      targetType,
      targetModel: currentTargetIsCompatible ? tool.targetModel : defaultCustomToolTarget(targetType),
      parametersJson: !tool.parametersJson.trim() || knownDefaults.has(tool.parametersJson)
        ? defaultOmniToolParametersJson(targetType)
        : tool.parametersJson,
    });
  };

  const updateOmniCustomTool = (id: string, patch: Partial<OmniCustomToolDraft>) => {
    handleCustomDraftChange({
      omniCustomTools: customDraft.omniCustomTools.map(tool => tool.id === id ? { ...tool, ...patch } : tool),
    });
  };

  const removeOmniCustomTool = (id: string) => {
    handleCustomDraftChange({ omniCustomTools: customDraft.omniCustomTools.filter(tool => tool.id !== id) });
  };

  const displayLoadedModels = useMemo(
    () => withVirtualLoadedCollections(loadedModels, allModels),
    [loadedModels, allModels]
  );

  const loadedNames = useMemo(
    () => new Set(displayLoadedModels.map(m => m.model_name)),
    [displayLoadedModels]
  );

  // Local pull callbacks are lost when the user leaves/re-enters the Models
  // view, while the server-backed store keeps tracking the active download.
  const effectivePulling = useMemo(() => {
    const next = { ...pulling };
    downloadItems.forEach(item => {
      if (item.downloadType !== 'model' || !activeDownloadForModel(downloadItems, item.modelName)) return;
      if (next[item.modelName] === undefined) next[item.modelName] = item.percent;
    });
    return next;
  }, [downloadItems, pulling]);

  const pinnedNameSet = useMemo(() => new Set(pinnedModels.map(name => name.toLowerCase())), [pinnedModels]);
  const favoriteNameSet = useMemo(() => new Set(favoriteModels.map(name => name.toLowerCase())), [favoriteModels]);

  const { downloaded, available } = useMemo(() => {
    const dl: ModelInfo[] = [];
    const av: ModelInfo[] = [];
    for (const m of allModels) {
      const name = modelName(m);
      if (loadedNames.has(name)) continue;
      const hasActiveDownload = Boolean(activeDownloadForModel(downloadItems, name))
        || (isCollectionModel(m) && getCollectionComponents(m).some(component => Boolean(activeDownloadForModel(downloadItems, component))));
      const isDownloaded = !hasActiveDownload && (isCollectionModel(m)
        ? isCollectionFullyDownloaded(m, allModels)
        : Boolean((m as any).downloaded));
      if (isDownloaded) dl.push(m);
      else av.push(m);
    }
    return { downloaded: dl, available: av };
  }, [allModels, downloadItems, loadedNames]);

  const handleUpdateAllModels = useCallback(async (): Promise<UpdateAllModelsResult> => {
    const candidates = allModels.filter(model => {
      const name = modelName(model);
      if (!name || String((model as any).recipe || '').toLowerCase() === ROUTER_RECIPE) return false;
      return loadedNames.has(name)
        || (isCollectionModel(model) ? isCollectionFullyDownloaded(model, allModels) : Boolean((model as any).downloaded));
    });

    let started = 0;
    let skipped = 0;
    const errors: string[] = [];
    for (const model of candidates) {
      const name = modelName(model);
      if (pulling[name] !== undefined || activeDownloadForModel(downloadItems, name)) {
        skipped += 1;
        continue;
      }
      started += 1;
      // Queue all pulls without holding the settings panel open for potentially
      // multi-gigabyte downloads. Progress and terminal failures stay visible in
      // the normal download manager and model rows.
      void handlePull(model).catch(error => {
        console.error(`Could not start update for ${name}:`, error);
      });
    }
    return { started, skipped, errors };
  }, [allModels, downloadItems, loadedNames, pulling]);

  useEffect(() => {
    if (automaticUpdateStartedRef.current || !api.isConnected || modelsLoading) return;
    if (!automaticUpdateIsDue(globalModelSettings)) return;
    automaticUpdateStartedRef.current = true;
    void handleUpdateAllModels().finally(() => {
      const next = saveGlobalModelSettings(accountSession.storageScope, {
        ...loadGlobalModelSettings(accountSession.storageScope),
        lastAutomaticUpdateAt: new Date().toISOString(),
      });
      setGlobalModelSettings(next);
    });
  }, [accountSession.storageScope, globalModelSettings, handleUpdateAllModels, modelsLoading]);

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
  const visibleFilteredDownloaded = useMemo(() => filteredDownloaded.filter(m => !pinnedNameSet.has(modelName(m).toLowerCase())), [filteredDownloaded, pinnedNameSet]);
  const visibleFilteredAvailable = useMemo(() => filteredAvailable.filter(m => !pinnedNameSet.has(modelName(m).toLowerCase())), [filteredAvailable, pinnedNameSet]);

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
    if (showAllAvailable || searchQuery.trim() || filterTab !== 'all') return visibleFilteredAvailable;
    return visibleFilteredAvailable.slice(0, AVAILABLE_INITIAL);
  }, [visibleFilteredAvailable, showAllAvailable, searchQuery, filterTab]);
  const hiddenAvailableCount = visibleFilteredAvailable.length - visibleAvailable.length;

  const localRegistryRefs = useMemo(() => {
    const refs = new Set<string>();
    for (const model of allModels) {
      refs.add(modelName(model).toLowerCase());
      const checkpoint = String((model as any).checkpoint || '').split(':')[0].trim().toLowerCase();
      if (checkpoint) refs.add(checkpoint);
      const mainCheckpoint = String((model as any).checkpoints?.main || '').split(':')[0].trim().toLowerCase();
      if (mainCheckpoint) refs.add(mainCheckpoint);
    }
    return refs;
  }, [allModels]);

  const filterRemoteResults = useCallback((provider: ModelRegistryProvider, results: HFModelResult[]) => {
    if (!providerEnabled[provider] || searchQuery.trim().length < 2 || primaryFilter !== 'all') return [];
    return results.filter(result => {
      if (localRegistryRefs.has(result.id.toLowerCase())) return false;
      const info = remoteResultAsModelInfo(result, remoteVariants[providerKey(provider, result.id)]);
      if (!modelMatchesFilter(info, filterTab)) return false;
      if (!modelMatchesCapabilityTags(info, capabilityFilter)) return false;
      if (!modelMatchesBackend(info, backendFilter)) return false;
      if (!modelMatchesTag(info, tagFilter)) return false;
      return true;
    });
  }, [providerEnabled, searchQuery, primaryFilter, localRegistryRefs, remoteVariants, filterTab, capabilityFilter, backendFilter, tagFilter]);

  const filteredHfResults = useMemo(
    () => filterRemoteResults('huggingface', hfResults),
    [filterRemoteResults, hfResults],
  );
  const filteredModelScopeResults = useMemo(
    () => filterRemoteResults('modelscope', modelScopeResults),
    [filterRemoteResults, modelScopeResults],
  );

  // Rough check: does any local model match the current search query?
  // Used to decide whether to elevate the remote-provider zones (top, prominent)
  // or keep them inline at the bottom with an anchor bar.
  const hasLocalMatches = useMemo(() => {
    const q = searchQuery.trim().toLowerCase();
    if (q.length < 2) return true;
    return allModels.some(m => {
      const mName = modelName(m).toLowerCase();
      const disp = String(m.display_name || '').toLowerCase();
      const recipe = String((m as any).recipe || '').toLowerCase();
      const labels = (m.labels || []).join(' ').toLowerCase();
      return `${mName} ${disp} ${recipe} ${labels}`.includes(q);
    });
  }, [allModels, searchQuery]);
  const selectedDetailModel = selectedDetailModelId
    ? (allModels.find(m => modelName(m) === selectedDetailModelId) ?? null)
    : null;
  const selectedDetailIsCustom = Boolean(selectedDetailModel && modelIsCustom(selectedDetailModel));
  const showCustomEditor = showCustomForm || (primaryFilter === 'my-models' && !selectedDetailIsCustom);

  const handlePrimaryFilterChange = (next: PrimaryFilter) => {
    setPrimaryFilter(next);
    mobileRail.close();
    if (next === 'my-models') {
      if (!selectedDetailIsCustom) openCustomForm('model');
      else closeCustomForm();
      return;
    }
    closeCustomForm();
  };

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
            {capability === 'tts' && activeTtsModelName === name && (
              <div className="detail__field detail__field--wide detail__tts-playback">
                <span className="detail__label">Speech playback</span>
                <div className="detail__tts-controls">
                  <div className="detail__tts-mode" role="group" aria-label="Speech playback mode">
                    <span>Mode</span>
                    <span className="detail__tts-mode-buttons">
                      {(['demand', 'always'] as TtsPlaybackMode[]).map(mode => (
                        <button
                          key={mode}
                          type="button"
                          className={`detail__tts-mode-button${ttsPlaybackSettings.playbackMode === mode ? ' detail__tts-mode-button--active' : ''}`}
                          onClick={() => setTtsPlaybackMode(mode)}
                          aria-pressed={ttsPlaybackSettings.playbackMode === mode}
                        >
                          {mode === 'demand' ? 'On demand' : 'Always'}
                        </button>
                      ))}
                    </span>
                  </div>
                  <label className="detail__tts-toggle">
                    <span>Also read user text</span>
                    <input
                      type="range"
                      min={0}
                      max={1}
                      step={1}
                      value={ttsPlaybackSettings.speakUserText ? 1 : 0}
                      onChange={e => setSpeakUserText(Number(e.target.value) === 1)}
                      aria-label="Also read user text"
                    />
                    <strong>{ttsPlaybackSettings.speakUserText ? 'On' : 'Off'}</strong>
                  </label>
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
    const selectable = canSelectInComposer(m) || ['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(cap);
    const activePreset = activePresetForName(m.model_name);
    const isPinned = pinnedNameSet.has(m.model_name.toLowerCase());
    return (
      <div className={`row row--running${isActive ? ' row--active' : ''}`} key={m.model_name}>
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
            {renderPinAndSpeechControl(m.model_name, isPinned, cap)}
            <CopyInlineButton text={m.model_name} title={`Copy model ID: ${m.model_name}`} />
            <span className="row__status-pill row__status-pill--running">
              <span className="row__pulse" /> {isActive ? `Active ${capabilityLabel(cap)} mode` : 'Running'}
            </span>
            {selectable && !isActive && (
              <button className="row__action" aria-label={`Use ${m.model_name} in ${capabilityLabel(cap)} mode`} onClick={(e) => { e.stopPropagation(); onModelSelect(m.model_name); }}>
                Use in {capabilityLabel(cap)} mode
              </button>
            )}
            <button
              className="row__action row__action--unload"
              aria-label={loadingModel === m.model_name ? `Working on ${m.model_name}…` : `Unload ${m.model_name}`}
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
              aria-label={`Delete ${m.model_name}`}
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
    const activeDownload = activeDownloadForModel(downloadItems, name);
    const pullPercent = activeDownload?.percent ?? pulling[name];
    const isPulling = pullPercent !== undefined;
    const activePreset = activePresetForName(name);
    const cap = capabilityFromModelInfo(m);
    const isPinned = pinnedNameSet.has(name.toLowerCase());
    const rowCtx = contextSizeForDisplay(m, undefined, serverDefaultCtxSize);

    return (
      <div className={`row${expandedModel === name ? ' row--expanded' : ''}`} key={name}>
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
            {renderPinAndSpeechControl(name, isPinned, cap)}
            <CopyInlineButton text={name} title={`Copy model ID: ${name}`} />
            {isPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPercent}%` }} />
                </div>
                <span className="row__progress-text">{pullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(e) => { e.stopPropagation(); handleCancelPull(name); }}
                  title={`Cancel download of ${name}`}
                  aria-label={`Cancel download of ${name}`}
                ><Icon name="x" size={13} /></button>
              </div>
            ) : isDownloaded ? (
              <>
                <span className="row__status-pill row__status-pill--ready">Ready</span>
                <button
                  className="row__action"
                  aria-label={isLoading ? `Loading ${name}…` : `Load ${name}`}
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
                  aria-label={`Delete ${name}`}
                >
                  <Icon name="x" size={14} />
                </button>
              </>
            ) : (
              <>
                <button
                  className="row__action row__action--download"
                  aria-label={`Download ${name}`}
                  onClick={(e) => { e.stopPropagation(); handlePull(m); }}
                  disabled={isPulling}
                >
                  <Icon name="download" size={13} /> Download
                </button>
                <button
                  className="row__action"
                  aria-label={`Get and load ${name}`}
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

  const renderRemoteRow = (provider: ModelRegistryProvider, result: HFModelResult) => {
    const key = providerKey(provider, result.id);
    const providerMeta = PROVIDER_META[provider];
    const isExpanded = expandedRemoteModel === key;
    const pipelineTag = result.pipeline_tag || '';
    const variants = remoteVariants[key];
    const displayTags = Array.from(new Set([
      ...(result.tags || []),
      ...(variants?.suggested_labels || []),
    ]))
      .filter(tag => !['gguf', 'transformers', 'pytorch', 'safetensors'].includes(tag.toLowerCase()))
      .slice(0, 6);
    const remotePull = activeRemotePull(provider, result.id, variants);
    const pullPercent = remotePull?.percent;
    const isPulling = pullPercent !== undefined;
    const isLoadingVariants = remoteVariantsLoading[key] || false;
    const recipeBadge = variants ? (RECIPE_BADGES[variants.recipe] || variants.recipe) : '';

    const handleExpand = () => {
      const next = isExpanded ? null : key;
      setExpandedRemoteModel(next);
      if (next) void fetchRemoteVariants(provider, result.id);
      setSelectedRemoteModel(result);
      setSelectedRemoteProvider(provider);
      setSelectedDetailModelId(null);
      setMobileDetailOpen(true);
    };

    return (
      <div className={`row row--remote row--${provider} row--${provider === 'huggingface' ? 'hf' : 'modelscope'}${isExpanded ? ' row--expanded' : ''}`} key={key}>
        <div className="row__summary">
          <button type="button" className="row__content" onClick={handleExpand} aria-expanded={isExpanded}>
            <div className="row__main">
              <div className={`row__icon row__icon--${provider}`}><Icon name="cloud" size={18} /></div>
              <div className="row__text">
                <span className="row__name-wrap"><span className="row__name">{result.id}</span></span>
                <span className="row__sub">
                  {recipeBadge ? `${recipeBadge} · ` : ''}{pipelineTag && `${pipelineTag} · `}
                  {formatDownloads(result.downloads)} downloads · {formatDownloads(result.likes)} likes
                </span>
                {displayTags.length > 0 && (
                  <div className="row__labels">
                    {displayTags.map(tag => (
                      <span key={tag} className={`row__label row__label--${provider}`}>{tag}</span>
                    ))}
                  </div>
                )}
              </div>
            </div>
            <span className="row__expand">{isExpanded ? '▾' : '▸'}</span>
          </button>
          <div className="row__right">
            <CopyInlineButton text={result.id} title={`Copy repository name: ${result.id}`} />
            {isPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPercent}%` }} />
                </div>
                <span className="row__progress-text">{pullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(event) => { event.stopPropagation(); void handleCancelRemotePull(provider, result.id); }}
                  title={`Cancel download of ${result.id}`}
                  aria-label={`Cancel download of ${result.id}`}
                ><Icon name="x" size={13} /></button>
              </div>
            ) : (
              <button
                className="row__action row__action--download"
                aria-label={`Download ${result.id}`}
                onClick={(event) => { event.stopPropagation(); handleExpand(); }}
                title="Expand to pick a variant to download"
              >
                <Icon name="download" size={13} /> Download
              </button>
            )}
            <a
              className="row__action row__action--hf-link"
              href={providerMeta.url(result.id)}
              target="_blank"
              rel="noopener noreferrer"
              onClick={event => event.stopPropagation()}
            >
              View
            </a>
          </div>
        </div>

        {isExpanded && (
          <div className={`row__detail row__detail--remote row__detail--${provider}`}>
            <div className="detail__grid">
              <div className="detail__meta">
                <div className="detail__field">
                  <span className="detail__label">Provider</span>
                  <span className="detail__value">{providerMeta.label}</span>
                </div>
                <div className="detail__field">
                  <span className="detail__label">Repository</span>
                  <span className="detail__value detail__value--mono">{result.id}</span>
                </div>
                {pipelineTag && (
                  <div className="detail__field">
                    <span className="detail__label">Pipeline</span>
                    <span className="detail__value">{pipelineTag}</span>
                  </div>
                )}
                {variants && (
                  <>
                    <div className="detail__field">
                      <span className="detail__label">Backend</span>
                      <span className="detail__value">{RECIPE_BADGES[variants.recipe] || variants.recipe}</span>
                    </div>
                    {variants.suggested_labels.length > 0 && (
                      <div className="detail__field">
                        <span className="detail__label">Capabilities</span>
                        <span className="detail__value">{variants.suggested_labels.join(', ')}</span>
                      </div>
                    )}
                  </>
                )}
              </div>
              <div className="detail__source">
                {isLoadingVariants && (
                  <div className="detail__field"><span className="detail__label">Loading variants…</span></div>
                )}
                {variants && variants.variants.length > 0 && (
                  <div className="detail__field">
                    <span className="detail__label">Variants — pick one to download</span>
                    <div className="hf-detail__gguf-list">
                      {variants.variants.map(variant => (
                        <button
                          key={variant.name}
                          className="hf-detail__gguf-btn"
                          aria-label={`Download ${variant.name} from ${result.id}`}
                          disabled={isPulling}
                          onClick={() => void handleRemotePull(provider, result.id, variant.name, variants.recipe)}
                        >
                          <span className="hf-detail__gguf-name">
                            {variant.name}{variant.sharded ? ' (sharded)' : ''}
                          </span>
                          <span className="hf-detail__gguf-size">{formatBytes(variant.size_bytes)}</span>
                          <span className="hf-detail__gguf-action">Download</span>
                        </button>
                      ))}
                    </div>
                  </div>
                )}
                <a className="detail__hf-link" href={providerMeta.url(result.id)} target="_blank" rel="noopener noreferrer">
                  View on {providerMeta.label}
                </a>
              </div>
            </div>
          </div>
        )}
      </div>
    );
  };


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
  const searchActive = searchQuery.trim().length >= 2;
  const hasHuggingFaceActivity = searchActive && primaryFilter === 'all' && providerEnabled.huggingface;
  const hasModelScopeActivity = searchActive && primaryFilter === 'all' && providerEnabled.modelscope;
  const hasRemoteActivity = hasHuggingFaceActivity || hasModelScopeActivity;
  const remoteResultCount = filteredHfResults.length + filteredModelScopeResults.length;
  const providerCounts: Record<ModelRegistryProvider, number> = {
    huggingface: hasHuggingFaceActivity ? filteredHfResults.length : 0,
    modelscope: hasModelScopeActivity ? filteredModelScopeResults.length : 0,
  };
  const showManagerEmpty = !modelsLoading
    && filteredRunning.length === 0
    && visibleFilteredDownloaded.length === 0
    && visibleFilteredAvailable.length === 0
    && !hasRemoteActivity;
  const isCustomOmniCollectionDraft = customDraft.capability === 'omni' && customDraft.omniSource === 'collection';
  const customFormTitle = editingCustomModelName ? 'Edit Omni collection' : (isCustomOmniCollectionDraft ? 'Custom Omni collection' : 'Custom model');
  const customRecipeOptions = recipeOptionsForDraft(customDraft.capability, customDraft.omniSource);
  const selectedCustomRecipe = customRecipeOptions.find(option => option.value === customDraft.recipe) || customRecipeOptions[0];
  const selectedCustomRecipeName = selectedCustomRecipe?.recipe || optionRecipe(customDraft.recipe);
  const selectedCustomRecipeSuggestion = CUSTOM_RECIPE_SUGGESTIONS[selectedCustomRecipeName];
  const primaryCheckpointPlaceholder = selectedCustomRecipeSuggestion?.checkpoint || 'org/model:Q4_K_M.gguf or /path/to/model.gguf';
  const mmprojCheckpointExample = selectedCustomRecipeSuggestion?.extraCheckpoints?.find(example => example.key === 'mmproj');
  const textEncoderCheckpointExample = selectedCustomRecipeSuggestion?.extraCheckpoints?.find(example => example.key === 'text_encoder');
  const vaeCheckpointExample = selectedCustomRecipeSuggestion?.extraCheckpoints?.find(example => example.key === 'vae');
  const showLlamacppMmprojField = !isCustomOmniCollectionDraft
    && selectedCustomRecipeName === 'llamacpp'
    && supportsVisionProjectorField(customDraft.capability);
  const showImageExtraCheckpointFields = !isCustomOmniCollectionDraft && selectedCustomRecipeName === 'sd-cpp';
  const pinnedVisibleModels = useMemo(() => {
    if (pinnedModels.length === 0) return [];
    const map = new Map(allModels.map(m => [modelName(m).toLowerCase(), m] as const));
    return pinnedModels
      .map(name => map.get(name.toLowerCase()))
      .filter((m): m is ModelInfo => Boolean(m) && !loadedNames.has(modelName(m)));
  }, [allModels, pinnedModels, loadedNames]);
  const totalDownloaded = downloaded.length + displayLoadedModels.length;
  const totalPulling = new Set([
    ...Object.keys(pulling),
    ...downloadItems.filter(item => item.downloadType === 'model' && (item.status === 'downloading' || item.status === 'paused' || item.running === true)).map(item => item.modelName),
  ]).size;
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


  const toggleProvider = (provider: ModelRegistryProvider) => {
    setProviderEnabled(prev => ({ ...prev, [provider]: !prev[provider] }));
    if (selectedRemoteModel && selectedRemoteProvider === provider) {
      setSelectedRemoteModel(null);
      setExpandedRemoteModel(null);
      setMobileDetailOpen(false);
    }
  };

  const renderProviderZone = (provider: ModelRegistryProvider) => {
    const meta = PROVIDER_META[provider];
    const loading = provider === 'huggingface' ? hfLoading : modelScopeLoading;
    const error = provider === 'huggingface' ? hfError : modelScopeError;
    const results = provider === 'huggingface' ? filteredHfResults : filteredModelScopeResults;
    const enabled = providerEnabled[provider];
    if (!searchActive || !enabled) return null;
    return (
      <section
        className={`zone zone--registry zone--${provider === 'huggingface' ? 'hf' : 'modelscope'}`}
        aria-label={`${meta.label} search results`}
        data-provider={provider}
      >
        <div className="zone__head">
          <span className={`zone__dot zone__dot--${provider === 'huggingface' ? 'hf' : 'modelscope'}`} aria-hidden="true" />
          <span className="zone__title">{meta.compactLabel}</span>
          {!loading && <span className="zone__count">{results.length}</span>}
          <span className="zone__rule" />
        </div>
        {loading ? (
          <div className="hf-zone__loading" role="status" aria-live="polite">
            <span className="hf-zone__spinner" aria-hidden="true" />
            <span>Searching {meta.label}…</span>
          </div>
        ) : error ? (
          <div className="hf-zone__empty hf-zone__empty--error">
            <Icon name="alert" size={16} />
            <span>{meta.label} search is unavailable: {error}</span>
          </div>
        ) : results.length === 0 ? (
          <div className="hf-zone__empty">
            <Icon name="cloud-off" size={16} />
            <span>No compatible {meta.label} models match the active filters.</span>
          </div>
        ) : (
          results.map(result => renderRemoteRow(provider, result))
        )}
      </section>
    );
  };

  const renderRegistryZones = () => !hasRemoteActivity ? null : (
    <div className="registry-zones" aria-label="Remote model search results">
      {renderProviderZone('huggingface')}
      {renderProviderZone('modelscope')}
    </div>
  );
  return (
    <div
      className={`manager manager--detail${mobileDetailOpen ? ' manager--detail-mobile-open' : ''}${mobileRail.isOpen ? ' manager--nav-open' : ''}${navRailCollapsed ? ' workspace--rail-collapsed' : ''}`}
      style={modelDetailLayoutStyle}
    >
      {mobileRail.isOpen && <div className="workspace-mobile-rail-backdrop" onClick={mobileRail.close} aria-hidden="true" />}
      <WorkspaceMobileMenuButton
        menuLabel="Open model filters"
        panelId="model-nav-rail"
        expanded={mobileRail.isOpen}
        onClick={mobileRail.toggle}
        triggerRef={mobileRail.triggerRef}
      />

      {/* Left rail: navigation / filter dimensions */}
      <ModelNavRail
        allModels={allModels}
        loadedNames={loadedNames}
        pinnedNames={pinnedNameSet}
        favoriteNames={favoriteNameSet}
        primaryFilter={primaryFilter}
        onPrimaryFilterChange={handlePrimaryFilterChange}
        categoryFilter={filterTab}
        onCategoryFilterChange={(f) => { setFilterTab(f); mobileRail.close(); }}
        backendFilter={backendFilter}
        onBackendFilterChange={(backend) => { setBackendFilter(backend); mobileRail.close(); }}
        tagFilter={tagFilter}
        onTagFilterChange={(t) => { setTagFilter(t); mobileRail.close(); }}
        providerEnabled={providerEnabled}
        providerCounts={providerCounts}
        onToggleProvider={toggleProvider}
        storageInfo={storageInfo}
        collapsed={navRailCollapsed}
        onToggleCollapsed={() => setNavRailCollapsed(value => !value)}
        mobileOpen={mobileRail.isOpen}
        onMobileClose={mobileRail.close}
        railRef={mobileRail.panelRef}
      />

      {/* Middle panel: searchable/filterable model list */}
      <ModelListPanel
        allModels={allModels}
        loadedNames={loadedNames}
        pulling={pulling}
        downloadItems={downloadItems}
        selectedModelId={selectedDetailModelId}
        onSelectModel={(id) => {
          setSelectedDetailModelId(id);
          setSelectedRemoteModel(null);
          setMobileDetailOpen(true);
          if (showCustomForm) closeCustomForm();
          if (showRouterEditor) closeRouterEditor();
          if (showGlobalSettings) closeGlobalSettings();
        }}
        searchQuery={searchQuery}
        onSearchChange={setSearchQuery}
        filterTab={filterTab}
        onFilterChange={setFilterTab}
        capabilityFilter={capabilityFilter}
        onCapabilityFilterChange={setCapabilityFilter}
        primaryFilter={primaryFilter}
        backendFilter={backendFilter}
        tagFilter={tagFilter}
        searchInputRef={searchRef}
        onOpenRouter={() => openRouterEditor(selectedDetailModel)}
        onOpenGlobalSettings={openGlobalSettings}
        onOpenCustomModels={() => openCustomForm('model')}
        pinnedNames={pinnedNameSet}
        onTogglePin={togglePinnedModel}
        favoriteNames={favoriteNameSet}
        registryZoneTop={hasRemoteActivity && !hasLocalMatches ? renderRegistryZones() : undefined}
        registryZone={hasRemoteActivity && hasLocalMatches ? renderRegistryZones() : undefined}
        registryResultCount={remoteResultCount}
      />

      <div
        className="manager__model-list-resizer"
        role="separator"
        aria-orientation="vertical"
        aria-label="Resize model list panel"
        aria-valuemin={MODEL_LIST_MIN_WIDTH}
        aria-valuemax={MODEL_LIST_MAX_WIDTH}
        aria-valuenow={modelListWidth}
        tabIndex={0}
        onPointerDown={handleModelListResizeStart}
        onKeyDown={handleModelListResizeKeyDown}
      />

      {/* Right panel: global settings, router editor, custom form, or model detail */}
      {showGlobalSettings ? (
        <div className="manager__detail-form-panel manager__detail-form-panel--global-settings">
          <GlobalModelSettingsPanel
            scope={accountSession.storageScope}
            models={allModels}
            loadedModels={loadedModels}
            pinnedModels={pinnedModels}
            onTogglePin={togglePinnedModel}
            onUpdateAllModels={handleUpdateAllModels}
            onClose={closeGlobalSettings}
          />
        </div>
      ) : showRouterEditor ? (
        <div className="manager__detail-form-panel manager__detail-form-panel--router">
          <RouterEditorPanel
            models={allModels}
            scope={accountSession.storageScope}
            initialModel={routerEditorModel}
            onRegister={handleRegisterRouter}
            onSaved={handleRouterSaved}
            onDeleted={handleDeleteRouterDefinition}
            onClose={closeRouterEditor}
          />
        </div>
      ) : showCustomEditor ? (
        <div className="manager__detail-form-panel">
          <section className="zone custom-model-form" aria-label={customFormTitle}>
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">{customFormTitle}</span>
              {isCustomOmniCollectionDraft && <span className="zone__count">collection wrapper</span>}
              <span className="zone__rule" />
            </div>
            <div className="custom-model-form__toolbar">
              {editingCustomModelName ? (
                <span className="custom-model-form__editing-badge">Editing saved collection</span>
              ) : (
                <div className="custom-model-form__mode-switch" role="group" aria-label="Custom model type">
                  <button
                    type="button"
                    className={!isCustomOmniCollectionDraft ? 'is-active' : ''}
                    aria-pressed={!isCustomOmniCollectionDraft}
                    onClick={() => openCustomForm('model')}
                  >
                    Custom model
                  </button>
                  <button
                    type="button"
                    className={isCustomOmniCollectionDraft ? 'is-active' : ''}
                    aria-pressed={isCustomOmniCollectionDraft}
                    onClick={() => openCustomForm('omni-collection')}
                  >
                    Omni collection
                  </button>
                </div>
              )}
              <div className="custom-model-form__io-actions">
                <button className="btn btn--ghost btn--tiny" type="button" onClick={handleExportCustomModels}>Export JSON</button>
                <button className="btn btn--ghost btn--tiny" type="button" onClick={() => customJsonInputRef.current?.click()}>Import JSON</button>
              </div>
            </div>
            <form className="custom-model-form__grid" onSubmit={handleSaveCustomModel}>
              <label className="custom-model-form__field">Name
                <input
                  value={customDraft.displayName}
                  onChange={e => handleCustomDraftChange({ displayName: e.target.value })}
                  placeholder={isCustomOmniCollectionDraft ? 'My Omni collection' : 'My custom model'}
                />
              </label>
              <label className="custom-model-form__field">Extra labels
                <input value={customDraft.labels} onChange={e => handleCustomDraftChange({ labels: e.target.value })} placeholder="tool-calling, reasoning" />
              </label>

              {!isCustomOmniCollectionDraft && (
                <>
                  <label className="custom-model-form__field">Capability
                    <select value={customDraft.capability} onChange={e => {
                      const nextCapability = e.target.value as CustomModelCapability;
                      handleCustomDraftChange({
                        capability: nextCapability,
                        omniSource: 'single',
                        recipe: defaultRecipeForCapability(nextCapability, 'single'),
                      });
                    }}>
                      {CUSTOM_CAPABILITIES.map(c => <option key={c.value} value={c.value}>{c.label}</option>)}
                    </select>
                  </label>
                  <label className="custom-model-form__field">Recipe/backend
                    <select value={selectedCustomRecipe?.value || ''} onChange={e => handleCustomDraftChange({ recipe: e.target.value })} disabled={customRecipeOptions.length === 0}>
                      {customRecipeOptions.length === 0
                        ? <option value="">No compatible backend available</option>
                        : customRecipeOptions.map(option => <option key={option.value} value={option.value}>{option.label}</option>)}
                    </select>
                  </label>
                  <label className="custom-model-form__field custom-model-form__wide">Checkpoint, Hugging Face repo, or local path
                    {selectedCustomRecipeSuggestion && (
                      <InlineCheckpointExample
                        checkpoint={selectedCustomRecipeSuggestion.checkpoint}
                        note={selectedCustomRecipeSuggestion.note}
                      />
                    )}
                    <input
                      value={customDraft.checkpoint}
                      onChange={e => handleCustomDraftChange({ checkpoint: e.target.value })}
                      placeholder={primaryCheckpointPlaceholder}
                    />
                  </label>
                  {showLlamacppMmprojField && (
                    <label className="custom-model-form__field custom-model-form__wide">Vision projector (mmproj)
                      <InlineCheckpointExample
                        checkpoint={mmprojCheckpointExample?.checkpoint || 'repo/model:mmproj-model-f16.gguf'}
                        note={mmprojCheckpointExample?.note}
                      />
                      <input
                        value={customDraft.mmproj}
                        onChange={e => handleCustomDraftChange({ mmproj: e.target.value })}
                        placeholder="Optional"
                      />
                    </label>
                  )}
                  {showImageExtraCheckpointFields && (
                    <>
                      <label className="custom-model-form__field">Text encoder checkpoint
                        <InlineCheckpointExample
                          checkpoint={textEncoderCheckpointExample?.checkpoint || 'repo/text-encoder:model.safetensors'}
                          note={textEncoderCheckpointExample?.note}
                        />
                        <input
                          value={customDraft.imageTextEncoder}
                          onChange={e => handleCustomDraftChange({ imageTextEncoder: e.target.value })}
                          placeholder="Optional"
                        />
                      </label>
                      <label className="custom-model-form__field">VAE checkpoint
                        <InlineCheckpointExample
                          checkpoint={vaeCheckpointExample?.checkpoint || 'repo/vae:model.safetensors'}
                          note={vaeCheckpointExample?.note}
                        />
                        <input
                          value={customDraft.imageVae}
                          onChange={e => handleCustomDraftChange({ imageVae: e.target.value })}
                          placeholder="Optional"
                        />
                      </label>
                    </>
                  )}
                </>
              )}

              {isCustomOmniCollectionDraft && (
                <>
                  <div className="custom-model-form__hint custom-model-form__wide">
                    Choose the models that make up this collection. Only the planner LLM is required.
                  </div>
                  <OmniComponentPicker role="llm" value={customDraft.llmComponent} options={omniComponentOptions.llm} onChange={value => updateOmniComponent('llm', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="vision" value={customDraft.visionComponent} options={omniComponentOptions.vision} onChange={value => updateOmniComponent('vision', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="image" value={customDraft.imageComponent} options={omniComponentOptions.image} onChange={value => updateOmniComponent('image', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="edit" value={customDraft.editComponent} options={omniComponentOptions.edit} onChange={value => updateOmniComponent('edit', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="transcription" value={customDraft.transcriptionComponent} options={omniComponentOptions.transcription} onChange={value => updateOmniComponent('transcription', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="speech" value={customDraft.speechComponent} options={omniComponentOptions.speech} onChange={value => updateOmniComponent('speech', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />

                  <details className="custom-model-form__advanced custom-model-form__wide">
                    <summary>
                      <span>Advanced settings</span>
                      <small>System prompt and custom model tools</small>
                    </summary>
                    <div className="custom-model-form__advanced-body">
                      <label className="custom-model-form__field custom-model-form__wide custom-model-form__textarea-field">Omni tool system prompt
                        <textarea
                          value={customDraft.omniSystemPrompt}
                          onChange={e => handleCustomDraftChange({ omniSystemPrompt: e.target.value })}
                          rows={8}
                          spellCheck={false}
                        />
                      </label>
                      <div className="custom-model-form__prompt-actions custom-model-form__wide">
                        <button className="btn btn--ghost btn--tiny" type="button" onClick={() => handleCustomDraftChange({ omniSystemPrompt: DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE })}>Reset to default</button>
                      </div>
                      <div className="custom-model-form__tools custom-model-form__wide">
                        <div className="custom-model-form__section-head">
                          <div>
                            <strong>Custom model tools</strong>
                            <span>Add an editable example, choose its endpoint, then select one of the models configured in this collection.</span>
                          </div>
                          <div className="custom-model-form__section-actions">
                            <button className="btn btn--ghost btn--tiny" type="button" onClick={() => addOmniCustomTool('generic')}>+ LLM example</button>
                            <button className="btn btn--ghost btn--tiny" type="button" onClick={() => addOmniCustomTool('vision')}>+ Vision example</button>
                            <button className="btn btn--ghost btn--tiny" type="button" onClick={() => addOmniCustomTool('image')}>+ Image example</button>
                            <button className="btn btn--ghost btn--tiny" type="button" onClick={addCoderReviewerPair}>+ Coding pair</button>
                          </div>
                        </div>
                        {customDraft.omniCustomTools.length === 0 ? (
                          <div className="custom-model-form__empty-tools">No custom model tools configured. The buttons above insert working examples that you can rename and adapt.</div>
                        ) : customDraft.omniCustomTools.map((tool, index) => {
                          const targetOptions = omniCustomToolTargetOptions[tool.targetType];
                          const selectedTarget = targetOptions.find(option => option.id.toLowerCase() === tool.targetModel.trim().toLowerCase())?.id || '';
                          const promptDriven = tool.targetType === 'chat' || tool.targetType === 'vision';
                          return (
                            <div className="custom-model-form__tool-card" key={tool.id}>
                              <div className="custom-model-form__tool-card-head">
                                <div>
                                  <strong>Tool {index + 1}</strong>
                                  <small>Editable example · {tool.targetType === 'chat' ? 'Chat / LLM' : tool.targetType === 'vision' ? 'Vision' : 'Image generation'}</small>
                                </div>
                                <button className="btn btn--ghost btn--tiny" type="button" onClick={() => removeOmniCustomTool(tool.id)}>Remove</button>
                              </div>
                              <label className="custom-model-form__field">Tool name
                                <input
                                  value={tool.name}
                                  onChange={e => updateOmniCustomTool(tool.id, { name: sanitizeOmniToolName(e.target.value) })}
                                  placeholder="ask_coder"
                                />
                              </label>
                              <label className="custom-model-form__field">Execution type
                                <select
                                  value={tool.targetType}
                                  onChange={e => changeOmniCustomToolTargetType(tool, e.target.value as CustomOmniToolTargetType)}
                                >
                                  <option value="chat">Chat / LLM</option>
                                  <option value="vision">Vision LLM</option>
                                  <option value="image">Image generation</option>
                                </select>
                                <small>Selects the Lemonade endpoint used for this model.</small>
                              </label>
                              <label className="custom-model-form__field custom-model-form__wide">Target model
                                <select
                                  value={selectedTarget}
                                  onChange={e => updateOmniCustomTool(tool.id, { targetModel: e.target.value })}
                                  disabled={targetOptions.length === 0}
                                >
                                  <option value="">{targetOptions.length ? 'Select a configured target' : 'Configure a compatible collection component first'}</option>
                                  {targetOptions.map(option => (
                                    <option key={option.id} value={option.id}>{option.label}</option>
                                  ))}
                                </select>
                                <small>Only models configured in this Omni collection are available as targets.</small>
                              </label>
                              <label className="custom-model-form__field custom-model-form__wide">Description
                                <input
                                  value={tool.description}
                                  onChange={e => updateOmniCustomTool(tool.id, { description: e.target.value })}
                                  placeholder="When should the planner use this tool?"
                                />
                              </label>
                              {promptDriven && (
                                <>
                                  <label className="custom-model-form__field custom-model-form__wide custom-model-form__textarea-field custom-model-form__textarea-field--compact">Target system prompt
                                    <textarea
                                      value={tool.systemPrompt}
                                      onChange={e => updateOmniCustomTool(tool.id, { systemPrompt: e.target.value })}
                                      rows={4}
                                      spellCheck={false}
                                    />
                                  </label>
                                  <label className="custom-model-form__field custom-model-form__wide custom-model-form__textarea-field custom-model-form__textarea-field--compact">Target user prompt template
                                    <textarea
                                      value={tool.promptTemplate}
                                      onChange={e => updateOmniCustomTool(tool.id, { promptTemplate: e.target.value })}
                                      rows={4}
                                      spellCheck={false}
                                    />
                                  </label>
                                </>
                              )}
                              <label className="custom-model-form__field custom-model-form__wide custom-model-form__textarea-field custom-model-form__textarea-field--compact">Tool argument schema JSON
                                <textarea
                                  value={tool.parametersJson}
                                  onChange={e => updateOmniCustomTool(tool.id, { parametersJson: e.target.value })}
                                  rows={5}
                                  spellCheck={false}
                                />
                                <small>This editable schema tells the planner which arguments it may send.</small>
                              </label>
                              {promptDriven && (
                                <label className="custom-model-form__field">Max tokens
                                  <input
                                    value={tool.maxTokens}
                                    inputMode="numeric"
                                    onChange={e => updateOmniCustomTool(tool.id, { maxTokens: e.target.value.replace(/[^0-9]/g, '') })}
                                    placeholder="Optional"
                                  />
                                </label>
                              )}
                            </div>
                          );
                        })}
                      </div>
                    </div>
                  </details>
                </>
              )}
              {customError && <div className="custom-model-form__error"><Icon name="alert" size={14} /> {customError}</div>}
              <div className="custom-model-form__actions">
                <button className="btn btn--primary" type="submit" disabled={customRecipeOptions.length === 0}>Save {isCustomOmniCollectionDraft ? 'Omni collection' : 'custom model'}</button>
                <button className="btn btn--ghost" type="button" onClick={closeCustomForm}>Cancel</button>
              </div>
            </form>
          </section>
          <input
            ref={customJsonInputRef}
            className="hidden-file-input"
            type="file"
            accept="application/json,.json"
            onChange={e => { void handleImportCustomModels(e.target.files?.[0]); }}
          />
          {customJsonNotice && <div className="manager__inline-notice">{customJsonNotice}</div>}
        </div>
      ) : (
        <ModelDetailPanel
          model={selectedDetailModel}
          loadedModel={selectedDetailModelId
            ? (displayLoadedModels.find(m => m.model_name === selectedDetailModelId) ?? null)
            : null}
          loadingModel={loadingModel}
          pulling={effectivePulling}
          loadError={loadError}
          onLoad={handleLoad}
          onUnload={handleUnload}
          onReloadModel={handleReloadModel}
          onPull={handlePull}
          onPullAndLoad={handlePullAndLoad}
          onDelete={handleDelete}
          onCancelPull={handleCancelPull}
          serverDefaultCtxSize={serverDefaultCtxSize}
          isFavorite={selectedDetailModelId ? favoriteNameSet.has(selectedDetailModelId.toLowerCase()) : false}
          onToggleFavorite={toggleFavoriteModel}
          onEditCustomCollection={openCustomCollectionEditor}
          noModelsAvailable={allModels.length === 0}
          hfModel={selectedRemoteModel}
          hfProvider={selectedRemoteProvider}
          hfVariants={selectedRemoteModel ? remoteVariants[providerKey(selectedRemoteProvider, selectedRemoteModel.id)] : undefined}
          onFetchHfVariants={(modelId) => { void fetchRemoteVariants(selectedRemoteProvider, modelId); }}
          onHfPull={(modelId, variantName, recipe) => { void handleRemotePull(selectedRemoteProvider, modelId, variantName, recipe); }}
          pullingHf={selectedRemoteModel
            ? (() => {
              const pull = activeRemotePull(
                selectedRemoteProvider,
                selectedRemoteModel.id,
                remoteVariants[providerKey(selectedRemoteProvider, selectedRemoteModel.id)],
              );
              return pull ? { [selectedRemoteModel.id]: pull.percent } : {};
            })()
            : {}}
          onCancelHfPull={(modelId) => { void handleCancelRemotePull(selectedRemoteProvider, modelId); }}
          onBack={() => {
            setMobileDetailOpen(false);
            if (selectedDetailModelId) {
              requestAnimationFrame(() => {
                const sel = document.querySelector<HTMLElement>(`[data-model-id="${CSS.escape(selectedDetailModelId)}"]`);
                sel?.focus();
              });
            }
          }}
          onClose={() => { setSelectedDetailModelId(null); setSelectedRemoteModel(null); }}
        />
      )}
    </div>
  );
};

export default ModelManager;
