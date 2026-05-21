export const COLLECTION_OMNI_MODEL_RECIPE = 'collection.omni';

export const isCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE;
};

export const RECIPE_DISPLAY_NAMES: Record<string, string> = {
  [COLLECTION_OMNI_MODEL_RECIPE]: 'Lemonade',
  'flm': 'FastFlowLM NPU',
  'llamacpp': 'Llama.cpp GPU',
  'ryzenai-llm': 'Ryzen AI LLM',
  'whispercpp': 'Whisper.cpp',
  'sd-cpp': 'StableDiffusion.cpp',
  'kokoro': 'Kokoro',
  'vllm': 'vLLM ROCm (experimental)',
};
