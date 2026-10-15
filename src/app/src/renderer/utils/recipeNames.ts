import { useSyncExternalStore } from 'react';
import { ConnectionSnapshot, serverConfig } from './serverConfig';

export const COLLECTION_OMNI_MODEL_RECIPE = 'collection.omni';
export const COLLECTION_ROUTER_MODEL_RECIPE = 'collection.router';

export const isCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

export const isModelCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

const seeds: Record<string, string> = {
  [COLLECTION_OMNI_MODEL_RECIPE]: 'Lemonade',
  [COLLECTION_ROUTER_MODEL_RECIPE]: 'Lemonade Router',
  cloud: 'Cloud',
  vllm: 'vLLM ROCm (experimental)',
};
let sources = new Map<string, Record<string, { display_name?: string }>>();
let snapshot: Readonly<Record<string, string>> = Object.freeze(Object.assign(Object.create(null), seeds));
const listeners = new Set<() => void>();
const publish = () => {
  const next = Object.assign(Object.create(null), seeds);
  for (const recipes of sources.values()) for (const [recipe, info] of Object.entries(recipes)) {
    if (typeof info?.display_name === 'string' && info.display_name) next[recipe] = info.display_name;
  }
  snapshot = Object.freeze(next);
  listeners.forEach(listener => listener());
};
serverConfig.onConnectionChange(() => { sources = new Map(); publish(); });
export function replaceRecipeDisplayNames(source: string, recipes: Record<string, { display_name?: string }> | undefined, connection: ConnectionSnapshot): void {
  serverConfig.assertCurrent(connection);
  if (recipes) sources.set(source, recipes);
  else sources.delete(source);
  publish();
}
export function useRecipeDisplayNames(): Readonly<Record<string, string>> {
  return useSyncExternalStore(listener => { listeners.add(listener); return () => { listeners.delete(listener); }; }, () => snapshot);
}
