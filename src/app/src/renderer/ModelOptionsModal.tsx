import React, { useState, useEffect, useRef } from 'react';
import { RecipeOptions } from './recipes/recipeOptions';
import { serverFetch } from "./utils/serverConfig";
import * as llamacpp from './recipes/llamacpp/recipeOptions'
import * as whispercpp from './recipes/whispercpp/recipeOptions'
import * as fastflow from './recipes/fastflow/recipeOptions'
import * as onnx from './recipes/onnx/recipeOptions'
import * as sdcpp from './recipes/sdcpp/recipeOptions'
import { ModelInfo } from "./utils/modelData";
import { useSystem } from "./hooks/useSystem";
import { OgaRecipies } from "./recipes/onnx/recipeOptions";

interface SettingsModalProps {
  isOpen: boolean;
  onSubmit: (modelName: string, options: RecipeOptions) => void;
  onCancel: () => void;
  model: string | null;
}

const ModelOptionsModal: React.FC<SettingsModalProps> = ({ isOpen, onCancel, onSubmit, model }) => {
  const { supportedEngines } = useSystem();
  const [modelInfo, setModelInfo] = useState<ModelInfo>();
  const [modelName, setModelName] = useState("");
  const [modelUrl, setModelUrl] = useState<string>("");
  const [options, setOptions] = useState<RecipeOptions>();
  const [isLoading, setIsLoading] = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!isOpen) {
      return;
    }
    let isMounted = true;

    const fetchOptions = async () => {
      if (isMounted) {
        setIsLoading(true);
      }

      if (!model) return;

      try {
        const response = await serverFetch(`/models/${model}`);
        const data = await response.json();

        setModelName(model);
        setModelInfo({ ...data });
        const modelUrl = `https://huggingface.co/${data.checkpoint.replace(/:.+$/, '')}`;
        if (modelUrl) setModelUrl(modelUrl);

        if (data.recipe === 'whispercpp') {
          setOptions(whispercpp.createDefaultOptions());
          const stored: RecipeOptions = {
            recipe: 'whispercpp',
            ctxSize: { value: data.recipe_options.ctx_size, useDefault: true },
            saveOptions: { value: data.recipe_options.save_options, useDefault: true },
          }

          if (isMounted) {
            setOptions(whispercpp.mergeWithDefaultOptions(stored));
          }
        } else if (data.recipe === 'flm') {
          setOptions(fastflow.createDefaultOptions());
          const stored: RecipeOptions = {
            recipe: 'flm',
            ctxSize: { value: data.recipe_options.ctx_size, useDefault: true },
            saveOptions: { value: data.recipe_options.save_options, useDefault: true },
          }

          if (isMounted) {
            setOptions(fastflow.mergeWithDefaultOptions(stored));
          }
        } else if (OgaRecipies.includes(data.recipe)) {
          setOptions(onnx.createDefaultOptions());
          const stored: RecipeOptions = {
            recipe: data.recipe,
            ctxSize: { value: data.recipe_options.ctx_size, useDefault: true },
            saveOptions: { value: data.recipe_options.save_options, useDefault: true },
          }

          if (isMounted) {
            setOptions(onnx.mergeWithDefaultOptions(stored));
          }
        } else { // llamacpp
          setOptions(llamacpp.createDefaultOptions());
          const stored: RecipeOptions = {
            recipe: 'llamacpp',
            ctxSize: { value: data.recipe_options.ctx_size, useDefault: true },
            llamacppBackend: { value: data.recipe_options.llamacpp_backend, useDefault: true },
            llamacppArgs: { value: data.recipe_options.llamacpp_args, useDefault: true },
            saveOptions: { value: data.recipe_options.save_options, useDefault: true },
          }

          if (isMounted) {
            setOptions(llamacpp.mergeWithDefaultOptions(stored));
          }
        }
      } catch (error) {
        console.error('Failed to load options:', error);
        if (!options) return;

        if (isMounted) {
          if (options.recipe === 'llamacpp') {
            setOptions(llamacpp.createDefaultOptions());
          } else if (options.recipe === 'whispercpp') {
            setOptions(whispercpp.createDefaultOptions());
          } else if (options.recipe === 'flm') {
            setOptions(fastflow.createDefaultOptions());
          } else if (options.recipe === 'sd-cpp') {
            setOptions(fastflow.createDefaultOptions());
          } else if (OgaRecipies.includes(options.recipe)) {
            setOptions(onnx.createDefaultOptions());
          }
        }
      } finally {
        if (isMounted) {
          setIsLoading(false);
        }
      }
    };

    fetchOptions();

    return () => {
      isMounted = false;
    };
  }, [isOpen]);

  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (event: MouseEvent) => {
      if (cardRef.current && !cardRef.current.contains(event.target as Node)) {
        onCancel();
      }
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onCancel();
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [isOpen, onCancel]);

  const handleOverlayClick = (e: React.MouseEvent) => {
    if (e.target === e.currentTarget) {
      onCancel();
    }
  };

  const handleNumericChange = (key: llamacpp.NumericOptionKey | whispercpp.NumericOptionKey, rawValue: number) => {
    if (!options) {
      return null;
    }

    setOptions((prev) => {
      if (!prev) return prev;
      if (prev.recipe === 'llamacpp') {
        return {
          ...prev,
          [key]: {
            value: llamacpp.clampNumericOptionValue(key, rawValue),
            useDefault: false,
          }
        }
      } else if (prev.recipe === 'whispercpp') {
        return {
          ...prev,
          [key]: {
            value: whispercpp.clampNumericOptionValue(key, rawValue),
            useDefault: false,
          }
        }
      } else if (prev.recipe === 'flm') {
        return {
          ...prev,
          [key]: {
            value: fastflow.clampNumericOptionValue(key, rawValue),
            useDefault: false,
          }
        }
      } else if (prev.recipe === 'sd-cpp') {
        return {
          ...prev,
          [key]: {
            value: sdcpp.clampNumericOptionValue(key, rawValue),
            useDefault: false,
          }
        }
      } else if (OgaRecipies.includes(prev.recipe)) {
        return {
          ...prev,
          [key]: {
            value: onnx.clampNumericOptionValue(key, rawValue),
            useDefault: false,
          }
        }
      }
    });
  };

  const handleStringChange = (key: llamacpp.StringOptionKey, rawValue: string) => {
    setOptions((prev) => {
      if (!prev) return prev;

      if (prev.recipe == 'llamacpp') {
        return {
          ...prev,
          [key]: {
            value: rawValue,
            useDefault: false,
          },
        }
      }
    });
  };

  const handleBooleanChange = (key: llamacpp.BooleanOptionKey | whispercpp.BooleanOptionKey, value: boolean) => {
    setOptions((prev) => {
      if (!prev) return prev;
      return {
        ...prev,
        [key]: {
          value,
          useDefault: false,
        },
      }
    });
  };

  const handleResetField = (key: llamacpp.NumericOptionKey | llamacpp.StringOptionKey | llamacpp.BooleanOptionKey | whispercpp.NumericOptionKey | whispercpp.BooleanOptionKey) => {
    setOptions((prev) => {
      if (!prev) return prev;
      if (prev.recipe === 'llamacpp') {
        return {
          ...prev,
          [key]: {
            value: llamacpp.DEFAULT_OPTION_VALUES[key],
            useDefault: true,
          },
        };
      }
    });
  };

  const handleReset = () => {
    if (!options) return;

    if (options.recipe == 'llamacpp') {
      setOptions(llamacpp.createDefaultOptions());
    } else if (options.recipe == 'whispercpp') {
      setOptions(whispercpp.createDefaultOptions());
    } else if (options.recipe === 'flm') {
      setOptions(fastflow.createDefaultOptions());
    } else if (options.recipe === 'sd-cpp') {
      setOptions(sdcpp.createDefaultOptions());
    } else if (OgaRecipies.includes(options.recipe)) {
      setOptions(onnx.createDefaultOptions());
    }
  };

  const handleCancel = async () => {
    onCancel();
    return;
  };

  const handleSubmit = async () => {
    if (!options || !modelName) return;
    onSubmit(modelName, options);
    return;
  };

  if (!isOpen) return null;
  if (!options) return null;

  return (
    <div className="settings-overlay">
      <div className="settings-modal" ref={cardRef} onMouseDown={(e) => e.stopPropagation()}>
        <div className="settings-header">
          <h3>Model Options</h3>
          <button className="settings-close-button" onClick={onCancel} title="Close">
            <svg width="14" height="14" viewBox="0 0 14 14">
              <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
            </svg>
          </button>
        </div>

        {isLoading ? (
          <div className="settings-loading">Loading options…</div>
        ) : options.recipe === 'llamacpp' ? (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: <a href={modelUrl} target="_blank">{modelInfo?.checkpoint}</a></h5>
            </div>

            <div className="form-section">
              <label className="form-label" title="context size">context size</label>
              <input
                type="text"
                value={options.ctxSize.value}
                onChange={(e) => {
                  if (e.target.value === 'auto' || e.target.value === '') {
                    return;
                  }
                  const parsed = parseFloat(e.target.value);
                  if (Number.isNaN(parsed)) {
                    return;
                  }
                  handleNumericChange('ctxSize', parsed);
                }}
                className="form-input"
                placeholder="auto"
              />
            </div>

            {/*<div className="form-section">
              <label className="form-label" title="Select llamacpp backend to use for this model">llamacpp
                backend</label>
              <select
                className="form-input form-select"
                value={options.llamacppBackend.value}
                onChange={(e) => handleStringChange('llamacppBackend', e.target.value)}
              >
                <option value="">Select a llamacpp backend...</option>
                {supportedEngines.filter(engine => engine !== 'OGA').map((engine) => {
                  return (<option key={engine.toLowerCase()} value={engine.toLowerCase()}>{engine}</option>)
                })}
              </select>
            </div>*/}

            <div className="form-section">
              <label className="form-label" title="llamacpp arguments">llamacpp arguments</label>
              <input
                type="text"
                className="form-input"
                placeholder=""
                value={options.llamacppArgs.value}
                onChange={(e) => handleStringChange('llamacppArgs', e.target.value)}
              />
            </div>

            <div
              className={`settings-section ${
                options.saveOptions.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Save Options</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('saveOptions')}
                  disabled={options.saveOptions.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={options.saveOptions.value}
                  onChange={(e) => handleBooleanChange('saveOptions', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines if the options will be saved in lemonade server.
                  </span>
                </div>
              </label>
            </div>
          </div>
        ) : options.recipe === 'whispercpp' ? (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: {modelInfo?.checkpoint}</h5>
            </div>

            <div className="form-section">
              <label className="form-label" title="context size">context size</label>
              <input
                type="text"
                value={options.ctxSize.value}
                onChange={(e) => {
                  if (e.target.value === 'auto' || e.target.value === '') {
                    return;
                  }
                  const parsed = parseFloat(e.target.value);
                  if (Number.isNaN(parsed)) {
                    return;
                  }
                  handleNumericChange('ctxSize', parsed);
                }}
                className="form-input"
                placeholder="auto"
              />
            </div>
            <div
              className={`settings-section ${
                options.saveOptions.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Save Options</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('saveOptions')}
                  disabled={options.saveOptions.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={options.saveOptions.value}
                  onChange={(e) => handleBooleanChange('saveOptions', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines if the options will be saved in lemonade server.
                  </span>
                </div>
              </label>
            </div>
          </div>) : options.recipe === 'flm' ? (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: {modelInfo?.checkpoint}</h5>
            </div>

            <div className="form-section">
              <label className="form-label" title="context size">context size</label>
              <input
                type="text"
                value={options.ctxSize.value}
                onChange={(e) => {
                  if (e.target.value === 'auto' || e.target.value === '') {
                    return;
                  }
                  const parsed = parseFloat(e.target.value);
                  if (Number.isNaN(parsed)) {
                    return;
                  }
                  handleNumericChange('ctxSize', parsed);
                }}
                className="form-input"
                placeholder="auto"
              />
            </div>
            <div
              className={`settings-section ${
                options.saveOptions.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Save Options</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('saveOptions')}
                  disabled={options.saveOptions.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={options.saveOptions.value}
                  onChange={(e) => handleBooleanChange('saveOptions', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines if the options will be saved in lemonade server.
                  </span>
                </div>
              </label>
            </div>
          </div>) : options.recipe === 'sd-cpp' ? (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: {modelInfo?.checkpoint}</h5>
            </div>

            <div className="form-section">
              <label className="form-label" title="context size">context size</label>
              <input
                type="text"
                value={options.ctxSize.value}
                onChange={(e) => {
                  if (e.target.value === 'auto' || e.target.value === '') {
                    return;
                  }
                  const parsed = parseFloat(e.target.value);
                  if (Number.isNaN(parsed)) {
                    return;
                  }
                  handleNumericChange('ctxSize', parsed);
                }}
                className="form-input"
                placeholder="auto"
              />
            </div>
            <div
              className={`settings-section ${
                options.saveOptions.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Save Options</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('saveOptions')}
                  disabled={options.saveOptions.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={options.saveOptions.value}
                  onChange={(e) => handleBooleanChange('saveOptions', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines if the options will be saved in lemonade server.
                  </span>
                </div>
              </label>
            </div>
          </div>) : OgaRecipies.includes(options.recipe) ? (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: {modelInfo?.checkpoint}</h5>
            </div>

            <div className="form-section">
              <label className="form-label" title="context size">context size</label>
              <input
                type="text"
                value={options.ctxSize.value}
                onChange={(e) => {
                  if (e.target.value === 'auto' || e.target.value === '') {
                    return;
                  }
                  const parsed = parseFloat(e.target.value);
                  if (Number.isNaN(parsed)) {
                    return;
                  }
                  handleNumericChange('ctxSize', parsed);
                }}
                className="form-input"
                placeholder="auto"
              />
            </div>
            <div
              className={`settings-section ${
                options.saveOptions.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Save Options</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('saveOptions')}
                  disabled={options.saveOptions.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={options.saveOptions.value}
                  onChange={(e) => handleBooleanChange('saveOptions', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines if the options will be saved in lemonade server.
                  </span>
                </div>
              </label>
            </div>
          </div>
        ) : <div></div>}

        <div className="settings-footer">
          <button
            className="settings-reset-button"
            onClick={handleReset}
            disabled={isSubmitting || isLoading}
          >
            Reset All
          </button>

          <button
            className="settings-save-button"
            onClick={handleCancel}
            disabled={isSubmitting || isLoading}
          >
            {isSubmitting ? 'Cancelling…' : 'Cancel'}
          </button>
          <button
            className="settings-save-button"
            onClick={handleSubmit}
            disabled={isSubmitting || isLoading}
          >
            {isSubmitting ? 'Connecting…' : 'Load'}
          </button>
        </div>
      </div>
    </div>
  );
};

export default ModelOptionsModal;
