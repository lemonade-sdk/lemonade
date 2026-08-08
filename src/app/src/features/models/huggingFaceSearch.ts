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

export function filterHuggingFaceSearchResults<T extends { pipeline_tag?: string }>(results: T[]): T[] {
  return results
    .slice(0, HUGGING_FACE_SEARCH_LIMIT)
    .filter(result => !result.pipeline_tag || !EXCLUDED_PIPELINE_TAGS.has(result.pipeline_tag.toLowerCase()));
}

export function isCompatibleHuggingFaceVariantResult(result: HuggingFaceVariantResult | null | undefined): boolean {
  if (!result?.variants?.length) return false;
  return !EXCLUDED_RECIPES.has(String(result.recipe || '').trim().toLowerCase());
}
