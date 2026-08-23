import type { ModelInfo, ModelsData } from './modelData';
import { COLLECTION_ROUTER_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';
import { isChatPlannerCandidate } from './modelLabels';

export { NON_LLM_LABELS, NON_CHAT_PLANNER_LABELS, isChatPlannerCandidate } from './modelLabels';

export const getCollectionComponents = (info?: ModelInfo): string[] => {
  const components = info?.components;
  if (!Array.isArray(components)) {
    return [];
  }
  return components.filter((name): name is string => typeof name === 'string' && name.length > 0);
};

export const isCollectionModel = (info?: ModelInfo): boolean => {
  return !!info && isCollectionRecipe(info.recipe) && getCollectionComponents(info).length > 0;
};

export const isModelEffectivelyDownloaded = (modelName: string, info: ModelInfo | undefined, modelsData: ModelsData): boolean => {
  if (isCollectionModel(info)) {
    return isCollectionFullyDownloaded(modelName, modelsData);
  }
  return info?.downloaded === true;
};

export const isModelEffectivelyLoaded = (
  modelName: string,
  info: ModelInfo | undefined,
  modelsData: ModelsData,
  loadedModels: Set<string>,
): boolean => {
  if (isCollectionModel(info)) {
    return isCollectionFullyLoaded(modelName, modelsData, loadedModels);
  }
  return loadedModels.has(modelName);
};

export const isCollectionFullyDownloaded = (modelName: string, modelsData: ModelsData): boolean => {
  const info = modelsData[modelName];
  const components = getCollectionComponents(info);
  if (components.length === 0) return false;
  return components.every((component) => modelsData[component]?.downloaded === true);
};

export const isCollectionFullyLoaded = (
  modelName: string,
  modelsData: ModelsData,
  loadedModels: Set<string>,
): boolean => {
  const info = modelsData[modelName];
  const components = getCollectionComponents(info);
  if (components.length === 0) return false;
  return components.every((component) => loadedModels.has(component));
};

export const getCollectionImageModel = (selectedModel: string, modelsData: ModelsData): string | null => {
  const info = modelsData[selectedModel];
  const components = getCollectionComponents(info);
  const imageModel = components.find((component) => {
    const componentInfo = modelsData[component];
    return componentInfo?.labels?.includes('image');
  });
  return imageModel || null;
};

export const isRouterCollection = (info?: ModelInfo): boolean => {
  return info?.recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

export const getCollectionPrimaryChatModel = (selectedModel: string, modelsData: ModelsData): string => {
  const info = modelsData[selectedModel];
  // A collection.router must be addressed by its own name: the server's routing
  // engine picks the component per request, so resolving to a component here
  // would silently bypass routing.
  if (isRouterCollection(info)) {
    return selectedModel;
  }
  const components = getCollectionComponents(info);
  if (components.length === 0) {
    return selectedModel;
  }

  const explicitLLM = components.find((component) => isChatPlannerCandidate(modelsData[component]));
  return explicitLLM || components[0];
};
