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

type RecipeExample = {
  name: string;
  checkpoint: string;
  textEncoderCheckpoint?: string;
  vaeCheckpoint?: string;
};

const RECIPE_EXAMPLES: Record<string, RecipeExample> = {
  'llamacpp': {
    name: 'Gemma-3-4b-it-GGUF',
    checkpoint: 'ggml-org/gemma-3-4b-it-GGUF:Q4_K_M',
  },
  'ryzenai-llm': {
    name: 'Qwen2.5-0.5B-Instruct-CPU',
    checkpoint: 'amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx',
  },
  'flm': {
    name: 'Gemma-3-4B-FLM',
    checkpoint: 'gemma3:4b',
  },
  'whispercpp': {
    name: 'Whisper-Tiny',
    checkpoint: 'ggerganov/whisper.cpp:ggml-tiny.bin',
  },
  'sd-cpp': {
    name: 'Z-Image-Turbo',
    checkpoint: 'Comfy-Org/z_image_turbo:split_files/diffusion_models/z_image_turbo_bf16.safetensors',
    textEncoderCheckpoint: 'Comfy-Org/z_image_turbo:split_files/text_encoders/qwen_3_4b.safetensors',
    vaeCheckpoint: 'Comfy-Org/z_image_turbo:split_files/vae/ae.safetensors',
  },
  'kokoro': {
    name: 'kokoro-v1',
    checkpoint: 'mikkoph/kokoro-onnx',
  },
  'vllm': {
    name: 'Qwen3.5-0.8B-vLLM',
    checkpoint: 'Qwen/Qwen3.5-0.8B',
  },
};

const getRecipeExample = (recipe: string): RecipeExample => RECIPE_EXAMPLES[recipe] ?? RECIPE_EXAMPLES.llamacpp;

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

const hasRepoRelativeFilePath = (checkpoint: string): boolean => {
  const separatorIndex = checkpoint.indexOf(':');
  if (separatorIndex === -1) return false;
  const variant = checkpoint.slice(separatorIndex + 1).trim();
  return variant.includes('.');
};

const isGgufCheckpoint = (checkpoint: string): boolean => checkpoint.toLowerCase().includes('gguf');

const AddModelPanel: React.FC<AddModelPanelProps> = ({ onClose, onInstall, initialValues }) => {
  const { supportedRecipes, ensureSystemInfoLoaded } = useSystem();
  const [form, setForm] = useState(() => createEmptyForm(initialValues));
  const [error, setError] = useState<string | null>(null);

  const mmprojOptions = initialValues?.mmprojOptions ?? [];
  const isSdCpp = form.recipe === 'sd-cpp';
  const recipeExample = getRecipeExample(form.recipe);

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
    if (recipe === 'sd-cpp' && !hasRepoRelativeFilePath(checkpoint)) {
      setError('StableDiffusion.cpp checkpoints must include the full file path relative to the Hugging Face repo, for example repo/model:path/to/model.safetensors or repo/model:path/to/model.gguf.');
      return;
    }
    if (recipe !== 'sd-cpp' && isGgufCheckpoint(checkpoint) && !checkpoint.includes(':')) {
      setError('GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax.');
      return;
    }
    if (recipe === 'vllm' && isGgufCheckpoint(checkpoint)) {
      setError('vLLM checkpoints should use a Hugging Face model repo, not a GGUF file or GGUF repo.');
      return;
    }
    if (recipe === 'sd-cpp' && hasSdComponents && (!textEncoderCheckpoint || !vaeCheckpoint)) {
      setError('Provide both text encoder and VAE checkpoints for sd-cpp component models.');
      return;
    }
    if (recipe === 'sd-cpp' && ((textEncoderCheckpoint && !hasRepoRelativeFilePath(textEncoderCheckpoint)) || (vaeCheckpoint && !hasRepoRelativeFilePath(vaeCheckpoint)))) {
      setError('Additional sd-cpp checkpoints must use the full repo-relative file path, for example repo/model:path/to/file.safetensors or repo/model:path/to/file.gguf.');
      return;
    }

    onInstall({
      name,
      checkpoint,
      checkpoints: recipe === 'sd-cpp' && hasSdComponents
        ? { main: checkpoint, text_encoder: textEncoderCheckpoint, vae: vaeCheckpoint }
        : undefined,
      recipe,
      mmproj: !isSdCpp ? (form.mmproj.trim() || undefined) : undefined,
      reasoning: form.reasoning,
      vision: !isSdCpp ? form.vision : false,
      embedding: !isSdCpp ? form.embedding : false,
      reranking: !isSdCpp ? form.reranking : false,
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

  const showMmproj = !isSdCpp && (mmprojOptions.length > 0 || !initialValues);
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
            placeholder={recipeExample.name}
            value={form.name}
            onChange={(e) => handleChange('name', e.target.value)}
          />
        </div>

        <div className="form-section">
          <label
            className="form-label"
            title={isSdCpp
              ? 'Hugging Face repo and exact model file path relative to the repo'
              : form.recipe === 'vllm'
                ? 'Hugging Face model repository for vLLM; do not use GGUF checkpoints'
                : 'Hugging Face model path, for example repo/model or repo/model:variant'}
          >
            {isSdCpp ? 'Main checkpoint' : 'Checkpoint'}
          </label>
          <input
            type="text"
            className="form-input"
            placeholder={recipeExample.checkpoint}
            value={form.checkpoint}
            onChange={(e) => handleChange('checkpoint', e.target.value)}
          />
          {isSdCpp && (
            <span className="settings-description">
              Use the full repo-relative file path. StableDiffusion.cpp checkpoints can be .safetensors or .gguf when supported by the model/backend.
            </span>
          )}
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
          {isSdCpp && (
            <div className="form-subsection">
              <label
                className="form-label-secondary"
                title="Optional text encoder checkpoint for sd.cpp models with separate component files"
              >
                Text encoder checkpoint (Optional)
              </label>
              <input
                type="text"
                className="form-input"
                placeholder={recipeExample.textEncoderCheckpoint}
                value={form.textEncoderCheckpoint}
                onChange={(e) => handleChange('textEncoderCheckpoint', e.target.value)}
              />
              <label
                className="form-label-secondary"
                title="Optional VAE checkpoint for sd.cpp models with separate component files"
              >
                VAE checkpoint (Optional)
              </label>
              <input
                type="text"
                className="form-input"
                placeholder={recipeExample.vaeCheckpoint}
                value={form.vaeCheckpoint}
                onChange={(e) => handleChange('vaeCheckpoint', e.target.value)}
              />
            </div>
          )}
          {mmprojField}
        </div>

        {!isSdCpp && (
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
        )}

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
