import type { DownloadedModel } from '../hooks/useModels';
import { isCollectionModel } from './collectionModels';
import { isCustomCollectionModel } from './customCollections';

/**
 * Downloaded models visible in chat selector and Model Manager inventory.
 * Excludes upscaling models and raw collection components; keeps collection
 * parents when suggested or custom.
 */
export function getVisibleDownloadedModels(
  downloadedModels: DownloadedModel[],
): DownloadedModel[] {
  return downloadedModels.filter((model) => {
    if (model.info?.labels?.includes('upscaling')) return false;
    if (!isCollectionModel(model.info)) {
      return true;
    }
    return model.info.suggested === true || isCustomCollectionModel(model.id, model.info);
  });
}
