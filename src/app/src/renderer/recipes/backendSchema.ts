export type Scalar = string | number | boolean | null;
export type Control = 'text' | 'textarea' | 'number' | 'checkbox' | 'select';
export interface BackendOption {
  name: string;
  value_type: 'string' | 'integer' | 'number' | 'boolean';
  default_value: Scalar;
  unset_values: Scalar[];
  enum?: Scalar[];
  minimum?: number;
  maximum?: number;
  help?: string;
  group?: string;
  ui?: { label: string; control: Control; order: number; choices?: 'supported_backends' };
}
export interface BackendRecipe {
  recipe: string;
  source: 'builtin' | 'external';
  display_name: string;
  options: BackendOption[];
  backend_option?: string | null;
  support: { backend: string; tier: string; device_summary: string; supported_os: string[] }[];
  incompatibility?: string;
}
export interface BackendSchema {
  schema_version: number;
  control_version: number;
  definition_schema_version: number;
  catalog_id: string;
  recipes: BackendRecipe[];
}
export class SchemaError extends Error {}
export const own = <T,>(record: Record<string, T> | undefined, key: string): T | undefined =>
  record && Object.prototype.hasOwnProperty.call(record, key) ? record[key] : undefined;
export const isRecord = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === 'object' && !Array.isArray(value);
export const isScalar = (value: unknown): value is Scalar =>
  value === null || typeof value === 'string' || typeof value === 'boolean' ||
  (typeof value === 'number' && Number.isFinite(value));
export const isUnset = (option: BackendOption, value: Scalar): boolean =>
  option.unset_values.some(sentinel => sentinel === value);
export const controlFor = (option: BackendOption): Control => option.ui?.control ??
  (option.enum?.length ? 'select' : option.value_type === 'boolean' ? 'checkbox' :
    option.value_type === 'string' ? 'text' : 'number');

export function validateValue(option: BackendOption, value: Scalar, backends?: string[]): string | undefined {
  if (typeof value === 'number' && (!Number.isFinite(value) ||
      (Number.isInteger(value) && !Number.isSafeInteger(value)))) return 'Value cannot round-trip safely in this client.';
  if (isUnset(option, value)) return;
  if (option.value_type === 'integer' ? typeof value !== 'number' || !Number.isSafeInteger(value) :
      option.value_type === 'number' ? typeof value !== 'number' : typeof value !== option.value_type) return `Expected ${option.value_type}.`;
  if (typeof value === 'number' && ((option.minimum !== undefined && value < option.minimum) ||
      (option.maximum !== undefined && value > option.maximum))) return `Value must be in range ${option.minimum ?? '−∞'} to ${option.maximum ?? '∞'}.`;
  if (option.enum && !option.enum.some(item => item === value)) return 'Value is not an advertised choice.';
  if (option.ui?.choices && backends && !backends.includes(value as string)) return 'Backend is unavailable on the selected server.';
}

function requireShape(condition: unknown): asserts condition {
  if (!condition) throw new SchemaError('Malformed backend schema response.');
}

export function parseBackendSchema(value: unknown): BackendSchema {
  requireShape(isRecord(value));
  for (const key of ['schema_version', 'control_version', 'definition_schema_version']) {
    if (own(value, key) !== 1) throw new SchemaError('Incompatible backend schema/control version. This client supports version 1.');
  }
  requireShape(typeof value.catalog_id === 'string' && value.catalog_id.length > 0 &&
    Array.isArray(value.recipes) && value.recipes.length <= 1024);
  const names = new Set<string>();
  const recipes = value.recipes.map((row: unknown): BackendRecipe => {
    requireShape(isRecord(row) && typeof row.recipe === 'string' && row.recipe.length > 0 && !names.has(row.recipe));
    names.add(row.recipe);
    requireShape((row.source === 'builtin' || row.source === 'external') && typeof row.display_name === 'string' &&
      Array.isArray(row.options) && row.options.length <= 1024 && Array.isArray(row.support));
    requireShape(row.backend_option === undefined || row.backend_option === null || typeof row.backend_option === 'string');
    const optionNames = new Set<string>();
    const options = row.options.map((item: unknown): BackendOption => {
      requireShape(isRecord(item) && typeof item.name === 'string' && item.name.length > 0 && !optionNames.has(item.name));
      optionNames.add(item.name);
      requireShape(['string', 'integer', 'number', 'boolean'].includes(item.value_type as string) && isScalar(item.default_value));
      for (const key of ['enum', 'unset_values']) requireShape(item[key] === undefined ||
        (Array.isArray(item[key]) && item[key].length <= 1024 && item[key].every(isScalar)));
      for (const key of ['minimum', 'maximum']) requireShape(item[key] === undefined ||
        (typeof item[key] === 'number' && Number.isFinite(item[key])));
      for (const key of ['help', 'group']) requireShape(item[key] === undefined || typeof item[key] === 'string');
      if (item.ui !== undefined) requireShape(isRecord(item.ui) && typeof item.ui.label === 'string' &&
        typeof item.ui.control === 'string' && Number.isSafeInteger(item.ui.order) &&
        (item.ui.choices === undefined || item.ui.choices === 'supported_backends'));
      return { ...item, unset_values: item.unset_values ?? [null] } as unknown as BackendOption;
    });
    requireShape(row.support.every((support: unknown) => isRecord(support) && typeof support.backend === 'string' &&
      typeof support.tier === 'string' && typeof support.device_summary === 'string' &&
      Array.isArray(support.supported_os) && support.supported_os.every(os => typeof os === 'string')));
    const recipe = { ...row, options } as unknown as BackendRecipe;
    if (recipe.source === 'external') {
      for (const option of options) {
        const control = controlFor(option);
        const compatible = control === 'checkbox' ? option.value_type === 'boolean' :
          control === 'number' ? ['integer', 'number'].includes(option.value_type) :
          control === 'text' || control === 'textarea' ? option.value_type === 'string' :
          control === 'select' && (option.ui?.choices ? !option.enum && option.value_type === 'string' : !!option.enum?.length);
        const error = validateValue(option, option.default_value) ?? option.enum?.map(item => validateValue(option, item)).find(Boolean);
        if (!compatible || error) recipe.incompatibility = `Unsupported option ${option.name}: ${error ?? 'incompatible control/type or choice source'}.`;
      }
      if (typeof recipe.backend_option === 'string' && !optionNames.has(recipe.backend_option)) recipe.incompatibility = 'Backend selector is absent from the option contract.';
    }
    return recipe;
  });
  return { ...value, recipes } as unknown as BackendSchema;
}

export function backendSelector(recipe: BackendRecipe): string | null {
  if (recipe.backend_option !== undefined) return recipe.backend_option;
  const hints = recipe.options.filter(option => option.ui?.choices === 'supported_backends');
  return hints.length === 1 ? hints[0].name : null;
}
