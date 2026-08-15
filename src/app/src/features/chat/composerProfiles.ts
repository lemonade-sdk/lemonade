/**
 * Composer affordances that belong to a specific backend.
 *
 * These are request shapes and controls, not metadata: lemond's `options[]`
 * describes load-time CLI flags, not the body of `/v1/audio/speech` or
 * `/v1/audio/generations`, so there is nothing on the wire to read them from.
 * A backend added to lemond after this build therefore gets the plain composer
 * for its modality, which is the correct default - only these two backends
 * offer more than their peers do.
 */
export interface ComposerProfile {
  /** Long-form musical generation: duration control and a music-mode prompt. */
  musicGeneration: boolean;
  /** Two-model voice-design then clone flow. */
  voiceCloning: boolean;
}

const NONE: ComposerProfile = { musicGeneration: false, voiceCloning: false };

const BY_RECIPE: Record<string, Partial<ComposerProfile>> = {
  acestep: { musicGeneration: true },
  openmoss: { voiceCloning: true },
};

export function composerProfile(recipe: string): ComposerProfile {
  return { ...NONE, ...(BY_RECIPE[String(recipe || '').toLowerCase()] ?? {}) };
}
