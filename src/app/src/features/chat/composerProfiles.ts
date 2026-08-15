import { descriptorForRecipe } from '../backends/backendOptions';

/**
 * Composer affordances that belong to a specific backend.
 *
 * These are request shapes and controls, not metadata: lemond's `options[]`
 * describes load-time CLI flags, not the body of `/v1/audio/speech` or
 * `/v1/audio/generations`, so there is nothing on the wire to read them from.
 *
 * Keyed by modality first so a backend added to lemond after this build gets a
 * working composer for its kind, with per-recipe entries only where one backend
 * genuinely offers more than its peers.
 */
export interface ComposerProfile {
  /** Long-form musical generation: duration control and a music-mode prompt. */
  musicGeneration: boolean;
  /** Two-model voice-design then clone flow. */
  voiceCloning: boolean;
}

const NONE: ComposerProfile = { musicGeneration: false, voiceCloning: false };

const BY_MODALITY: Record<string, ComposerProfile> = {
  'Audio generation': NONE,
  'Text-to-speech': NONE,
};

const BY_RECIPE: Record<string, Partial<ComposerProfile>> = {
  acestep: { musicGeneration: true },
  openmoss: { voiceCloning: true },
};

export function composerProfile(recipe: string): ComposerProfile {
  const key = String(recipe || '').toLowerCase();
  const base = BY_MODALITY[descriptorForRecipe(key)?.modality ?? ''] ?? NONE;
  return { ...base, ...(BY_RECIPE[key] ?? {}) };
}
