import React from 'react';
import { useModels, DEFAULT_MODEL_ID, DownloadedModel } from '../hooks/useModels';

interface ModelSelectorProps {
  disabled: boolean;
  filter?: (model: DownloadedModel) => boolean;
}

const ModelSelector: React.FC<ModelSelectorProps> = ({ disabled, filter }) => {
  const {
    downloadedModels,
    selectedModel,
    setSelectedModel,
    isDefaultModelPending,
    setUserHasSelectedModel,
  } = useModels();

  const filteredModels = filter ? downloadedModels.filter(filter) : downloadedModels;

  const dropdownModels = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : filteredModels;

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
