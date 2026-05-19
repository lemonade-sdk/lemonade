export const COLLECTION_OMNI_MODEL_RECIPE = 'collection.omni-model';
export const LEGACY_COLLECTION_RECIPE = 'collection';

export const isCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === LEGACY_COLLECTION_RECIPE;
};

export const canonicalizeRecipe = (recipe: string): string => {
  return recipe === LEGACY_COLLECTION_RECIPE ? COLLECTION_OMNI_MODEL_RECIPE : recipe;
};

export const RECIPE_DISPLAY_NAMES: Record<string, string> = {
  [COLLECTION_OMNI_MODEL_RECIPE]: 'OmniRouter',
  [LEGACY_COLLECTION_RECIPE]: 'OmniRouter',
  'flm': 'FastFlowLM NPU',
  'llamacpp': 'Llama.cpp GPU',
  'ryzenai-llm': 'Ryzen AI LLM',
  'whispercpp': 'Whisper.cpp',
  'sd-cpp': 'StableDiffusion.cpp',
  'kokoro': 'Kokoro',
  'vllm': 'vLLM ROCm (experimental)',
};
