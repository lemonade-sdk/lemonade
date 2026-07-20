export const COLLECTION_OMNI_MODEL_RECIPE = 'collection.omni';
export const COLLECTION_ROUTER_MODEL_RECIPE = 'collection.router';

export const isCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

export const isModelCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

// Recipe display names. Hardware-backend names (llamacpp, whispercpp, sd-cpp, …)
// are populated at runtime from /system-info's `recipes[].display_name`, which is
// generated from the C++ backend descriptors — the single source of truth. Only
// recipes NOT surfaced by /system-info's hardware support matrix are seeded here:
// the collection orchestrator (not a backend) and cloud offload (a backend with
// no local support rows).
export const RECIPE_DISPLAY_NAMES: Record<string, string> = {
  [COLLECTION_OMNI_MODEL_RECIPE]: 'Lemonade',
  [COLLECTION_ROUTER_MODEL_RECIPE]: 'Lemonade Router',
  'cloud': 'Cloud',
  'vllm': 'vLLM ROCm (experimental)',
};

// Merge display names from a /system-info `recipes` object into RECIPE_DISPLAY_NAMES.
// Called whenever system info is (re)fetched so the map reflects the descriptors.
export const updateRecipeDisplayNames = (
  recipes?: Record<string, { display_name?: string }>
): void => {
  if (!recipes) {
    return;
  }
  for (const [recipe, info] of Object.entries(recipes)) {
    if (info && typeof info.display_name === 'string' && info.display_name) {
      RECIPE_DISPLAY_NAMES[recipe] = info.display_name;
    }
  }
};
