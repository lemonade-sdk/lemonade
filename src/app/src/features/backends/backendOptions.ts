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

export function optionSchemaForRecipe(recipe: string): Map<string, RecipeOptionSchema> {
  return new Map((descriptorForRecipe(recipe)?.options ?? []).map(option => [option.name, option]));
}

/** Option names are unique across recipes, so a name is enough to find its schema. */
export function findOptionSchema(name: string): RecipeOptionSchema | undefined {
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

/* Hints describing this editor's behavior, which the server cannot know. */
const TYPE_HINTS: Record<string, string> = {
  BACKEND: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  DEVICES: 'Optional device selector for the selected backend.',
};

const HINT_OVERRIDES: Record<string, string> = {
  ctx_size: 'Runtime context window for this exact model.',
};

export function optionLabel(key: string): string {
  const override = LABEL_OVERRIDES[key];
  if (override) return override;
  const byType = TYPE_LABELS[findOptionSchema(key)?.typeName ?? ''];
  if (byType) return byType;
  if (key.endsWith('_args')) return 'Backend args';
  return key.charAt(0).toUpperCase() + key.slice(1).replace(/_/g, ' ');
}

export function optionHint(key: string): string | undefined {
  const override = HINT_OVERRIDES[key];
  if (override) return override;
  const schema = findOptionSchema(key);
  const byType = TYPE_HINTS[schema?.typeName ?? ''];
  if (byType) return byType;
  if (key.endsWith('_args')) return 'Raw backend args for this model only.';
  return schema?.help || undefined;
}

/* `speed` comes from a model's sample_params, not from any descriptor. */
const RESIDUAL_NUMERIC_KEYS = new Set(['speed']);

export function isBackendOption(key: string): boolean {
  return findOptionSchema(key)?.typeName === 'BACKEND';
}

export function isDeviceOption(key: string): boolean {
  return findOptionSchema(key)?.typeName === 'DEVICES';
}

export function isArgsOption(key: string): boolean {
  return key.endsWith('_args');
}

export function isNumericOption(key: string): boolean {
  return findOptionSchema(key)?.typeName === 'SIZE' || RESIDUAL_NUMERIC_KEYS.has(key);
}

export function isBooleanOption(key: string): boolean {
  return findOptionSchema(key)?.typeName === 'BOOL';
}
