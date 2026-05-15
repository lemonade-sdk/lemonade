import React, { useState, useEffect } from 'react';
import { useSystem } from './hooks/useSystem';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';

export interface AddModelInitialValues {
  name: string;
  checkpoint: string;
  recipe: string;
  checkpoints?: Record<string, string>;
  mmprojOptions?: string[];
  vision?: boolean;
  reranking?: boolean;
  embedding?: boolean;
}

export interface ModelInstallData {
  name: string;
  checkpoint: string;
  recipe: string;
  checkpoints?: Record<string, string>;
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

const FALLBACK_RECIPE_OPTIONS = ['llamacpp', 'flm', 'ryzenai-llm'];
const HIDDEN_RECIPE_OPTIONS = new Set(['collection']);

const getRecipeLabel = (recipe: string): string => RECIPE_DISPLAY_NAMES[recipe] ?? recipe;

const createEmptyForm = (initial?: AddModelInitialValues) => ({
  name: initial?.name ?? '',
  checkpoint: initial?.checkpoint ?? initial?.checkpoints?.main ?? '',
  recipe: initial?.recipe ?? 'llamacpp',
  textEncoderCheckpoint: initial?.checkpoints?.text_encoder ?? '',
  vaeCheckpoint: initial?.checkpoints?.vae ?? '',
  mmproj: '',
  reasoning: false,
  vision: initial?.vision ?? false,
  embedding: initial?.embedding ?? false,
  reranking: initial?.reranking ?? false,
});

const AddModelPanel: React.FC<AddModelPanelProps> = ({ onClose, onInstall, initialValues }) => {
  const { supportedRecipes, ensureSystemInfoLoaded } = useSystem();
  const [form, setForm] = useState(() => createEmptyForm(initialValues));
  const [error, setError] = useState<string | null>(null);

  const mmprojOptions = initialValues?.mmprojOptions ?? [];

  const getMmprojLabel = (filename: string): string =>
    filename.replace(/^mmproj-/i, '').replace(/^model-/i, '').replace(/\.gguf$/i, '');

  useEffect(() => {
    void ensureSystemInfoLoaded();
  }, [ensureSystemInfoLoaded]);

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
    const textEncoderCheckpoint = form.textEncoderCheckpoint.trim();
    const vaeCheckpoint = form.vaeCheckpoint.trim();
    const hasSdComponents = Boolean(textEncoderCheckpoint || vaeCheckpoint);

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
    if (hasSdComponents && recipe !== 'sd-cpp') {
      setError('Text encoder and VAE checkpoints are only supported for StableDiffusion.cpp models.');
      return;
    }
    if (hasSdComponents && (!textEncoderCheckpoint || !vaeCheckpoint)) {
      setError('Provide both text encoder and VAE checkpoints for sd-cpp component models.');
      return;
    }
    if ((textEncoderCheckpoint && !textEncoderCheckpoint.includes(':')) || (vaeCheckpoint && !vaeCheckpoint.includes(':'))) {
      setError('Additional sd-cpp checkpoints must include exact variants using the CHECKPOINT:VARIANT syntax.');
      return;
    }

    onInstall({
      name,
      checkpoint,
      checkpoints: recipe === 'sd-cpp' && hasSdComponents
        ? { main: checkpoint, text_encoder: textEncoderCheckpoint, vae: vaeCheckpoint }
        : undefined,
      recipe,
      mmproj: form.mmproj.trim() || undefined,
      reasoning: form.reasoning,
      vision: form.vision,
      embedding: form.embedding,
      reranking: form.reranking,
    });
  };

  const supportedRecipeOptions = Object.keys(supportedRecipes)
    .filter(recipe => !HIDDEN_RECIPE_OPTIONS.has(recipe))
    .sort((a, b) => getRecipeLabel(a).localeCompare(getRecipeLabel(b)));
  const recipeOptions = supportedRecipeOptions.length > 0
    ? supportedRecipeOptions
    : FALLBACK_RECIPE_OPTIONS;

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
          <input
            type="text"
            className="form-input"
            placeholder="Gemma-3-12b-it-GGUF"
            value={form.name}
            onChange={(e) => handleChange('name', e.target.value)}
          />
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
                {getRecipeLabel(recipe)}
              </option>
            ))}
          </select>
        </div>

        <div className="form-section">
          <label className="form-label">More info</label>
          {form.recipe === 'sd-cpp' && (
            <div className="form-subsection">
              <label
                className="form-label-secondary"
                title="Optional component checkpoints for sd.cpp models with separate text encoder or VAE files"
              >
                sd.cpp component checkpoints (Optional)
              </label>
              <input
                type="text"
                className="form-input"
                placeholder="Text encoder CHECKPOINT:VARIANT"
                value={form.textEncoderCheckpoint}
                onChange={(e) => handleChange('textEncoderCheckpoint', e.target.value)}
              />
              <input
                type="text"
                className="form-input"
                placeholder="VAE CHECKPOINT:VARIANT"
                value={form.vaeCheckpoint}
                onChange={(e) => handleChange('vaeCheckpoint', e.target.value)}
              />
            </div>
          )}
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
