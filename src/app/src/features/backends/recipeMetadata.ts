/**
 * Small, read-only view over lemond's /v1/system-info recipe metadata.
 *
 * Recipe ids are an open server-owned set. Descriptor metadata is authoritative:
 * this module intentionally has no per-recipe fallback tables. Presentation and
 * branding remain in the existing UI presentation helpers.
 */

export interface RecipeSchemaOption {
  value_type: string;
  enum?: unknown[];
  minimum?: number;
  maximum?: number;
  help?: string;
  ui?: { label: string; choices?: string };
}

export interface RecipeSchemaSource {
  backendOptionName(recipe: string, systemOptionName: string | undefined, selectableBackend: boolean): string | null;
  option(recipe: string, key: string): RecipeSchemaOption | undefined;
}

let schemaSource: RecipeSchemaSource | null = null;

export function setRecipeSchemaSource(source: RecipeSchemaSource | null): void {
  schemaSource = source;
}

export type RecipeCapability = 'LLM' | 'Audio' | 'Image' | 'TTS' | '3D' | 'Other';

export interface RecipeOptionMetadata {
  name: string;
  cliFlag: string;
  defaultValue: unknown;
  typeName: string;
  help: string;
  group: string;
  userConfigurable: boolean;
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function asString(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

function normalizedRecipeKey(recipe: string): string {
  return String(recipe || '').trim().toLowerCase();
}

function recipeRecord(systemInfo: unknown, recipe: string): Record<string, unknown> {
  const recipes = asRecord(asRecord(systemInfo).recipes);
  const key = normalizedRecipeKey(recipe);
  return asRecord(Object.prototype.hasOwnProperty.call(recipes, key) ? recipes[key] : undefined);
}

export function recipeCapability(systemInfo: unknown, recipe: string): RecipeCapability {
  const modality = asString(recipeRecord(systemInfo, recipe).modality).toLowerCase();
  switch (modality) {
    case 'text generation':
    case 'embeddings':
    case 'reranking':
    case 'text classification': return 'LLM';
    case 'speech-to-text':
    case 'audio generation': return 'Audio';
    case 'image generation': return 'Image';
    case 'text-to-speech': return 'TTS';
    case '3d generation': return '3D';
    default: return 'Other';
  }
}

export function recipeOptions(systemInfo: unknown, recipe: string): RecipeOptionMetadata[] {
  const raw = recipeRecord(systemInfo, recipe).options;
  if (!Array.isArray(raw)) return [];
  return raw.map(entry => {
    const option = asRecord(entry);
    return {
      name: asString(option.name),
      cliFlag: asString(option.cli_flag),
      defaultValue: option.default,
      typeName: asString(option.type_name),
      help: asString(option.help),
      group: asString(option.group),
      userConfigurable: option.user_configurable !== false,
    };
  }).filter(option => option.name !== '');
}

export function recipeOptionNames(systemInfo: unknown, recipe: string): string[] {
  const names = recipeOptions(systemInfo, recipe).map(option => option.name);
  const backend = recipeBackendOptionName(systemInfo, recipe);
  if (backend && !names.includes(backend)) names.unshift(backend);
  return names;
}

export function recipeOption(
  systemInfo: unknown,
  recipe: string,
  key: string,
): RecipeOptionMetadata | undefined {
  return recipeOptions(systemInfo, recipe).find(option => option.name === key);
}

export function recipeBackendOptionName(systemInfo: unknown, recipe: string): string | null {
  const key = normalizedRecipeKey(recipe);
  const explicit = recipeOptions(systemInfo, recipe).find(option => option.typeName === 'BACKEND')?.name;
  const selectable = recipeRecord(systemInfo, recipe).selectable_backend === true;
  if (schemaSource) return schemaSource.backendOptionName(key, explicit, selectable);
  if (explicit) return explicit;
  return selectable ? `${key}_backend` : null;
}

export function recipeSchemaOption(recipe: string, key: string): RecipeSchemaOption | undefined {
  return schemaSource?.option(normalizedRecipeKey(recipe), key);
}

function recipeOptionIsUserConfigurable(systemInfo: unknown, recipe: string, key: string): boolean {
  return recipeOption(systemInfo, recipe, key)?.userConfigurable !== false;
}

export function recipeConfigurableOptionNames(systemInfo: unknown, recipe: string): string[] {
  return recipeOptionNames(systemInfo, recipe)
    .filter(name => recipeOptionIsUserConfigurable(systemInfo, recipe, name));
}

export function recipeOptionLabel(systemInfo: unknown, recipe: string, key: string): string {
  const schemaLabel = recipeSchemaOption(recipe, key)?.ui?.label;
  if (schemaLabel) return schemaLabel;
  const typeName = recipeOption(systemInfo, recipe, key)?.typeName;
  if (recipeOptionIsBackend(systemInfo, recipe, key)) return 'Backend';
  if (typeName === 'DEVICES') return 'Device';
  if (recipeOptionIsArgs(systemInfo, recipe, key)) return 'Backend args';
  return key.charAt(0).toUpperCase() + key.slice(1).replace(/_/g, ' ');
}

export function recipeOptionHint(systemInfo: unknown, recipe: string, key: string): string | undefined {
  const help = recipeOption(systemInfo, recipe, key)?.help || recipeSchemaOption(recipe, key)?.help;
  if (help) return help;
  if (recipeOptionIsArgs(systemInfo, recipe, key)) return 'Raw backend args for this model only.';
  return undefined;
}

export function recipeOptionIsBackend(systemInfo: unknown, recipe: string, key: string): boolean {
  return recipeBackendOptionName(systemInfo, recipe) === key;
}

export function recipeOptionIsDevice(systemInfo: unknown, recipe: string, key: string): boolean {
  return recipeOption(systemInfo, recipe, key)?.typeName === 'DEVICES';
}

export function recipeOptionIsArgs(systemInfo: unknown, recipe: string, key: string): boolean {
  const option = recipeOption(systemInfo, recipe, key);
  return Boolean(option && option.typeName === 'ARGS' && option.name.endsWith('_args'));
}

export function recipeOptionIsNumeric(systemInfo: unknown, recipe: string, key: string): boolean {
  if (recipeOption(systemInfo, recipe, key)?.typeName === 'SIZE' || key === 'speed') return true;
  const valueType = recipeSchemaOption(recipe, key)?.value_type;
  return !recipeOption(systemInfo, recipe, key) && (valueType === 'integer' || valueType === 'number');
}

export function recipeOptionIsBoolean(systemInfo: unknown, recipe: string, key: string): boolean {
  if (recipeOption(systemInfo, recipe, key)?.typeName === 'BOOL') return true;
  return !recipeOption(systemInfo, recipe, key) && recipeSchemaOption(recipe, key)?.value_type === 'boolean';
}

export function recipeOptionChoices(recipe: string, key: string): string[] {
  const option = recipeSchemaOption(recipe, key);
  if (!option?.enum?.length || option.ui?.choices) return [];
  return option.enum.filter((choice): choice is string | number => typeof choice === 'string' || typeof choice === 'number').map(String);
}

export function recipeOptionRange(recipe: string, key: string): { minimum?: number; maximum?: number } {
  const option = recipeSchemaOption(recipe, key);
  return { minimum: option?.minimum, maximum: option?.maximum };
}
