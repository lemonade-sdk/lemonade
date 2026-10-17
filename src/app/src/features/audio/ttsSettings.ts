import { storageKey } from '../../storage';
import type { RecipeOptions } from '../../modelConfiguration';

export const TTS_SETTINGS_EVENT = 'lemonade:tts-settings-changed';
export const DEFAULT_TTS_VOICE = 'coral';

// Lemonade's Kokoro backend exposes an OpenAI-compatible speech contract. Keep
// this list intentionally small and English-focused; users can still type a
// custom value through direct model configuration.
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
export type TtsPlaybackMode = 'demand' | 'always';
export type TtsReadMode = 'on-demand' | 'agent' | 'agent-and-user';

export interface TtsPlaybackSettings {
  modelName: string | null;
  speakUserText: boolean;
  playbackMode: TtsPlaybackMode;
}

function activeModelKey(): string {
  return storageKey('tts_active_speech_model');
}

function speakUserKey(): string {
  return storageKey('tts_speak_user_text');
}

function playbackModeKey(): string {
  return storageKey('tts_playback_mode');
}

function normalizePlaybackMode(value: unknown): TtsPlaybackMode {
  return value === 'always' ? 'always' : 'demand';
}

export function loadTtsPlaybackSettings(): TtsPlaybackSettings {
  try {
    const modelName = localStorage.getItem(activeModelKey());
    const speakUserText = localStorage.getItem(speakUserKey()) === 'true';
    const playbackMode = normalizePlaybackMode(localStorage.getItem(playbackModeKey()));
    return { modelName: modelName || null, speakUserText, playbackMode };
  } catch {
    return { modelName: null, speakUserText: false, playbackMode: 'demand' };
  }
}

export function saveActiveTtsModel( modelName: string | null): void {
  try {
    const key = activeModelKey();
    if (modelName) localStorage.setItem(key, modelName);
    else localStorage.removeItem(key);
  } catch { /* ignore */ }
  emitTtsSettingsChanged();
}

export function saveSpeakUserText( enabled: boolean): void {
  try { localStorage.setItem(speakUserKey(), enabled ? 'true' : 'false'); } catch { /* ignore */ }
  emitTtsSettingsChanged();
}

export function saveTtsPlaybackMode( mode: TtsPlaybackMode): void {
  try { localStorage.setItem(playbackModeKey(), normalizePlaybackMode(mode)); } catch { /* ignore */ }
  emitTtsSettingsChanged();
}

export function ttsReadModeFromSettings(settings: TtsPlaybackSettings): TtsReadMode {
  if (settings.playbackMode !== 'always') return 'on-demand';
  return settings.speakUserText ? 'agent-and-user' : 'agent';
}

export function saveTtsReadMode( mode: TtsReadMode): void {
  if (mode === 'on-demand') {
    try {
      localStorage.setItem(playbackModeKey(), 'demand');
      localStorage.setItem(speakUserKey(), 'false');
    } catch { /* ignore */ }
  } else {
    try {
      localStorage.setItem(playbackModeKey(), 'always');
      localStorage.setItem(speakUserKey(), mode === 'agent-and-user' ? 'true' : 'false');
    } catch { /* ignore */ }
  }
  emitTtsSettingsChanged();
}

export function emitTtsSettingsChanged(): void {
  try {
    window.dispatchEvent(new CustomEvent(TTS_SETTINGS_EVENT));
  } catch { /* ignore */ }
}

export function normalizeTtsVoice(value: unknown): string {
  const voice = String(value || '').trim();
  return voice || DEFAULT_TTS_VOICE;
}

export function ttsVoiceFromRecipeOptions(options: RecipeOptions | null | undefined): string {
  return normalizeTtsVoice(options?.voice);
}
