import type { ConnectionSnapshot } from '../../api';
import { BackendRecipe, BackendSchema, backendSelector, own } from './backendSchema';
import { setRecipeSchemaSource } from './recipeMetadata';

export type CatalogState =
  | { status: 'idle' | 'loading'; connection: ConnectionSnapshot | null }
  | { status: 'ready'; connection: ConnectionSnapshot; catalog: BackendSchema }
  | { status: 'missing' | 'authentication' | 'unreachable' | 'incompatible'; connection: ConnectionSnapshot; message: string };

export const LEGACY_OPTIONS_NOTICE = 'Legacy options: this server has no backend schema endpoint.';

let state: CatalogState = { status: 'idle', connection: null };
let recipes: Record<string, BackendRecipe> = Object.create(null);
let systemNames: Record<string, string> = Object.create(null);
const listeners = new Set<() => void>();
let version = 0;

export function catalogVersion(): number {
  return version;
}

function publish(): void {
  version += 1;
  listeners.forEach(listener => { try { listener(); } catch {} });
}

export function catalogState(): CatalogState {
  return state;
}

export function subscribeCatalog(listener: () => void): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

export function setCatalogState(next: CatalogState): void {
  state = next;
  recipes = Object.create(null);
  if (next.status === 'ready') for (const row of next.catalog.recipes) recipes[row.recipe] = row;
  publish();
}

export function catalogRecipe(recipe: string): BackendRecipe | undefined {
  return own(recipes, recipe);
}

export function catalogRecipes(): BackendRecipe[] {
  return state.status === 'ready' ? state.catalog.recipes : [];
}

export function setSystemRecipeNames(systemInfo: unknown): void {
  const next: Record<string, string> = Object.create(null);
  const rows = systemInfo && typeof systemInfo === 'object' ? (systemInfo as { recipes?: unknown }).recipes : null;
  if (rows && typeof rows === 'object' && !Array.isArray(rows)) {
    for (const [recipe, info] of Object.entries(rows as Record<string, { display_name?: unknown } | null>)) {
      if (typeof info?.display_name === 'string' && info.display_name) next[recipe] = info.display_name;
    }
  }
  systemNames = next;
  publish();
}

export function clearServerRecipeNames(): void {
  systemNames = Object.create(null);
  publish();
}

export function serverRecipeDisplayName(recipe: string): string | undefined {
  const key = String(recipe || '');
  const name = own(recipes, key)?.display_name || own(systemNames, key)
    || own(recipes, key.toLowerCase())?.display_name || own(systemNames, key.toLowerCase());
  return name || undefined;
}

export function resolveBackendOptionName(
  recipe: string,
  systemOptionName: string | undefined,
  selectableBackend: boolean,
): string | null {
  const row = catalogRecipe(recipe);
  if (row) {
    const selector = backendSelector(row);
    if (selector) return selector;
    if (row.source === 'external') return null;
  }
  if (systemOptionName) return systemOptionName;
  if (!selectableBackend) return null;
  const conventional = `${recipe}_backend`;
  if (row) return row.options.some(option => option.name === conventional) ? conventional : null;
  return conventional;
}

setRecipeSchemaSource({
  backendOptionName: resolveBackendOptionName,
  option: (recipe, key) => catalogRecipe(recipe)?.options.find(option => option.name === key),
});
