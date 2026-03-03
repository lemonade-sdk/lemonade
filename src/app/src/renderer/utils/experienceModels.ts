import { ModelInfo, ModelsData } from './modelData';

const NON_LLM_LABELS = new Set(['image', 'speech', 'tts', 'audio', 'transcription', 'embeddings', 'embedding', 'reranking']);

export const getExperienceComponents = (info?: ModelInfo): string[] => {
  if (!info || !Array.isArray(info.composite_models)) {
    return [];
  }
  return info.composite_models.filter((name): name is string => typeof name === 'string' && name.length > 0);
};

export const isExperienceModel = (info?: ModelInfo): boolean => {
  return !!info && info.recipe === 'experience' && getExperienceComponents(info).length > 0;
};

export const isExperienceFullyDownloaded = (modelName: string, modelsData: ModelsData): boolean => {
  const info = modelsData[modelName];
  const components = getExperienceComponents(info);
  if (components.length === 0) return false;
  return components.every((component) => modelsData[component]?.downloaded === true);
};

export const isExperienceFullyLoaded = (
  modelName: string,
  modelsData: ModelsData,
  loadedModels: Set<string>,
): boolean => {
  const info = modelsData[modelName];
  const components = getExperienceComponents(info);
  if (components.length === 0) return false;
  return components.every((component) => loadedModels.has(component));
};

export const getExperiencePrimaryChatModel = (selectedModel: string, modelsData: ModelsData): string => {
  const info = modelsData[selectedModel];
  const components = getExperienceComponents(info);
  if (components.length === 0) {
    return selectedModel;
  }

  const explicitLLM = components.find((component) => {
    const componentInfo = modelsData[component];
    const labels = componentInfo?.labels ?? [];
    return !labels.some((label) => NON_LLM_LABELS.has(label));
  });

  return explicitLLM || components[0];
};
