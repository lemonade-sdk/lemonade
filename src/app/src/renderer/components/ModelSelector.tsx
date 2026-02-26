import React from 'react';
import { useModels, DEFAULT_MODEL_ID } from '../hooks/useModels';

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

  const dropdownModels = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : downloadedModels;

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
