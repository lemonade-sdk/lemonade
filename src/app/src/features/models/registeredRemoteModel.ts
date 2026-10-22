import type { ModelInfo, ModelRegistryProvider, PullVariantsResult } from '../../api';

/** Reuse lemond's existing model ID when its recorded GGUF exactly matches the selected file. */
export function findRegisteredRemoteModel(
  models: readonly ModelInfo[],
  provider: ModelRegistryProvider,
  repository: string,
  variantName: string,
  recipe: string,
  details?: PullVariantsResult,
): ModelInfo | undefined {
  if (recipe.toLowerCase() !== 'llamacpp' || details?.mmproj_files?.length) return undefined;
  const file = details?.variants?.find(variant => variant.name === variantName)?.primary_file;
  if (!file) return undefined;

  const checkpoints = new Set([`${repository}:${variantName}`, `${repository}:${file}`]);
  return models.find(model => {
    const name = String(model.model_name || model.name || model.id || '');
    const source = String(model.source || '').toLowerCase();
    const registry = String(model.registry_source || source || 'huggingface').toLowerCase();
    if (!name || name.startsWith('extra.') ||
        (source && source !== 'huggingface' && source !== 'modelscope')) return false;
    if (registry !== provider || String(model.recipe || '').toLowerCase() !== 'llamacpp') return false;

    // A matching main file alone does not establish equivalence for multimodal models.
    const extraFiles = model.checkpoints;
    if (extraFiles && typeof extraFiles === 'object' &&
        Object.keys(extraFiles).some(role => role !== 'main')) return false;
    if (model.mmproj) return false;
    const main = extraFiles && typeof extraFiles === 'object' && 'main' in extraFiles
      ? extraFiles.main : '';
    return checkpoints.has(String(model.checkpoint || main || ''));
  });
}
