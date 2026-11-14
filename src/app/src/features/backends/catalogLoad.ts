import { api, ConnectionSnapshot, ModelInfo } from '../../api';
import { BackendRecipe, Scalar, backendSelector, isRecord, isUnset, own, validateValue } from './backendSchema';
import { refreshBackendCatalog } from './backendCatalog';
import { catalogRecipe, catalogState, resolveBackendOptionName } from './catalogState';
import { parseModelOptions } from './recipeForm';

export interface CatalogLoadContext {
  connection: ConnectionSnapshot;
  recipe: BackendRecipe;
  effective: Record<string, Scalar>;
}

type SystemRecipes = Record<string, {
  default_backend?: string;
  backends?: Record<string, { state?: string; message?: string }>;
}>;

const BUILTIN_SYSTEM_INFO_MAX_AGE_MS = 10_000;
const TRANSIENT_LOAD_KEYS = new Set(['pinned', 'save_options', 'model_name', 'dry_run']);

function modelRecipe(model: ModelInfo | null | undefined): string {
  return typeof model?.recipe === 'string' ? model.recipe : '';
}

export function catalogBackend(context: CatalogLoadContext, recipes?: SystemRecipes): string | null {
  const selector = backendSelector(context.recipe);
  if (!selector) return null;
  const value = own(context.effective, selector);
  const option = context.recipe.options.find(item => item.name === selector);
  if (value !== undefined && option && !isUnset(option, value)) {
    if (typeof value !== 'string') throw new Error('Invalid backend selector value.');
    return value;
  }
  return own(recipes, context.recipe.recipe)?.default_backend ?? null;
}

export function validateLoadOptions(recipe: BackendRecipe, body: Record<string, unknown>): void {
  for (const [key, value] of Object.entries(body)) {
    if (key === 'pinned' && typeof value === 'boolean') continue;
    if (key === 'save_options' && value === false) continue;
    const option = recipe.options.find(item => item.name === key);
    if (!option || (value !== null && validateValue(option, value as Scalar))) throw new Error(`Invalid option: ${key}`);
  }
}

async function systemRecipes(connection: ConnectionSnapshot): Promise<SystemRecipes | undefined> {
  const response = await api.fetchForConnection(connection, '/system-info', { cache: 'no-store' });
  if (!response.ok) throw new Error('Cannot read backend status from the selected server.');
  const info: unknown = await response.json();
  api.assertCurrentConnection(connection);
  return isRecord(info) && isRecord(info.recipes) ? info.recipes as SystemRecipes : undefined;
}

async function installSelectedBackend(connection: ConnectionSnapshot, recipe: string, backend: string): Promise<void> {
  api.assertCurrentConnection(connection);
  let failure: Error | undefined;
  let action = false;
  await api.installBackend(recipe, backend, {
    onProgress: data => { if (data.action) action = true; },
    onError: error => { failure = error; },
  });
  api.assertCurrentConnection(connection);
  if (failure) throw failure;
  if (action) throw new Error('Complete the server backend setup before loading.');
}

async function pullMissingModel(connection: ConnectionSnapshot, modelName: string): Promise<void> {
  api.assertCurrentConnection(connection);
  let failure: Error | undefined;
  await api.pullModel(modelName, { onError: error => { failure = error; } });
  api.assertCurrentConnection(connection);
  if (failure) throw failure;
}

export async function ensureCatalogModelReady(
  modelName: string,
  model: ModelInfo | null | undefined,
  body: Record<string, unknown>,
  context: CatalogLoadContext,
): Promise<void> {
  const { connection, recipe } = context;
  const guard = () => api.assertCurrentConnection(connection);
  guard();
  if (recipe.incompatibility || modelRecipe(model) !== recipe.recipe) throw new Error('Model recipe or option contract changed.');
  validateLoadOptions(recipe, body);
  const recipes = await systemRecipes(connection);
  const backend = catalogBackend(context, recipes);
  if (backend) {
    const info = own(own(recipes, recipe.recipe)?.backends, backend);
    if (!info) throw new Error('Selected backend is unavailable on the selected server.');
    if (info.state === 'installable' || info.state === 'update_required') {
      await installSelectedBackend(connection, recipe.recipe, backend);
    } else if (info.state !== 'installed' && info.state !== 'update_available') {
      throw new Error(info.message || 'Backend setup is required on the selected server.');
    }
  }
  const response = await api.fetchForConnection(connection, `/models/${encodeURIComponent(modelName)}`);
  if (!response.ok) throw new Error('Cannot verify the selected model.');
  const current: unknown = await response.json();
  guard();
  if (!isRecord(current) || current.recipe !== recipe.recipe) throw new Error('Model recipe changed; reopen its options.');
  if (!current.downloaded) await pullMissingModel(connection, modelName);
  await api.loadModelForConnection(connection, modelName, body);
  guard();
}

function persistentOptions(body: Record<string, unknown>): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(body)) if (!TRANSIENT_LOAD_KEYS.has(key)) result[key] = value;
  return result;
}

