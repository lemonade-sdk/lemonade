export const HUGGING_FACE_SEARCH_LIMIT = 12;

const EXCLUDED_PIPELINE_TAGS = new Set([
  'automatic-speech-recognition',
  'text-to-speech',
  'audio-text-to-text',
  'text-to-audio',
  'audio-to-audio',
  'voice-activity-detection',
  'text-to-image',
  'image-to-image',
  'image-to-video',
  'image-to-3d',
  'image-text-to-image',
  'image-text-to-video',
  'unconditional-image-generation',
  'image-segmentation',
  'object-detection',
  'depth-estimation',
  'mask-generation',
  'zero-shot-object-detection',
  'text-to-video',
  'text-to-3d',
  'video-to-video',
]);

const EXCLUDED_RECIPES = new Set(['sd-cpp', 'whispercpp', 'moonshine']);

interface HuggingFaceVariantResult {
  recipe?: string;
  variants?: unknown[];
}

interface HuggingFaceRepositoryMetadata {
  tags?: string[];
  siblings?: Array<{ rfilename?: string }>;
}

export function filterHuggingFaceSearchResults<T extends { pipeline_tag?: string }>(results: T[]): T[] {
  return results
    .slice(0, HUGGING_FACE_SEARCH_LIMIT)
    .filter(result => !result.pipeline_tag || !EXCLUDED_PIPELINE_TAGS.has(result.pipeline_tag.toLowerCase()));
}

export function isCompatibleHuggingFaceVariantResult(result: HuggingFaceVariantResult | null | undefined): boolean {
  if (!result?.variants?.length) return false;
  return isCompatibleHuggingFaceRecipe(result.recipe);
}

export function isCompatibleHuggingFaceRecipe(recipe: string | null | undefined): boolean {
  return Boolean(recipe) && !EXCLUDED_RECIPES.has(String(recipe).trim().toLowerCase());
}

export function detectHuggingFaceBackendFromMetadata(
  modelId: string,
  metadata: HuggingFaceRepositoryMetadata,
): string | null {
  const id = modelId.toLowerCase();
  const tags = (metadata.tags || []).map(tag => tag.toLowerCase());
  const files = (metadata.siblings || [])
    .map(sibling => String(sibling.rfilename || '').toLowerCase())
    .filter(Boolean);

  if (id.startsWith('fastflowlm/') || tags.includes('flm') || files.some(file => file.endsWith('.flm'))) {
    return 'flm';
  }
  if (id.includes('moonshine') && files.some(file => file.endsWith('.onnx'))) {
    return 'moonshine';
  }
  if (files.some(file => file.endsWith('.onnx') || file.endsWith('.onnx_data'))) {
    return 'ryzenai-llm';
  }
  if ((tags.includes('whisper') || id.includes('whisper')) && files.some(file => file.endsWith('.bin'))) {
    return 'whispercpp';
  }
  if (tags.includes('stable-diffusion')
    || tags.includes('text-to-image')
    || id.includes('stable-diffusion')
    || id.includes('flux')) {
    return 'sd-cpp';
  }
  return null;
}

export async function detectHuggingFaceBackend(
  modelId: string,
  signal?: AbortSignal,
): Promise<string | null> {
  const response = await fetch(`https://huggingface.co/api/models/${modelId}`, { signal });
  if (!response.ok) throw new Error(`Failed to inspect Hugging Face repository (${response.status})`);
  return detectHuggingFaceBackendFromMetadata(modelId, await response.json());
}
