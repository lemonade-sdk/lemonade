import { scopedStorageKey } from '../accounts/accountStore';
import type { RecipeOptions } from '../../presetStore';

export const TTS_SETTINGS_EVENT = 'lemonade:tts-settings-changed';
export const DEFAULT_TTS_VOICE = 'alloy';

export const TTS_VOICES = [
  { id: 'alloy', label: 'Alloy' },
  { id: 'echo', label: 'Echo' },
  { id: 'fable', label: 'Fable' },
  { id: 'onyx', label: 'Onyx' },
  { id: 'nova', label: 'Nova' },
  { id: 'shimmer', label: 'Shimmer' },
];

export type TtsPlaybackMode = 'demand' | 'always';

export interface TtsPlaybackSettings {
  modelName: string | null;
  speakUserText: boolean;
  playbackMode: TtsPlaybackMode;
}

function activeModelKey(scope: string): string {
  return scopedStorageKey(scope, 'tts_active_speech_model');
}

function speakUserKey(scope: string): string {
  return scopedStorageKey(scope, 'tts_speak_user_text');
}

function playbackModeKey(scope: string): string {
  return scopedStorageKey(scope, 'tts_playback_mode');
}

function normalizePlaybackMode(value: unknown): TtsPlaybackMode {
  return value === 'always' ? 'always' : 'demand';
}

export function loadTtsPlaybackSettings(scope: string): TtsPlaybackSettings {
  try {
    const modelName = localStorage.getItem(activeModelKey(scope));
    const speakUserText = localStorage.getItem(speakUserKey(scope)) === 'true';
    const playbackMode = normalizePlaybackMode(localStorage.getItem(playbackModeKey(scope)));
    return { modelName: modelName || null, speakUserText, playbackMode };
  } catch {
    return { modelName: null, speakUserText: false, playbackMode: 'demand' };
  }
}

export function saveActiveTtsModel(scope: string, modelName: string | null): void {
  try {
    const key = activeModelKey(scope);
    if (modelName) localStorage.setItem(key, modelName);
    else localStorage.removeItem(key);
  } catch { /* ignore */ }
  emitTtsSettingsChanged(scope);
}

export function saveSpeakUserText(scope: string, enabled: boolean): void {
  try { localStorage.setItem(speakUserKey(scope), enabled ? 'true' : 'false'); } catch { /* ignore */ }
  emitTtsSettingsChanged(scope);
}

export function saveTtsPlaybackMode(scope: string, mode: TtsPlaybackMode): void {
  try { localStorage.setItem(playbackModeKey(scope), normalizePlaybackMode(mode)); } catch { /* ignore */ }
  emitTtsSettingsChanged(scope);
}

export function emitTtsSettingsChanged(scope: string): void {
  try {
    window.dispatchEvent(new CustomEvent(TTS_SETTINGS_EVENT, { detail: { scope } }));
  } catch { /* ignore */ }
}

export function normalizeTtsVoice(value: unknown): string {
  const voice = String(value || '').trim();
  return voice || DEFAULT_TTS_VOICE;
}

export function ttsVoiceFromRecipeOptions(options: RecipeOptions | null | undefined): string {
  return normalizeTtsVoice(options?.voice);
}
