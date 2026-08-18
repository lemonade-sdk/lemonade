/**
 * Small, read-only view over lemond's /v1/system-info recipe metadata.
 *
 * Recipe ids are an open server-owned set. Descriptor metadata is authoritative:
 * this module intentionally has no per-recipe fallback tables. Presentation and
 * branding remain in the existing UI presentation helpers.
 */

export type RecipeCapability = 'LLM' | 'Audio' | 'Image' | 'TTS' | '3D' | 'Other';

export interface RecipeOptionMetadata {
  name: string;
  cliFlag: string;
  defaultValue: unknown;
  typeName: string;
  help: string;
  group: string;
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
  return asRecord(recipes[normalizedRecipeKey(recipe)]);
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
  const explicit = recipeOptions(systemInfo, recipe).find(option => option.typeName === 'BACKEND')?.name;
  if (explicit) return explicit;
  return recipeRecord(systemInfo, recipe).selectable_backend === true
    ? `${normalizedRecipeKey(recipe)}_backend`
    : null;
}

export function recipeOptionLabel(systemInfo: unknown, recipe: string, key: string): string {
  const typeName = recipeOption(systemInfo, recipe, key)?.typeName;
  if (recipeOptionIsBackend(systemInfo, recipe, key)) return 'Backend';
  if (typeName === 'DEVICES') return 'Device';
  if (recipeOptionIsArgs(systemInfo, recipe, key)) return 'Backend args';
  return key.charAt(0).toUpperCase() + key.slice(1).replace(/_/g, ' ');
}

export function recipeOptionHint(systemInfo: unknown, recipe: string, key: string): string | undefined {
  const help = recipeOption(systemInfo, recipe, key)?.help;
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
  return recipeOption(systemInfo, recipe, key)?.typeName === 'SIZE' || key === 'speed';
}

export function recipeOptionIsBoolean(systemInfo: unknown, recipe: string, key: string): boolean {
  return recipeOption(systemInfo, recipe, key)?.typeName === 'BOOL';
}

export interface GenerationParamMetadata {
  name: string;
  label: string;
  typeName: string;
  defaultValue: unknown;
  min: number | null;
  max: number | null;
  step: number | null;
  enumValues: string[];
  help: string;
  group: string;
  exclusiveGroup: string;
  accept: string;
  randomSentinel: number | null;
}

export interface GenerationParamChoice {
  id: string;
  label: string;
  members: GenerationParamMetadata[];
}

function asNumberOrNull(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

export function generationParams(
  systemInfo: unknown,
  recipe: string,
  mode: string,
): GenerationParamMetadata[] {
  const byMode = asRecord(recipeRecord(systemInfo, recipe).generation_params);
  const raw = byMode[mode];
  if (!Array.isArray(raw)) return [];
  return raw.map(entry => {
    const param = asRecord(entry);
    return {
      name: asString(param.name),
      label: asString(param.label),
      typeName: asString(param.type_name),
      defaultValue: param.default,
      min: asNumberOrNull(param.min),
      max: asNumberOrNull(param.max),
      step: asNumberOrNull(param.step),
      enumValues: Array.isArray(param.enum_values) ? param.enum_values.map(asString) : [],
      help: asString(param.help),
      group: asString(param.group),
      exclusiveGroup: asString(param.exclusive_group),
      accept: asString(param.accept),
      randomSentinel: asNumberOrNull(param.random_sentinel),
    };
  }).filter(param => param.name !== '' && param.typeName !== '');
}

export function generationParamChoices(
  params: GenerationParamMetadata[],
): GenerationParamChoice[] {
  const order: string[] = [];
  const byGroup = new Map<string, GenerationParamMetadata[]>();
  for (const param of params) {
    if (!param.exclusiveGroup) continue;
    if (!byGroup.has(param.exclusiveGroup)) {
      byGroup.set(param.exclusiveGroup, []);
      order.push(param.exclusiveGroup);
    }
    byGroup.get(param.exclusiveGroup)!.push(param);
  }
  return order.map(id => ({
    id,
    label: id.charAt(0).toUpperCase() + id.slice(1).replace(/_/g, ' '),
    members: byGroup.get(id)!,
  }));
}

export function generationParamDefault(
  param: GenerationParamMetadata,
  modelDefaults: Record<string, unknown>,
  effectiveOptions: Record<string, unknown>,
): unknown {
  const effective = effectiveOptions[param.name];
  if (effective !== undefined && effective !== null) return effective;
  const declared = modelDefaults[param.name];
  if (declared !== undefined && declared !== null) return declared;
  return param.defaultValue ?? '';
}

/**
 * A blank seed box means "surprise me". Backends that read a sentinel get it;
 * backends whose seed is unsigned have none, so a value is drawn here instead
 * of sending something they would take literally.
 */
export function resolveGenerationSeed(param: GenerationParamMetadata): number {
  if (param.randomSentinel !== null) return param.randomSentinel;
  const low = param.min !== null ? param.min : 0;
  const high = param.max !== null ? param.max : 0xffffffff;
  return low + Math.floor(Math.random() * Math.max(1, high - low));
}
