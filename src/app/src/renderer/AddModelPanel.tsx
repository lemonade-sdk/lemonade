import React, { useState, useEffect } from 'react';
import { useSystem } from './hooks/useSystem';

export interface AddModelInitialValues {
  name: string;
  checkpoint: string;
  recipe: string;
  mmprojOptions?: string[];
  vision?: boolean;
  reranking?: boolean;
  embedding?: boolean;
}

export interface ModelInstallData {
  name: string;
  checkpoint: string;
  recipe: string;
  mmproj?: string;
  reasoning?: boolean;
  vision?: boolean;
  embedding?: boolean;
  reranking?: boolean;
}

interface AddModelPanelProps {
  onClose: () => void;
  onInstall: (data: ModelInstallData) => void;
  initialValues?: AddModelInitialValues;
}

const RECIPE_LABELS: Record<string, string> = {
  'llamacpp': 'Llama.cpp GPU',
  'flm': 'FastFlowLM NPU',
  'ryzenai-llm': 'Ryzen AI LLM',
};

const createEmptyForm = (initial?: AddModelInitialValues) => ({
  name: initial?.name ?? '',
  checkpoint: initial?.checkpoint ?? '',
  recipe: initial?.recipe ?? 'llamacpp',
  mmproj: '',
  reasoning: false,
  vision: initial?.vision ?? false,
  embedding: initial?.embedding ?? false,
  reranking: initial?.reranking ?? false,
});

