import { scopedStorageKey } from '../accounts/accountStore';
import type { RecipeOptions } from '../../presetStore';

export const TTS_SETTINGS_EVENT = 'lemonade:tts-settings-changed';
export const DEFAULT_TTS_VOICE = 'coral';

// Lemonade's Kokoro backend exposes an OpenAI-compatible speech contract. Keep
// this list intentionally small and English-focused; users can still type a
// custom value through imported presets.
export const TTS_VOICES = [
  { id: 'alloy', label: 'Alloy' },
  { id: 'ash', label: 'Ash' },
  { id: 'ballad', label: 'Ballad' },
  { id: 'coral', label: 'Coral' },
  { id: 'echo', label: 'Echo' },
  { id: 'fable', label: 'Fable' },
  { id: 'nova', label: 'Nova' },
  { id: 'onyx', label: 'Onyx' },
  { id: 'sage', label: 'Sage' },
  { id: 'shimmer', label: 'Shimmer' },
  { id: 'verse', label: 'Verse' },
];

export const OPENMOSS_VOICE_PRESETS = [
  { id: 'Natural multilingual assistant voice', label: 'Natural multilingual' },
  { id: 'Warm multilingual narrator voice', label: 'Warm narrator' },
  { id: 'Clear multilingual professional voice', label: 'Clear professional' },
];

export type TtsPlaybackMode = 'demand' | 'always';
export type TtsReadMode = 'on-demand' | 'agent' | 'agent-and-user';

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

export function ttsReadModeFromSettings(settings: TtsPlaybackSettings): TtsReadMode {
  if (settings.playbackMode !== 'always') return 'on-demand';
  return settings.speakUserText ? 'agent-and-user' : 'agent';
}

export function saveTtsReadMode(scope: string, mode: TtsReadMode): void {
  if (mode === 'on-demand') {
    try {
      localStorage.setItem(playbackModeKey(scope), 'demand');
      localStorage.setItem(speakUserKey(scope), 'false');
    } catch { /* ignore */ }
  } else {
    try {
      localStorage.setItem(playbackModeKey(scope), 'always');
      localStorage.setItem(speakUserKey(scope), mode === 'agent-and-user' ? 'true' : 'false');
    } catch { /* ignore */ }
  }
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
