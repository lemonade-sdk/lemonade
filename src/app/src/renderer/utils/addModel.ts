import type { ModelInstallData } from '../AddModelPanel';
import { USER_MODEL_PREFIX } from './modelData';

/**
 * Register a model from the Add Model form and return the id it will be
 * registered under. The Model Manager owns the install, so the caller only
 * needs the id to follow the model once the server reports it.
 */
export const installModelFromForm = (data: ModelInstallData): string => {
  // The server only accepts registrations in the `user.` namespace.
  const modelName = data.name.startsWith(USER_MODEL_PREFIX) ? data.name : `${USER_MODEL_PREFIX}${data.name}`;
  window.dispatchEvent(new CustomEvent('installModel', {
    detail: {
      name: modelName,
      registrationData: {
        checkpoint: data.checkpoint,
        checkpoints: data.checkpoints,
        recipe: data.recipe,
        // Omitted on "Automatic" so lemond applies its default_model_source.
        ...(data.source ? { source: data.source } : {}),
        mmproj: data.mmproj,
        labels: data.labels,
        reasoning: data.reasoning,
        vision: data.vision,
        embedding: data.embedding,
        reranking: data.reranking,
      },
    },
  }));
  return modelName;
};

export const installModelFromJSON = (json: unknown): void => {
  window.dispatchEvent(new CustomEvent('installModelFromJSON', { detail: json }));
};

/** The id a model JSON file registers under, or null when the file names none. */
export const getModelJSONName = (json: unknown): string | null => {
  if (!json || typeof json !== 'object' || Array.isArray(json)) return null;
  const record = json as Record<string, unknown>;
  if (typeof record.model_name === 'string' && record.model_name) return record.model_name;
  if (typeof record.id === 'string' && record.id) return record.id;
  return null;
};

/** Resolves to null on unreadable or malformed JSON. */
export const readModelJSONFile = (file: File): Promise<unknown | null> =>
  new Promise(resolve => {
    const reader = new FileReader();
    reader.onload = ev => {
      try {
        resolve(JSON.parse(ev.target?.result as string));
      } catch {
        resolve(null);
      }
    };
    reader.onerror = () => resolve(null);
    reader.readAsText(file);
  });
