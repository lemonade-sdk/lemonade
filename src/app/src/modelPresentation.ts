import { getBackendCatalogSnapshot } from './features/backends/backendCatalogStore';

/**
 * Recipe names come from lemond, and so do their labels: `web_display_name`
 * falling back to `display_name`. These overrides exist only where the server's
 * docs-facing vocabulary is wrong for a UI chip — it bakes the device into the
 * name ("llama.cpp GPU", "Ryzen AI SW NPU"), repeats the experimental flag
 * ("vLLM ROCm (experimental)"), or is a lowercase website slug rather than a
 * name ("thenoise") — plus the three `collection*` pseudo-recipes,
 * which are client-side and have no server counterpart by construction.
 *
 * This is an override map, never an allowlist: a recipe that is not listed
 * still resolves, using the name the server published for it.
 *
 * Backend identity is a text label, never a color: see the
 * model-nav-filter-layout and model-list-backend-layout contracts.
 */
const BRAND_OVERRIDES: Record<string, { label?: string; compact?: string }> = {
  llamacpp: { label: 'llama.cpp', compact: 'llama.cpp' },
  vllm: { label: 'vLLM', compact: 'vLLM' },
  flm: { label: 'FastFlowLM', compact: 'FLM' },
  'ryzenai-llm': { label: 'RyzenAI', compact: 'RyzenAI' },
  'sd-cpp': { compact: 'SD.cpp' },
  thenoise: { label: 'TheNoise', compact: 'TheNoise' },
  whispercpp: { compact: 'Whisper' },
  kokoro: { label: 'Kokoro TTS', compact: 'Kokoro' },
  openmoss: { compact: 'OpenMOSS' },
  'collection.omni': { label: 'Omni Collection', compact: 'Omni' },
  'collection.router': { label: 'Router', compact: 'Router' },
  collection: { label: 'Collection', compact: 'Collection' },
};

function publishedLabel(recipe: string): string {
  const descriptor = getBackendCatalogSnapshot().catalog?.byRecipe.get(recipe);
  return descriptor?.webDisplayName || descriptor?.displayName || '';
}

export function backendLabel(recipe: string): string {
  const key = String(recipe || '').toLowerCase();
  return BRAND_OVERRIDES[key]?.label || publishedLabel(key) || recipe || 'Unknown';
}

export function backendCompactLabel(recipe: string): string {
  const key = String(recipe || '').toLowerCase();
  const override = BRAND_OVERRIDES[key];
  return override?.compact || override?.label || publishedLabel(key) || recipe || 'Backend';
}

/** Capability targets that carry a --cap-* identity token in both themes. */
const CAPABILITY_COLOR_TOKENS = new Set([
  'chat', 'omni', 'router', 'vision', 'code', 'embedding', 'reranking',
  'image', 'image-edit', 'audio', 'audio-generation', 'tts', 'model3d',
]);

/** Undefined for targets with no identity token, so the glyph stays neutral. */
export function capabilityColor(capability: string): string | undefined {
  return CAPABILITY_COLOR_TOKENS.has(capability) ? `var(--cap-${capability})` : undefined;
}
