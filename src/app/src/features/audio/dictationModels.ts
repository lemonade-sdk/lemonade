import type { LoadedModel, ModelInfo } from '../../api';

/** A separate realtime ASR model is used for dictation, not the chat model. */
export interface DictationModelChoice {
  name: string;
  info: ModelInfo | null;
  alreadyLoaded: boolean;
  downloaded: boolean;
}

function modelName(info: ModelInfo): string {
  return String((info as any).model_name || info.name || info.id || '').trim();
}

function isRealtimeTranscriber(info: ModelInfo): boolean {
  const labels = (info.labels || []).map(label => String(label).toLowerCase());
  return labels.includes('realtime-transcription') && !labels.includes('chat');
}

function isDownloaded(info: ModelInfo): boolean {
  const status = String((info as any).status || '').toLowerCase();
  return (info as any).downloaded === true || (info as any).is_downloaded === true
    || ['downloaded', 'ready', 'local'].includes(status)
    || (info.labels || []).some(label => ['downloaded', 'ready', 'local', 'installed'].includes(String(label).toLowerCase()));
}

function size(info: ModelInfo): number {
  const value = Number(info.size);
  return Number.isFinite(value) && value > 0 ? value : Infinity;
}

/** Prefer an already-running realtime transcriber, then a downloaded small one. */
export function selectDictationModel(
  models: readonly ModelInfo[],
  loaded: readonly LoadedModel[],
): DictationModelChoice | null {
  const catalog = new Map<string, ModelInfo>();
  for (const info of models) {
    const name = modelName(info).toLowerCase();
    if (name && !catalog.has(name)) catalog.set(name, info);
  }

  for (const model of loaded) {
    if (String(model.type).toLowerCase() !== 'transcription') continue;
    const info = catalog.get(model.model_name.toLowerCase()) || null;
    const knownRealtime = info && isRealtimeTranscriber(info);
    const recipe = String(model.recipe || '').toLowerCase();
    if (knownRealtime || recipe.includes('whispercpp') || recipe.includes('moonshine')) {
      return { name: model.model_name, info, alreadyLoaded: true, downloaded: true };
    }
  }

  const candidates = [...catalog.values()].filter(isRealtimeTranscriber);
  candidates.sort((a, b) =>
    Number(isDownloaded(b)) - Number(isDownloaded(a))
    || size(a) - size(b)
    || modelName(a).localeCompare(modelName(b)));

  const info = candidates[0];
  return info ? {
    name: modelName(info),
    info,
    alreadyLoaded: false,
    downloaded: isDownloaded(info),
  } : null;
}

/** Replace an interim transcript without losing the rest of the editable draft. */
export function updateDraftWithDictation(draft: string, previous: string, transcript: string): string {
  if (!transcript) return draft;
  if (previous) {
    const index = draft.lastIndexOf(previous);
    if (index >= 0) return draft.slice(0, index) + transcript + draft.slice(index + previous.length);
  }
  return draft + (draft && !/\s$/.test(draft) ? ' ' : '') + transcript;
}
