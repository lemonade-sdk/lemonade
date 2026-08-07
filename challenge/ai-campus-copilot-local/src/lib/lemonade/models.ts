import type { LemonadeModel } from "./schemas";

/**
 * Labels the Lemonade model registry uses for non-text-generation modalities.
 * Mirrors the label vocabulary in src/cpp/resources/server_models.json of the
 * parent repository.
 */
const EMBEDDING_LABELS = new Set(["embeddings", "embedding"]);

const NON_CHAT_LABELS = new Set([
  "embeddings",
  "embedding",
  "reranking",
  "image",
  "edit",
  "upscaling",
  "tts",
  "voice-design",
  "transcription",
  "realtime-transcription",
  "chat-transcription",
  "audio-generation",
  "classification",
  "3d",
]);

/** Recipes that Lemonade documents as supporting POST /v1/embeddings. */
const EMBEDDING_CAPABLE_RECIPES = new Set(["llamacpp", "flm"]);

export interface ClassifiedModel {
  id: string;
  checkpoint: string | null;
  recipe: string | null;
  sizeGb: number | null;
  maxContextWindow: number | null;
  labels: string[];
  isChatCapable: boolean;
  isEmbeddingCapable: boolean;
}

export function classifyModel(model: LemonadeModel): ClassifiedModel {
  const labels = model.labels ?? [];
  const recipe = model.recipe ?? null;
  const idLower = model.id.toLowerCase();

  const looksLikeEmbedding =
    labels.some((label) => EMBEDDING_LABELS.has(label)) || idLower.includes("embed");

  const isEmbeddingCapable =
    looksLikeEmbedding && (recipe === null || EMBEDDING_CAPABLE_RECIPES.has(recipe));

  const isChatCapable = !labels.some((label) => NON_CHAT_LABELS.has(label)) && !looksLikeEmbedding;

  return {
    id: model.id,
    checkpoint: model.checkpoint ?? null,
    recipe,
    sizeGb: model.size ?? null,
    maxContextWindow: model.max_context_window ?? null,
    labels,
    isChatCapable,
    isEmbeddingCapable,
  };
}

export function classifyModels(models: LemonadeModel[]): ClassifiedModel[] {
  return models.map(classifyModel);
}

export function chatModels(models: ClassifiedModel[]): ClassifiedModel[] {
  return models.filter((model) => model.isChatCapable);
}

export function embeddingModels(models: ClassifiedModel[]): ClassifiedModel[] {
  return models.filter((model) => model.isEmbeddingCapable);
}

export function formatModelSize(model: ClassifiedModel): string {
  return model.sizeGb === null ? "size unknown" : `${model.sizeGb.toFixed(2)} GB`;
}
