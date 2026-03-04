import React from 'react';
import { useModels, DEFAULT_MODEL_ID } from '../hooks/useModels';
import { isExperienceModel } from '../utils/experienceModels';

interface ModelSelectorProps {
  disabled: boolean;
}

const ModelSelector: React.FC<ModelSelectorProps> = ({ disabled }) => {
  const {
    downloadedModels,
    selectedModel,
    setSelectedModel,
    isDefaultModelPending,
    setUserHasSelectedModel,
  } = useModels();

  const visibleDownloadedModels = downloadedModels.filter((model) => {
    if (!isExperienceModel(model.info)) {
      return true;
    }
    return model.info.suggested === true;
  });

  const dropdownModels = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : visibleDownloadedModels;

  return (
    <select
      className="model-selector"
      value={selectedModel}
      onChange={(e) => {
        setUserHasSelectedModel(true);
        setSelectedModel(e.target.value);
      }}
      disabled={disabled}
    >
      {dropdownModels.map((model) => (
        <option key={model.id} value={model.id}>
          {model.id}
        </option>
      ))}
    </select>
  );
};

export default ModelSelector;
