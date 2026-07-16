import type { HFModelResult, ModelInfo, PullVariantsResult } from './api';
import type { ModelCapability } from './modelCapabilities';

export type RemoteCapabilityConfidence = 'repository' | 'provider' | 'unknown';

export type RemoteCapabilityEvidence = {
  labels: string[];
  primary: ModelCapability;
  confidence: RemoteCapabilityConfidence;
};

const REMOTE_FEATURE_LABELS = new Map<string, string>([
  ['tool', 'tool-calling'], ['tools', 'tool-calling'], ['tool-use', 'tool-calling'],
  ['tool-calling', 'tool-calling'], ['function-calling', 'tool-calling'],
  ['reasoning', 'reasoning'], ['thinking', 'reasoning'], ['reasoner', 'reasoning'],
  ['code', 'coding'], ['coding', 'coding'], ['coder', 'coding'],
]);

function normalizedStringList(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .filter((item): item is string => typeof item === 'string')
    .map(item => item.trim().toLowerCase())
    .filter(Boolean);
}

function flattenMetadata(value: unknown, prefix = '', out: Record<string, unknown> = {}): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return out;
  for (const [key, child] of Object.entries(value as Record<string, unknown>)) {
    const path = prefix ? `${prefix}.${key}` : key;
    out[path.toLowerCase()] = child;
    if (child && typeof child === 'object' && !Array.isArray(child)) flattenMetadata(child, path, out);
  }
  return out;
}

function metadataValue(metadata: Record<string, unknown>, keys: string[]): unknown {
  for (const key of keys) {
    const value = metadata[key.toLowerCase()];
    if (value !== undefined && value !== null) return value;
  }
  return undefined;
}

function hasMeaningfulValue(value: unknown): boolean {
  if (typeof value === 'string') {
    const normalized = value.trim().toLowerCase();
    return Boolean(normalized) && !['none', 'null', 'false', '0', 'unknown'].includes(normalized);
  }
  if (typeof value === 'number') return Number.isFinite(value) && value !== 0;
  if (typeof value === 'boolean') return value;
  if (Array.isArray(value)) return value.length > 0;
  return Boolean(value);
}

/**
 * Derive remote model capabilities from structured evidence only.
 *
 * Evidence order:
 * 1. Repository/GGUF metadata returned by /pull/variants.
 * 2. Provider task/capability/modality fields returned by /registry/search.
 * 3. Unknown.
 *
 * Repository IDs, display names and the user's search query are deliberately
 * excluded. This keeps filters deterministic and prevents names such as
 * "my-embedding-chat-test" from being treated as proof of a capability.
 */
