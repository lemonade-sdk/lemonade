import React, { useState } from 'react';

export interface AddModelInitialValues {
  name?: string;
  checkpoint?: string;
  recipe?: string;
  mmproj?: string;
  reasoning?: boolean;
  vision?: boolean;
  embedding?: boolean;
  reranking?: boolean;
}

interface AddModelPanelProps {
  onClose: () => void;
  onInstall: (modelData: {
    name: string;
    checkpoint: string;
    recipe: string;
    mmproj?: string;
    reasoning?: boolean;
    vision?: boolean;
    embedding?: boolean;
    reranking?: boolean;
  }) => void;
  initialValues?: AddModelInitialValues;
}

const AddModelPanel: React.FC<AddModelPanelProps> = ({ onClose, onInstall, initialValues }) => {
  const [customForm, setCustomForm] = useState({
    name: initialValues?.name || '',
    checkpoint: initialValues?.checkpoint || '',
    recipe: initialValues?.recipe || 'llamacpp',
    mmproj: initialValues?.mmproj || '',
    reasoning: initialValues?.reasoning || false,
    vision: initialValues?.vision || false,
    embedding: initialValues?.embedding || false,
    reranking: initialValues?.reranking || false,
  });

  const handleCustomInstall = () => {
    const trimmedName = customForm.name.trim();
    const trimmedCheckpoint = customForm.checkpoint.trim();
    const trimmedRecipe = customForm.recipe.trim();
    const trimmedMmproj = customForm.mmproj.trim();

    if (!trimmedName || !trimmedCheckpoint || !trimmedRecipe) {
      return;
    }

    // Validate GGUF checkpoint format
    if (trimmedCheckpoint.toLowerCase().includes('gguf') && !trimmedCheckpoint.includes(':')) {
      alert('GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax');
      return;
    }

    onInstall({
      name: trimmedName,
      checkpoint: trimmedCheckpoint,
      recipe: trimmedRecipe,
      mmproj: trimmedMmproj || undefined,
      reasoning: customForm.reasoning,
      vision: customForm.vision,
      embedding: customForm.embedding,
      reranking: customForm.reranking,
    });
  };

  const handleCustomInputChange = (field: string, value: string | boolean) => {
    setCustomForm(prev => ({
      ...prev,
      [field]: value,
    }));
  };

  return (
    <div className="add-model-panel">
      <div className="add-model-panel-header">
        <span className="add-model-panel-title">Add Custom Model</span>
        <button className="add-model-panel-close" onClick={onClose} title="Close">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
            <line x1="18" y1="6" x2="6" y2="18" />
            <line x1="6" y1="6" x2="18" y2="18" />
          </svg>
        </button>
      </div>

      <div className="add-model-panel-content">
        <div className="amp-custom">
          <div className="amp-field">
            <label className="amp-label">Name</label>
            <div className="amp-input-prefix">
              <span className="amp-prefix">user.</span>
              <input
                type="text"
                className="amp-input"
                placeholder="my-model"
                value={customForm.name}
                onChange={(e) => handleCustomInputChange('name', e.target.value)}
                autoFocus
              />
            </div>
          </div>

          <div className="amp-field">
            <label className="amp-label">Checkpoint</label>
            <input
              type="text"
              className="amp-input amp-input-full"
              placeholder="org/model:Q4_0"
              value={customForm.checkpoint}
              onChange={(e) => handleCustomInputChange('checkpoint', e.target.value)}
            />
          </div>

          <div className="amp-field">
            <label className="amp-label">Recipe</label>
            <select
              className="amp-select"
              value={customForm.recipe}
              onChange={(e) => handleCustomInputChange('recipe', e.target.value)}
            >
              <option value="llamacpp">Llama.cpp GPU</option>
              <option value="flm">FastFlowLM NPU</option>
              <option value="oga-cpu">ONNX Runtime CPU</option>
              <option value="oga-hybrid">ONNX Runtime Hybrid</option>
              <option value="oga-npu">ONNX Runtime NPU</option>
            </select>
          </div>

          <div className="amp-field">
            <label className="amp-label">mmproj (optional)</label>
            <input
              type="text"
              className="amp-input amp-input-full"
              placeholder="mmproj-F16.gguf"
              value={customForm.mmproj}
              onChange={(e) => handleCustomInputChange('mmproj', e.target.value)}
            />
          </div>

          <div className="amp-checkboxes">
            <label className="amp-checkbox">
              <input
                type="checkbox"
                checked={customForm.reasoning}
                onChange={(e) => handleCustomInputChange('reasoning', e.target.checked)}
              />
              <span>Reasoning</span>
            </label>
            <label className="amp-checkbox">
              <input
                type="checkbox"
                checked={customForm.vision}
                onChange={(e) => handleCustomInputChange('vision', e.target.checked)}
              />
              <span>Vision</span>
            </label>
            <label className="amp-checkbox">
              <input
                type="checkbox"
                checked={customForm.embedding}
                onChange={(e) => handleCustomInputChange('embedding', e.target.checked)}
              />
              <span>Embedding</span>
            </label>
            <label className="amp-checkbox">
              <input
                type="checkbox"
                checked={customForm.reranking}
                onChange={(e) => handleCustomInputChange('reranking', e.target.checked)}
              />
              <span>Reranking</span>
            </label>
          </div>

          <button
            className="amp-install-btn"
            onClick={handleCustomInstall}
            disabled={!customForm.name.trim() || !customForm.checkpoint.trim()}
          >
            Install
          </button>
        </div>
      </div>
    </div>
  );
};

export default AddModelPanel;
