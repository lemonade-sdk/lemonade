/**
 * Which section of the Backends view a recipe belongs in.
 *
 * Keyed on lemond's `modality`, a closed vocabulary of seven strings, rather
 * than on the open set of recipe names — so adding a backend never needs an
 * edit here, and a modality this build has not seen still gets a section
 * instead of being silently filed under the first one.
 *
 * MODALITY_ORDER mirrors docs/tools/gen_backend_boilerplate.py:129 and carries
 * the same rule stated there: it controls presentation only, and must never
 * filter recipes.
 */
const MODALITY_ORDER = [
  'Text generation',
  'Speech-to-text',
  'Text-to-speech',
  'Audio generation',
  'Image generation',
  '3D generation',
];

export const OTHER_RUNTIMES = 'Other runtimes';

const SECTION_META: Record<string, { title: string; description: string }> = {
  'Text generation': { title: 'Language models', description: 'Chat, completion, embedding, and reranking runtimes.' },
  'Speech-to-text': { title: 'Speech to text', description: 'Transcription runtimes.' },
  'Text-to-speech': { title: 'Text to speech', description: 'Text-to-speech runtimes.' },
  'Audio generation': { title: 'Audio generation', description: 'Music and sound generation runtimes.' },
  'Image generation': { title: 'Image generation', description: 'Image generation and editing runtimes.' },
  '3D generation': { title: '3D generation', description: '3D asset generation runtimes.' },
  'Text classification': { title: 'Classification', description: 'Text classification runtimes.' },
  [OTHER_RUNTIMES]: { title: OTHER_RUNTIMES, description: 'Runtimes this build has no section for yet.' },
};

export function sectionForModality(modality: string): string {
  return modality || OTHER_RUNTIMES;
}

export function sectionMeta(section: string): { title: string; description: string } {
  return SECTION_META[section] ?? { title: section, description: '' };
}

/** Known modalities in the documented order, then any newcomers, then the catch-all. */
export function orderSections(present: string[]): string[] {
  const unique = [...new Set(present)];
  const known = MODALITY_ORDER.filter(modality => unique.includes(modality));
  const extra = unique
    .filter(section => !known.includes(section) && section !== OTHER_RUNTIMES)
    .sort();
  return [...known, ...extra, ...(unique.includes(OTHER_RUNTIMES) ? [OTHER_RUNTIMES] : [])];
}