export function remoteCapabilityEvidence(
  result: HFModelResult,
  variants?: PullVariantsResult,
): RemoteCapabilityEvidence {
  const labels = new Set<string>();
  const providerTags = normalizedStringList(result.tags);
  const providerCapabilities = normalizedStringList(result.capabilities);
  const variantCapabilities = normalizedStringList(variants?.capabilities);
  const inputModalities = new Set([
    ...normalizedStringList(result.input_modalities),
    ...normalizedStringList(variants?.input_modalities),
  ]);
  const outputModalities = new Set([
    ...normalizedStringList(result.output_modalities),
    ...normalizedStringList(variants?.output_modalities),
  ]);
  const metadata = flattenMetadata({
    ...(result.metadata || {}),
    ...(variants?.metadata || {}),
    ...(variants?.gguf_metadata || {}),
  });

  const addCanonicalFeature = (raw: string) => {
    const mapped = REMOTE_FEATURE_LABELS.get(raw.trim().toLowerCase());
    if (mapped) labels.add(mapped);
  };
  for (const tag of providerTags) addCanonicalFeature(tag);
  for (const label of normalizedStringList(variants?.suggested_labels)) addCanonicalFeature(label);

  const pipeline = String(result.pipeline_tag || '').trim().toLowerCase();
  // Provider tags are accepted only as exact canonical values below. There is
  // no substring scan and no use of the repository/display name.
  const declared = new Set([
    ...providerCapabilities,
    ...variantCapabilities,
    ...providerTags,
    pipeline,
  ].filter(Boolean));
  const hasMmproj = (variants?.mmproj_files?.length || 0) > 0;
  const hasUsableGguf = variants?.repo_kind === 'gguf' && (variants.variants?.length || 0) > 0;
  const chatTemplate = metadataValue(metadata, [
    'tokenizer.chat_template', 'tokenizer.ggml.chat_template', 'chat_template',
  ]);
  const poolingType = metadataValue(metadata, [
    'pooling_type', 'llama.pooling_type', 'bert.pooling_type', 'nomic-bert.pooling_type',
  ]);
  const embeddingLength = metadataValue(metadata, [
    'embedding_length', 'sentence_embedding_length', 'general.embedding_length',
  ]);
  const rerankerMarker = metadataValue(metadata, [
    'reranker', 'reranking', 'classifier.output_labels', 'classifier.labels', 'cross_encoder',
  ]);

  const providerEmbedding = outputModalities.has('embedding')
    || outputModalities.has('embeddings')
    || outputModalities.has('vector')
    || [...declared].some(value => [
      'embedding', 'embeddings', 'sentence-embedding', 'sentence-similarity',
      'feature-extraction', 'text-embedding', 'multi-modal-embedding',
      'generative-multi-modal-embedding',
    ].includes(value));
  const providerReranking = outputModalities.has('ranking')
    || outputModalities.has('scores')
    || [...declared].some(value => [
      'reranking', 'reranker', 'text-ranking', 'cross-encoder',
    ].includes(value));
  const providerChat = (outputModalities.has('text')
      && (inputModalities.has('text') || inputModalities.has('image') || inputModalities.has('vision')))
    || [...declared].some(value => [
      'chat', 'llm', 'text-generation', 'text2text-generation', 'conversational',
      'image-text-to-text', 'multimodal-dialogue', 'visual-question-answering',
    ].includes(value));
  const providerVision = hasMmproj
    || inputModalities.has('image')
    || inputModalities.has('vision')
    || [...declared].some(value => [
      'vision', 'vision-language', 'vlm', 'image-input', 'image-text-to-text',
      'visual-question-answering', 'multimodal-dialogue', 'multi-modal', 'multimodal',
    ].includes(value));

  // Repository/GGUF evidence is authoritative. Provider fields are fallback.
  const repositoryEmbedding = hasMeaningfulValue(poolingType) || hasMeaningfulValue(embeddingLength);
  const repositoryReranking = hasMeaningfulValue(rerankerMarker);
  const repositoryChat = hasMeaningfulValue(chatTemplate);
  // A validated llama.cpp GGUF plus an mmproj is repository-structure proof of
  // a vision-language model. It does not depend on the repository name or tags.
  const repositoryOmni = hasUsableGguf && hasMmproj && variants?.recipe === 'llamacpp';

  let primary: ModelCapability = 'unknown';
  let confidence: RemoteCapabilityConfidence = 'unknown';
  if (repositoryReranking) {
    primary = 'reranking';
    confidence = 'repository';
  } else if (repositoryEmbedding) {
    primary = 'embedding';
    confidence = 'repository';
  } else if (repositoryOmni) {
    primary = 'omni';
    confidence = 'repository';
  } else if (repositoryChat && providerVision) {
    primary = 'omni';
    confidence = 'repository';
  } else if (repositoryChat) {
    primary = 'chat';
    confidence = 'repository';
  } else if (providerReranking) {
    primary = 'reranking';
    confidence = 'provider';
  } else if (providerEmbedding) {
    primary = 'embedding';
    confidence = 'provider';
  } else if (providerChat && providerVision) {
    primary = 'omni';
    confidence = 'provider';
  } else if (providerChat) {
    primary = 'chat';
    confidence = 'provider';
  }

  if (primary !== 'unknown') {
    labels.add(primary === 'embedding' ? 'embeddings' : primary);
  }
  if (providerVision) labels.add('vision');

  return { labels: [...labels], primary, confidence };
}

export function remoteResultAsModelInfo(
  result: HFModelResult,
  variants?: PullVariantsResult,
): ModelInfo {
  const evidence = remoteCapabilityEvidence(result, variants);
  const source = result.source || 'huggingface';
  return {
    id: result.id,
    name: result.id,
    display_name: result.id,
    model_name: result.id,
    recipe: variants?.recipe || 'llamacpp',
    type: evidence.primary,
    capability: evidence.primary,
    capability_confidence: evidence.confidence,
    labels: evidence.labels,
    source,
    registry_source: source,
  } as ModelInfo;
}
