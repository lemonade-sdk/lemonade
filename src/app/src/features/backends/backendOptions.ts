import { getBackendCatalogSnapshot } from './backendCatalogStore';
import type { RecipeOptionSchema } from './backendCatalog';

/**
 * Presentation for a recipe option, driven by the schema lemond publishes in
 * `recipes[].options[]`. Nothing here enumerates recipe or option names except
 * the residue lemond does not publish, which is named explicitly.
 */

export function descriptorForRecipe(recipe: string) {
  return getBackendCatalogSnapshot().catalog?.byRecipe.get(String(recipe || '').toLowerCase());
}

/**
 * The image backends all declare `steps`, `width`, `height` and `cfg_scale`,
 * and describe them differently, so a caller that knows which recipe it is
 * editing must say so or it gets another backend's wording. Callers with no
 * recipe in hand still resolve, through the first descriptor declaring the key.
 */
export function findOptionSchema(name: string, recipe?: string): RecipeOptionSchema | undefined {
  const own = recipe ? descriptorForRecipe(recipe)?.options.find(option => option.name === name) : undefined;
  if (own) return own;
  for (const descriptor of getBackendCatalogSnapshot().catalog?.recipes ?? []) {
    const match = descriptor.options.find(option => option.name === name);
    if (match) return match;
  }
  return undefined;
}

/* lemond publishes CLI help text but no end-user field label, so labels are
 * derived from the option's declared type. `cfg_scale` is the one name that
 * does not title-case cleanly; `ctx_size` is a common option, not a
 * descriptor one, so it has no schema to derive from. */
const LABEL_OVERRIDES: Record<string, string> = {
  ctx_size: 'Context size',
  cfg_scale: 'CFG scale',
};

const TYPE_LABELS: Record<string, string> = { BACKEND: 'Backend', DEVICES: 'Device' };

export function optionLabel(key: string, recipe?: string): string {
  const override = LABEL_OVERRIDES[key];
  if (override) return override;
  const byType = TYPE_LABELS[findOptionSchema(key, recipe)?.typeName ?? ''];
  if (byType) return byType;
  if (isArgsOption(key)) return 'Backend args';
  return key.charAt(0).toUpperCase() + key.slice(1).replace(/_/g, ' ');
}

/** The backend's own CLI help, which is the only description of an option anyone writes. */
export function optionHint(key: string, recipe?: string): string | undefined {
  const help = findOptionSchema(key, recipe)?.help;
  return help || (isArgsOption(key) ? 'Raw backend args for this model only.' : undefined);
}

/* `speed` comes from a model's sample_params, not from any descriptor. */
const RESIDUAL_NUMERIC_KEYS = new Set(['speed']);

export function isBackendOption(key: string, recipe?: string): boolean {
  return findOptionSchema(key, recipe)?.typeName === 'BACKEND';
}

export function isDeviceOption(key: string, recipe?: string): boolean {
  return findOptionSchema(key, recipe)?.typeName === 'DEVICES';
}

export function isArgsOption(key: string): boolean {
  return key.endsWith('_args');
}

export function isNumericOption(key: string, recipe?: string): boolean {
  return findOptionSchema(key, recipe)?.typeName === 'SIZE' || RESIDUAL_NUMERIC_KEYS.has(key);
}

export function isBooleanOption(key: string, recipe?: string): boolean {
  return findOptionSchema(key, recipe)?.typeName === 'BOOL';
}
