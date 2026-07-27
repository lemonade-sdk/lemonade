const BACKEND_PRESENTATION: Record<string, { compact: string; label: string; color: string }> = {
  llamacpp: { compact: 'llama.cpp', label: 'llama.cpp', color: 'var(--backend-llamacpp)' },
  vllm: { compact: 'vLLM', label: 'vLLM', color: 'var(--backend-vllm)' },
  flm: { compact: 'FLM', label: 'FastFlowLM', color: 'var(--backend-flm)' },
  'ryzenai-llm': { compact: 'RyzenAI', label: 'RyzenAI', color: 'var(--backend-ryzenai)' },
  'sd-cpp': { compact: 'SD.cpp', label: 'Stable Diffusion', color: 'var(--backend-sd-cpp)' },
  whispercpp: { compact: 'Whisper', label: 'Whisper', color: 'var(--backend-whispercpp)' },
  moonshine: { compact: 'Moonshine', label: 'Moonshine', color: 'var(--backend-moonshine)' },
  kokoro: { compact: 'Kokoro', label: 'Kokoro TTS', color: 'var(--backend-kokoro)' },
  acestep: { compact: 'ACE-Step', label: 'ACE-Step', color: 'var(--backend-acestep)' },
  thinksound: { compact: 'ThinkSound', label: 'ThinkSound', color: 'var(--backend-thinksound)' },
  openmoss: { compact: 'OpenMOSS', label: 'OpenMOSS TTS', color: 'var(--backend-openmoss)' },
  trellis: { compact: 'TRELLIS.2', label: 'TRELLIS.2', color: 'var(--backend-trellis)' },
  'collection.omni': { compact: 'Omni', label: 'Omni Collection', color: 'var(--backend-collection-omni)' },
  'collection.router': { compact: 'Router', label: 'Router', color: 'var(--backend-collection-router)' },
  collection: { compact: 'Collection', label: 'Collection', color: 'var(--backend-collection)' },
};

function backendPresentation(recipe: string) {
  const normalized = String(recipe || '').toLowerCase();
  return BACKEND_PRESENTATION[normalized];
}

export function backendCompactLabel(recipe: string): string {
  return backendPresentation(recipe)?.compact || recipe || 'Backend';
}

export function backendLabel(recipe: string): string {
  return backendPresentation(recipe)?.label || recipe || 'Unknown';
}

export function backendColor(recipe: string): string {
  return backendPresentation(recipe)?.color || 'var(--backend-other)';
}
