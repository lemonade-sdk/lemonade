type ModelLabels = { labels?: string[] };

/**
 * Legacy broad non-LLM label set kept for callers that need to exclude any
 * model marked as audio-only or tool-only. New planner selection should use
 * isChatPlannerCandidate instead, because some chat-capable multimodal LLMs
 * legitimately carry labels such as "audio" and "vision".
 */
export const NON_LLM_LABELS = new Set([
  'image',
  'speech',
  'tts',
  'audio',
  'transcription',
  'embeddings',
  'embedding',
  'reranking',
]);

/**
 * Labels that identify concrete non-chat components. "audio" and "vision" are
 * intentionally allowed here: a multimodal chat LLM may support those inputs
 * while still being the correct planner for a collection or custom collection.
 */
export const NON_CHAT_PLANNER_LABELS = new Set([
  'image',
  'speech',
  'tts',
  'transcription',
  'embeddings',
  'embedding',
  'reranking',
  'edit',
  'esrgan',
]);

export const hasAnyModelLabel = (info: ModelLabels | undefined, requiredLabels: string[]): boolean => {
  if (requiredLabels.length === 0) return false;
  const required = new Set(requiredLabels);
  return (info?.labels ?? []).some((label) => required.has(label));
};

export const isChatPlannerCandidate = (info: ModelLabels | undefined): boolean => {
  return !(info?.labels ?? []).some((label) => NON_CHAT_PLANNER_LABELS.has(label));
};
