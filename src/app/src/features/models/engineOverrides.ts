import type { ModelInfo } from '../../api';
import { capabilityFromModelInfo, type ModelCapability } from '../../modelCapabilities';

type RecordValue = Record<string, unknown>;

function record(value: unknown): RecordValue {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as RecordValue : {};
}

export function canonicalModelId(model: ModelInfo): string {
  return String((model as any).canonical_model_name || '').trim();
}

export function bareModelId(model: ModelInfo): string {
  const id = canonicalModelId(model) || String(model.id || '');
  return id.replace(/^(builtin|user|extra)\./, '');
}

function checkpointRoles(model: ModelInfo): Record<string, string> {
  const checkpoints = record((model as any).checkpoints);
  const roles: Record<string, string> = {};
  for (const [role, value] of Object.entries(checkpoints)) {
    if (typeof value === 'string' && value.trim()) roles[role] = value.trim();
  }
  const main = String(roles.main || (model as any).checkpoint || '').trim();
  if (main) roles.main = main;
  return roles;
}

function modelArtifactSource(model: ModelInfo): string {
  const source = String((model as any).source || '').trim().toLowerCase();
  if (source === 'local_path' || source === 'local_upload') return source;
  return String((model as any).registry_source || source).trim().toLowerCase();
}

export function sameModelArtifacts(left: ModelInfo, right: ModelInfo): boolean {
  const a = checkpointRoles(left);
  const b = checkpointRoles(right);
  const aKeys = Object.keys(a).sort();
  const bKeys = Object.keys(b).sort();
  return Boolean(a.main && b.main)
    && modelArtifactSource(left) === modelArtifactSource(right)
    && aKeys.length === bKeys.length
    && aKeys.every((key, i) => key === bKeys[i] && a[key] === b[key]);
}

export function collapseIdenticalBuiltinOverrides(models: ModelInfo[]): ModelInfo[] {
  const overrides = new Map<string, ModelInfo>();
  for (const model of models) {
    const canonical = canonicalModelId(model);
    if (canonical.startsWith('user.')) overrides.set(canonical.slice(5).toLowerCase(), model);
  }
  return models.filter(model => {
    const canonical = canonicalModelId(model);
    if (!canonical.startsWith('builtin.')) return true;
    const override = overrides.get(canonical.slice(8).toLowerCase());
    return !override || !sameModelArtifacts(model, override);
  });
}

const MODALITY_MODE: Record<string, string> = {
  'text generation': 'chat',
  embeddings: 'embeddings',
  reranking: 'reranking',
  'text classification': 'classification',
  'speech-to-text': 'transcription',
  'image generation': 'image',
  'text-to-speech': 'tts',
  'audio generation': 'audio-generation',
  '3d generation': '3d',
};

const CAPABILITY_MODE: Record<Exclude<ModelCapability, 'unknown'>, string> = {
  chat: 'chat',
  image: 'image',
  audio: 'transcription',
  'audio-generation': 'audio-generation',
  tts: 'tts',
  model3d: '3d',
  embedding: 'embeddings',
  reranking: 'reranking',
  classification: 'classification',
};

export interface InferenceEngineChoice { recipe: string; label: string }

export function inferenceEnginesForModel(model: ModelInfo, systemInfo: unknown): InferenceEngineChoice[] {
  const current = String((model as any).recipe || '').trim();
  const recipes = record(record(systemInfo).recipes);
  const capability = capabilityFromModelInfo(model);
  const requiredMode = capability !== 'unknown' ? CAPABILITY_MODE[capability] : '';
  const alternatives: Array<InferenceEngineChoice & { order: number }> = [];

  if (requiredMode) {
    for (const [recipe, raw] of Object.entries(recipes)) {
      const info = record(raw);
      const fromServer = Array.isArray(info.supported_modes)
        ? info.supported_modes.filter((mode): mode is string => typeof mode === 'string').map(mode => mode.toLowerCase())
        : [];
      const mode = MODALITY_MODE[String(info.modality || '').trim().toLowerCase()];
      const modes = fromServer.length ? fromServer : (mode ? [mode] : []);
      if (!modes.includes(requiredMode)) continue;
      const backends = Object.values(record(info.backends));
      if (!backends.length || backends.every(backend => String(record(backend).state || '').toLowerCase() === 'unsupported')) continue;
      alternatives.push({
        recipe,
        label: String(info.display_name || recipe),
        order: typeof info.order === 'number' ? info.order : 9999,
      });
    }
  }

  alternatives.sort((a, b) => a.order - b.order || a.label.localeCompare(b.label));
  const result: InferenceEngineChoice[] = alternatives.map(({ recipe, label }) => ({ recipe, label }));
  if (current && !result.some(option => option.recipe === current)) {
    result.unshift({ recipe: current, label: String(record(recipes[current]).display_name || current) });
  }
  return result;
}
