import type { BackendRecipe } from './backendSchema';
import { catalogRecipe, catalogRecipes } from './catalogState';

export type CatalogCapability =
  | 'chat' | 'omni' | 'image' | 'audio' | 'audio-generation'
  | 'tts' | 'model3d' | 'embedding' | 'reranking' | 'classification';

const MODE_CAPABILITIES: Record<string, CatalogCapability[]> = {
  chat: ['chat', 'omni'],
  embedding: ['embedding'],
  reranking: ['reranking'],
  transcription: ['audio'],
  image: ['image'],
  tts: ['tts'],
  'audio-generation': ['audio-generation'],
  classification: ['classification'],
  mesh: ['model3d'],
};

export function recipeModeCapabilities(row: BackendRecipe | undefined): CatalogCapability[] | undefined {
  if (!row || !Array.isArray(row.supported_modes)) return undefined;
  const capabilities = new Set<CatalogCapability>();
  for (const mode of row.supported_modes) {
    const mapped = Object.prototype.hasOwnProperty.call(MODE_CAPABILITIES, mode) ? MODE_CAPABILITIES[mode] : [];
    mapped.forEach(capability => capabilities.add(capability));
  }
  return capabilities.size ? [...capabilities] : undefined;
}

export function catalogRecipeCapabilities(recipe: string): CatalogCapability[] | undefined {
  return recipeModeCapabilities(catalogRecipe(recipe));
}

export interface ExternalRecipeChoice {
  recipe: string;
  label: string;
  hint: string;
  capabilities: CatalogCapability[];
}

export function externalRecipeChoices(): ExternalRecipeChoice[] {
  return catalogRecipes()
    .filter(row => row.source === 'external' && !row.incompatibility)
    .map(row => ({
      recipe: row.recipe,
      label: row.display_name || row.recipe,
      hint: 'External backend defined on this Lemonade server.',
      capabilities: recipeModeCapabilities(row) ?? ['chat', 'omni'],
    }));
}

export function externalCheckpointRoles(recipe: string): string[] | undefined {
  const row = catalogRecipe(recipe);
  if (!row || row.source !== 'external') return undefined;
  const roles = Array.isArray(row.required_checkpoints) ? row.required_checkpoints : ['main'];
  return [...new Set(roles.filter(role => role && role !== 'main'))];
}

export function externalCheckpoints(
  recipe: string,
  main: string,
  values: Record<string, string>,
): { checkpoints?: Record<string, string>; missing: string[] } {
  const roles = externalCheckpointRoles(recipe) ?? [];
  const checkpoints: Record<string, string> = { main };
  const missing: string[] = [];
  for (const role of roles) {
    const value = Object.prototype.hasOwnProperty.call(values, role) ? String(values[role] || '').trim() : '';
    if (value) checkpoints[role] = value;
    else missing.push(role);
  }
  return { checkpoints: roles.length && main ? checkpoints : undefined, missing };
}