export async function loadThroughCatalog(
  modelName: string,
  model: ModelInfo | null | undefined,
  overrides: Record<string, unknown> = {},
  modelDefaults: Record<string, unknown> = {},
): Promise<boolean> {
  const recipeName = modelRecipe(model);
  if (!recipeName) return false;
  const state = await refreshBackendCatalog();
  const recipe = state.status === 'ready' ? state.catalog.recipes.find(row => row.recipe === recipeName) : undefined;
  if (!recipe || state.status !== 'ready' || recipe.source !== 'external') return false;
  const { connection } = state;
  const contract = new Set(recipe.options.map(option => option.name));
  const body = {
    ...Object.fromEntries(Object.entries(modelDefaults).filter(([key]) => contract.has(key) || key === 'pinned')),
    ...overrides,
  };
  const response = await api.fetchForConnection(connection, `/models/${encodeURIComponent(modelName)}/options`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...persistentOptions(body), dry_run: true }),
  });
  if (!response.ok) throw new Error('The server rejected model options.');
  const snapshot = parseModelOptions(await response.json(), recipeName, modelName);
  api.assertCurrentConnection(connection);
  await ensureCatalogModelReady(modelName, model, body, { connection, recipe, effective: snapshot.effective });
  return true;
}

export type BuiltinPreflight =
  | { outcome: 'skipped'; reason: 'no-schema' | 'not-builtin' }
  | { outcome: 'server-validation'; reason: 'dry-run-rejected' | 'dry-run-unavailable' }
  | { outcome: 'prepared'; backend: string | null; installed: boolean; pulled: boolean };

function systemBackendOption(recipes: SystemRecipes | undefined, recipe: string): {
  name: string | undefined;
  selectable: boolean;
} {
  const row = own(recipes as Record<string, unknown> | undefined, recipe);
  const options = isRecord(row) && Array.isArray(row.options) ? row.options : [];
  const explicit = options.find(option => isRecord(option) && option.type_name === 'BACKEND');
  return {
    name: isRecord(explicit) && typeof explicit.name === 'string' ? explicit.name : undefined,
    selectable: isRecord(row) && row.selectable_backend === true,
  };
}

// /load ignores keys it does not know and accepts values /options refuses (a
// fractional value for a whole-number option, `pinned`), so a rejected dry run
// must not stop a built-in load: the server validates it instead.
export async function prepareBuiltinLoad(
  modelName: string,
  model: ModelInfo | null | undefined,
  body: Record<string, unknown>,
): Promise<BuiltinPreflight> {
  const state = catalogState();
  if (state.status !== 'ready') return { outcome: 'skipped', reason: 'no-schema' };
  const recipe = catalogRecipe(modelRecipe(model));
  if (!recipe || recipe.source !== 'builtin') return { outcome: 'skipped', reason: 'not-builtin' };
  const { connection } = state;
  api.assertCurrentConnection(connection);
  const contract = new Set(recipe.options.map(option => option.name));
  const changes = Object.fromEntries(Object.entries(persistentOptions(body))
    .filter(([key]) => contract.has(key) && key !== 'recipe'));
  let response: Response;
  try {
    response = await api.fetchForConnection(connection, `/models/${encodeURIComponent(modelName)}/options`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...changes, dry_run: true }),
    });
  } catch (error) {
    api.assertCurrentConnection(connection);
    return { outcome: 'server-validation', reason: 'dry-run-unavailable' };
  }
  if (!response.ok) {
    return { outcome: 'server-validation', reason: response.status === 400 ? 'dry-run-rejected' : 'dry-run-unavailable' };
  }
  let effective: Record<string, Scalar>;
  try {
    effective = parseModelOptions(await response.json(), recipe.recipe, modelName).effective;
  } catch {
    api.assertCurrentConnection(connection);
    return { outcome: 'server-validation', reason: 'dry-run-unavailable' };
  }
  api.assertCurrentConnection(connection);
  const resolve = (recipes: SystemRecipes | undefined): string | null => {
    const fromSystem = systemBackendOption(recipes, recipe.recipe);
    const selector = resolveBackendOptionName(recipe.recipe, fromSystem.name, fromSystem.selectable);
    const requested = selector ? own(effective, selector) : undefined;
    const option = selector ? recipe.options.find(item => item.name === selector) : undefined;
    return typeof requested === 'string' && requested && !(option && isUnset(option, requested))
      ? requested
      : own(recipes, recipe.recipe)?.default_backend ?? null;
  };
  const cached = await api.systemInfo({ maxAgeMs: BUILTIN_SYSTEM_INFO_MAX_AGE_MS });
  api.assertCurrentConnection(connection);
  let recipes = isRecord(cached.recipes) ? cached.recipes as SystemRecipes : undefined;
  let backend = resolve(recipes);
  // A cached snapshot may predate an install, so only "installed" is taken from it.
  if (backend && own(own(recipes, recipe.recipe)?.backends, backend)?.state !== 'installed') {
    recipes = await systemRecipes(connection);
    backend = resolve(recipes);
  }
  let installed = false;
  if (backend) {
    const info = own(own(recipes, recipe.recipe)?.backends, backend);
    if (info?.state === 'unsupported') {
      throw new Error(info.message || `Backend ${backend} is not supported on the selected server.`);
    }
    if (info?.state === 'installable' || info?.state === 'update_required') {
      await installSelectedBackend(connection, recipe.recipe, backend);
      installed = true;
    }
  }
  let pulled = false;
  if (model?.downloaded === false) {
    const detail = await api.fetchForConnection(connection, `/models/${encodeURIComponent(modelName)}`);
    const current: unknown = detail.ok ? await detail.json() : null;
    api.assertCurrentConnection(connection);
    if (isRecord(current) && current.downloaded === false) {
      await pullMissingModel(connection, modelName);
      pulled = true;
    }
  }
  return { outcome: 'prepared', backend, installed, pulled };
}