const AddModelPanel: React.FC<AddModelPanelProps> = ({ onClose, onInstall, initialValues }) => {
  const { supportedRecipes } = useSystem();
  const [form, setForm] = useState(() => createEmptyForm(initialValues));
  const [error, setError] = useState<string | null>(null);

  const mmprojOptions = initialValues?.mmprojOptions ?? [];

  const getMmprojLabel = (filename: string): string =>
    filename.replace(/^mmproj-/i, '').replace(/^model-/i, '').replace(/\.gguf$/i, '');

  useEffect(() => {
    const newForm = createEmptyForm(initialValues);
    if (initialValues?.mmprojOptions && initialValues.mmprojOptions.length > 0) {
      newForm.mmproj = initialValues.mmprojOptions[0];
    }
    setForm(newForm);
    setError(null);
  }, [initialValues]);

  const handleChange = (field: string, value: string | boolean) => {
    setForm(prev => ({ ...prev, [field]: value }));
    setError(null);
  };

  const handleInstall = () => {
    const name = form.name.trim();
    const checkpoint = form.checkpoint.trim();
    const recipe = form.recipe.trim();

    if (!name) {
      setError('Model name is required.');
      return;
    }
    if (!checkpoint) {
      setError('Checkpoint is required.');
      return;
    }
    if (!recipe) {
      setError('Recipe is required.');
      return;
    }
    if (checkpoint.toLowerCase().includes('gguf') && !checkpoint.includes(':')) {
      setError('GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax.');
      return;
    }

    onInstall({
      name,
      checkpoint,
      recipe,
      mmproj: form.mmproj.trim() || undefined,
      reasoning: form.reasoning,
      vision: form.vision,
      embedding: form.embedding,
      reranking: form.reranking,
    });
  };

  const filteredSupportedRecipes = Object.keys(supportedRecipes).filter(r => r in RECIPE_LABELS);
  const recipeOptions = filteredSupportedRecipes.length > 0
    ? filteredSupportedRecipes
    : Object.keys(RECIPE_LABELS);

  const mmprojOptionElements = mmprojOptions.map((f: string) => {
    const label = getMmprojLabel(f);
    return React.createElement('option', { key: f, value: f }, label);
  });

  const showMmproj = mmprojOptions.length > 0 || !initialValues;
  const mmprojField: React.ReactNode = showMmproj
    ? React.createElement(
        'div',
        { className: 'form-subsection' },
        React.createElement(
          'label',
          { className: 'form-label-secondary', title: 'Multimodal projection file for vision models' },
          'mmproj file (Optional)'
        ),
        mmprojOptions.length > 0
          ? React.createElement(
              'select',
              {
                className: 'form-input form-select',
                value: form.mmproj,
                onChange: (e: React.ChangeEvent<HTMLSelectElement>) => handleChange('mmproj', e.target.value),
              },
              ...mmprojOptionElements
            )
          : React.createElement('input', {
              type: 'text',
              className: 'form-input',
              placeholder: 'mmproj-F16.gguf',
              value: form.mmproj,
              onChange: (e: React.ChangeEvent<HTMLInputElement>) => handleChange('mmproj', e.target.value),
            })
      )
    : null;

  return (
    <>
      <div className="settings-header">
        <h3>Add a Model</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content">
        <div className="form-section">
          <label className="form-label" title="A unique name to identify your model in the catalog">
            Model Name
          </label>
          <div className="input-with-prefix">
            <span className="input-prefix">user.</span>
            <input
              type="text"
              className="form-input with-prefix"
              placeholder="Gemma-3-12b-it-GGUF"
              value={form.name}
              onChange={(e) => handleChange('name', e.target.value)}
            />
          </div>
        </div>

        <div className="form-section">
          <label className="form-label" title="Hugging Face model path (repo/model:quantization)">
            Checkpoint
          </label>
          <input
            type="text"
            className="form-input"
            placeholder="unsloth/gemma-3-12b-it-GGUF:Q4_0"
            value={form.checkpoint}
            onChange={(e) => handleChange('checkpoint', e.target.value)}
          />
        </div>

        <div className="form-section">
          <label className="form-label" title="Inference backend to use for this model">Recipe</label>
          <select
            className="form-input form-select"
            value={form.recipe}
            onChange={(e) => handleChange('recipe', e.target.value)}
          >
            <option value="">Select a recipe...</option>
            {recipeOptions.map(recipe => (
              <option key={recipe} value={recipe}>
                {RECIPE_LABELS[recipe] ?? recipe}
              </option>
            ))}
          </select>
        </div>

        <div className="form-section">
          <label className="form-label">More info</label>
          {mmprojField}
        </div>

        <div className="settings-section-container">
          <div className="settings-section">
            <label className="settings-checkbox-label">
              <input
                type="checkbox"
                className="settings-checkbox"
                checked={form.embedding}
                onChange={(e) => handleChange('embedding', e.target.checked)}
              />
              <div className="settings-checkbox-content">
                <span className="settings-label-text">Embedding</span>
                <span className="settings-description">Select this box if your model outputs numerical vectors that capture semantic meaning. This enables the <code>--embeddings</code> flag in llama.cpp</span>
              </div>
            </label>
          </div>

          <div className="settings-section">
            <label className="settings-checkbox-label">
              <input
                type="checkbox"
                className="settings-checkbox"
                checked={form.reranking}
                onChange={(e) => handleChange('reranking', e.target.checked)}
              />
              <div className="settings-checkbox-content">
                <span className="settings-label-text">Reranking</span>
                <span className="settings-description">Select this box if your model reorders a list of inputs based on relevance to a query. This enables the <code>--reranking</code> flag in llama.cpp</span>
              </div>
            </label>
          </div>

          <div className="settings-section">
            <label className="settings-checkbox-label">
              <input
                type="checkbox"
                className="settings-checkbox"
                checked={form.vision}
                onChange={(e) => handleChange('vision', e.target.checked)}
              />
              <div className="settings-checkbox-content">
                <span className="settings-label-text">Vision</span>
                <span className="settings-description">Select this box if your model can respond to combinations of image and text. If selected, llama.cpp will be run with <code>--mmproj &lt;path&gt;</code> for multimodal input.</span>
              </div>
            </label>
          </div>
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer">
        <button className="settings-reset-button" onClick={onClose}>
          Cancel
        </button>
        <button className="settings-save-button" onClick={handleInstall}>
          Install
        </button>
      </div>
    </>
  );
};

export default AddModelPanel;
