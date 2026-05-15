/**
 * Model Options Modal
 *
 * This component uses the central recipe options configuration to dynamically
 * render the appropriate options for each recipe type.
 */

import React, { useState, useEffect, useRef } from 'react';
import { serverFetch } from "./utils/serverConfig";
import { ModelInfo } from "./utils/modelData";
import { useSystem } from "./hooks/useSystem";
import { writeClipboard } from "./utils/clipboardUtils";
import {
  RecipeOptions,
  getOptionsForRecipe,
  getOptionDefinition,
  clampOptionValue,
  createDefaultOptions,
  apiToRecipeOptions,
} from './recipes/recipeOptions';

// Display names for backend options
const BACKEND_DISPLAY_NAMES: Record<string, string> = {
  cpu: "CPU",
  npu: "NPU",
  rocm: "ROCm",
  vulkan: "Vulkan",
  metal: "Metal",
};

const getBackendDisplayName = (backend: string): string => {
  return BACKEND_DISPLAY_NAMES[backend] ?? backend;
};

const CONTEXT_SLIDER_MIN = 2048;
const CONTEXT_SLIDER_THUMB_SIZE = 14;

const formatContextSize = (value: number): string => {
  if (value >= 1024 && value % 1024 === 0) {
    return `${value / 1024}k`;
  }
  if (value >= 1024) {
    return `${Math.round(value / 1024)}k`;
  }
  return String(value);
};

const getContextSliderMarks = (maxContextWindow?: number): number[] => {
  if (!maxContextWindow || maxContextWindow < CONTEXT_SLIDER_MIN) {
    return [];
  }

  const marks: number[] = [];
  for (let value = CONTEXT_SLIDER_MIN; value < maxContextWindow; value *= 2) {
    marks.push(value);
  }

  if (marks[marks.length - 1] !== maxContextWindow) {
    marks.push(maxContextWindow);
  }

  return marks;
};

const contextSizeToSliderValue = (contextSize: number, maxContextWindow: number): number => {
  const clamped = contextSize === 0
    ? maxContextWindow
    : Math.min(Math.max(contextSize, CONTEXT_SLIDER_MIN), maxContextWindow);
  return Math.log2(clamped);
};

const sliderValueToContextSize = (sliderValue: number, maxContextWindow: number): number => {
  const maxSliderValue = Math.log2(maxContextWindow);
  if (sliderValue >= maxSliderValue - 0.0005) {
    return maxContextWindow;
  }
  return Math.min(Math.round(2 ** sliderValue), maxContextWindow);
};

interface SettingsModalProps {
  isOpen: boolean;
  onSubmit: (modelName: string, options: RecipeOptions) => void;
  onCancel: () => void;
  model: string | null;
}

const ModelOptionsModal: React.FC<SettingsModalProps> = ({ isOpen, onCancel, onSubmit, model }) => {
  const { supportedRecipes, ensureSystemInfoLoaded } = useSystem();
  const [modelInfo, setModelInfo] = useState<ModelInfo>();
  const [modelName, setModelName] = useState("");
  const [modelUrl, setModelUrl] = useState<string>("");
  const [options, setOptions] = useState<RecipeOptions>();
  const [numericDrafts, setNumericDrafts] = useState<Record<string, string>>({});
  const [isLoading, setIsLoading] = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [isModelNameCopied, setIsModelNameCopied] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);
  const exportModelBtn = useRef<HTMLAnchorElement | null>(null);
  const modelNameCopyTimeoutIdRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Fetch options when modal opens
  useEffect(() => {
    if (!isOpen) return;
    let isMounted = true;
    setNumericDrafts({});
    setIsModelNameCopied(false);
    if (modelNameCopyTimeoutIdRef.current) {
      clearTimeout(modelNameCopyTimeoutIdRef.current);
      modelNameCopyTimeoutIdRef.current = null;
    }
    void ensureSystemInfoLoaded();

    const fetchOptions = async () => {
      if (isMounted) setIsLoading(true);
      if (!model) {
        if (isMounted) setIsLoading(false);
        return;
      }

      try {
        const response = await serverFetch(`/models/${model}`);
        const data = await response.json();

        setModelName(model);
        setModelInfo({ ...data });

        const url = `https://huggingface.co/${data.checkpoint.replace(/:.+$/, '')}`;
        if (url) setModelUrl(url);

        const recipe = data.recipe as string;
        const recipeOptions = data.recipe_options ?? {};

        if (isMounted) {
          setOptions(apiToRecipeOptions(recipe, recipeOptions));
        }
      } catch (error) {
        console.error('Failed to load options:', error);
        if (isMounted && options?.recipe) {
          setOptions(createDefaultOptions(options.recipe));
        }
      } finally {
        if (isMounted) setIsLoading(false);
      }
    };

    fetchOptions();
    return () => { isMounted = false; };
  }, [isOpen, model, ensureSystemInfoLoaded]);

  useEffect(() => {
    return () => {
      if (modelNameCopyTimeoutIdRef.current) {
        clearTimeout(modelNameCopyTimeoutIdRef.current);
      }
    };
  }, []);

  // Handle click outside and escape key
  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (event: MouseEvent) => {
      if (cardRef.current && !cardRef.current.contains(event.target as Node)) {
        onCancel();
      }
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onCancel();
    };

    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [isOpen, onCancel]);

  // Generic handler for numeric option changes
  const handleNumericChange = (key: string, rawValue: number) => {
    if (!options) return;

    setOptions(prev => {
      if (!prev) return prev;
      return {
        ...prev,
        [key]: {
          value: clampOptionValue(key, rawValue),
          useDefault: false,
        }
      } as RecipeOptions;
    });
  };

  const clearNumericDraft = (key: string) => {
    setNumericDrafts(prev => {
      if (!(key in prev)) return prev;
      const next = { ...prev };
      delete next[key];
      return next;
    });
  };

  const commitNumericDraft = (key: string, draftValue: string): void => {
    const trimmed = draftValue.trim();
    if (trimmed === '') return;

    const parsed = parseFloat(trimmed);
    if (Number.isNaN(parsed)) return;

    handleNumericChange(key, parsed);
  };

  const commitContextSizeDraft = (key: string, draftValue: string, maxContextWindow: number): void => {
    const trimmed = draftValue.trim();
    if (trimmed === '') return;

    const parsed = parseFloat(trimmed);
    if (Number.isNaN(parsed)) return;

    const clamped = parsed === 0 ? 0 : Math.min(Math.max(parsed, 1), maxContextWindow);
    handleNumericChange(key, clamped);
  };

  // Generic handler for string option changes
  const handleStringChange = (key: string, value: string) => {
    if (!options) return;

    setOptions(prev => {
      if (!prev) return prev;
      return {
        ...prev,
        [key]: {
          value,
          useDefault: false,
        }
      } as RecipeOptions;
    });
  };

  // Generic handler for boolean option changes
  const handleBooleanChange = (key: string, value: boolean) => {
    if (!options) return;

    setOptions(prev => {
      if (!prev) return prev;
      return {
        ...prev,
        [key]: {
          value,
          useDefault: false,
        }
      } as RecipeOptions;
    });
  };

  // Reset a single field to its default value
  const handleResetField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def) return;

    clearNumericDraft(key);

    setOptions(prev => {
      if (!prev) return prev;
      return {
        ...prev,
        [key]: {
          value: def.default,
          useDefault: true,
        }
      } as RecipeOptions;
    });
  };

  // Reset all options to defaults
  const handleReset = () => {
    if (!options?.recipe) return;
    setNumericDrafts({});
    setOptions(createDefaultOptions(options.recipe));
  };

  const handleCopyModelName = async () => {
    if (!modelName) return;

    try {
      await writeClipboard(modelName);
      setIsModelNameCopied(true);

      if (modelNameCopyTimeoutIdRef.current) {
        clearTimeout(modelNameCopyTimeoutIdRef.current);
      }

      modelNameCopyTimeoutIdRef.current = setTimeout(() => {
        setIsModelNameCopied(false);
        modelNameCopyTimeoutIdRef.current = null;
      }, 2000);
    } catch (error) {
      console.error('Failed to copy model name:', error);
    }
  };

  const handleModelExport = () => {
    let modelName = (modelInfo?.id as string).startsWith("user.") ? modelInfo?.id : `user.${modelInfo?.id}`;

    let modelToExport = {
      "model_name": modelName,
      "downloaded": modelInfo?.downloaded,
      "labels": modelInfo?.labels,
      "recipe": modelInfo?.recipe,
      "recipe_options": modelInfo?.recipe_options,
      "size": modelInfo?.size,
      "checkpoints": modelInfo?.checkpoints,
      "image_defaults": modelInfo?.image_defaults
    };

    if(!modelInfo?.checkpoints) {
      Object.assign(modelToExport, {checkpoint: modelInfo?.checkpoint});
    }

    const model = JSON.stringify(modelToExport);
    const blob = new Blob([model], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    exportModelBtn!.current!.href = url;
    exportModelBtn!.current!.download = modelToExport?.model_name ? `${modelToExport?.model_name}.json` as string: 'model.json';
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  const handleCancel = () => {
    onCancel();
  };

  const handleSubmit = () => {
    if (!options || !modelName) return;

    let submitOptions: RecipeOptions = options;

    for (const [key, draftValue] of Object.entries(numericDrafts)) {
      const trimmed = draftValue.trim();
      if (trimmed === '') continue;

      const parsed = parseFloat(trimmed);
      if (Number.isNaN(parsed)) continue;

      const maxContextWindow = key === 'ctxSize' ? modelInfo?.max_context_window : undefined;
      const value = maxContextWindow
        ? (parsed === 0 ? 0 : Math.min(Math.max(parsed, 1), maxContextWindow))
        : clampOptionValue(key, parsed);

      submitOptions = {
        ...submitOptions,
        [key]: {
          value,
          useDefault: false,
        }
      } as RecipeOptions;
    }

    onSubmit(modelName, submitOptions);
  };

  if (!isOpen || !options) return null;

  const recipe = options.recipe;
  const availableOptions = getOptionsForRecipe(recipe);

  // Check if recipe has multiple backends available
  const hasMultipleBackends = modelInfo?.recipe && (supportedRecipes[modelInfo.recipe]?.length ?? 0) > 1;

  // Helper to get option value from options object
  const getOptionValue = <T,>(key: string): T | undefined => {
    const opt = (options as unknown as Record<string, { value: T; useDefault: boolean }>)[key];
    return opt?.value;
  };

  const getOptionUseDefault = (key: string): boolean => {
    const opt = (options as unknown as Record<string, { value: unknown; useDefault: boolean }>)[key];
    return opt?.useDefault ?? true;
  };

  const renderContextSizeField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'numeric') return null;

    const value = getOptionValue<number>(key);
    const maxContextWindow = modelInfo?.max_context_window;
    if (value === undefined || !maxContextWindow || maxContextWindow < CONTEXT_SLIDER_MIN) return null;

    const displayValue = numericDrafts[key] ?? String(value);
    const parsedDraft = parseFloat(displayValue.trim());
    const effectiveContextSize = !Number.isNaN(parsedDraft) ? parsedDraft : value;
    const sliderValue = contextSizeToSliderValue(effectiveContextSize, maxContextWindow);
    const marks = getContextSliderMarks(maxContextWindow);
    const sliderMin = Math.log2(CONTEXT_SLIDER_MIN);
    const sliderMax = Math.log2(maxContextWindow);
    const sliderRange = Math.max(sliderMax - sliderMin, 0.0001);
    const sliderProgress = ((sliderValue - sliderMin) / sliderRange) * 100;

    return (
      <div className="form-section context-size-section" key={key}>
        <div className="context-size-label-row">
          <label className="form-label" title={def.description}>{def.label.toLowerCase()}</label>
        </div>
        <div className="context-size-controls">
          <input
            type="range"
            min={sliderMin}
            max={sliderMax}
            step={0.001}
            value={sliderValue}
            list="context-size-marks"
            className="context-size-slider"
            style={{ '--context-slider-progress': `${sliderProgress}%` } as React.CSSProperties}
            aria-label="Context size"
            onChange={(e) => {
              clearNumericDraft(key);
              handleNumericChange(key, sliderValueToContextSize(parseFloat(e.target.value), maxContextWindow));
            }}
          />
          <datalist id="context-size-marks">
            {marks.map(mark => (
              <option key={mark} value={contextSizeToSliderValue(mark, maxContextWindow)} />
            ))}
          </datalist>
          <input
            type="text"
            value={displayValue}
            onChange={(e) => {
              setNumericDrafts(prev => ({ ...prev, [key]: e.target.value }));
            }}
            onBlur={() => {
              const draftValue = numericDrafts[key];
              if (draftValue !== undefined) {
                commitContextSizeDraft(key, draftValue, maxContextWindow);
              }
              clearNumericDraft(key);
            }}
            className="form-input context-size-input"
            placeholder="auto"
            inputMode="numeric"
          />
        </div>
        <div className="context-size-ticks" aria-hidden="true">
          {marks.map((mark) => {
            const left = ((contextSizeToSliderValue(mark, maxContextWindow) - sliderMin) / sliderRange) * 100;
            const thumbOffset = (CONTEXT_SLIDER_THUMB_SIZE / 2) - (left / 100) * CONTEXT_SLIDER_THUMB_SIZE;
            return (
              <span
                key={mark}
                className="context-size-tick"
                style={{ left: `calc(${left}% + ${thumbOffset}px)` }}
              />
            );
          })}
        </div>
        <div className="context-size-scale" aria-hidden="true">
          <span>{formatContextSize(CONTEXT_SLIDER_MIN)}</span>
          <span>{formatContextSize(maxContextWindow)}</span>
        </div>
      </div>
    );
  };

  // Render a numeric input field
  const renderNumericField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'numeric') return null;

    if (key === 'ctxSize' && modelInfo?.max_context_window) {
      return renderContextSizeField(key);
    }

    const value = getOptionValue<number>(key);
    if (value === undefined) return null;
    const displayValue = numericDrafts[key] ?? String(value);

    return (
      <div className="form-section" key={key}>
        <label className="form-label" title={def.description}>{def.label.toLowerCase()}</label>
        <input
          type="text"
          value={displayValue}
          onChange={(e) => {
            const inputValue = e.target.value;
            setNumericDrafts(prev => ({ ...prev, [key]: inputValue }));
          }}
          onBlur={() => {
            const draftValue = numericDrafts[key];
            if (draftValue !== undefined) {
              commitNumericDraft(key, draftValue);
            }
            clearNumericDraft(key);
          }}
          className="form-input"
          placeholder="auto"
        />
      </div>
    );
  };

  // Render a string input field (non-backend)
  const renderStringField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'string' || def.isBackendOption) return null;

    const value = getOptionValue<string>(key);
    if (value === undefined) return null;

    return (
      <div className="form-section" key={key}>
        <label className="form-label" title={def.description}>{def.label.toLowerCase()}</label>
        <input
          type="text"
          className="form-input"
          placeholder=""
          value={value}
          onChange={(e) => handleStringChange(key, e.target.value)}
        />
      </div>
    );
  };

  // Render a backend selector
  const renderBackendSelector = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'string' || !def.isBackendOption) return null;
    if (!hasMultipleBackends || !modelInfo?.recipe) return null;

    const value = getOptionValue<string>(key);
    if (value === undefined) return null;

    return (
      <div className="form-section" key={key}>
        <label className="form-label" title={def.description}>
          {def.label.toLowerCase()}
        </label>
        <select
          className="form-input form-select"
          value={value}
          onChange={(e) => handleStringChange(key, e.target.value)}
        >
          <option value="">Auto</option>
          {(supportedRecipes[modelInfo.recipe] ?? []).map((backend) => (
            <option key={backend} value={backend}>{getBackendDisplayName(backend)}</option>
          ))}
        </select>
      </div>
    );
  };

  // Render a boolean field (checkbox)
  const renderBooleanField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'boolean') return null;

    const value = getOptionValue<boolean>(key);
    const useDefault = getOptionUseDefault(key);
    if (value === undefined) return null;

    return (
      <div
        className={`settings-section ${useDefault ? 'settings-section-default' : ''}`}
        key={key}
      >
        <div className="settings-label-row">
          <span className="settings-label-text">{def.label}</span>
          <button
            type="button"
            className="settings-field-reset"
            onClick={() => handleResetField(key)}
            disabled={useDefault}
          >
            Reset
          </button>
        </div>
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            checked={value}
            onChange={(e) => handleBooleanChange(key, e.target.checked)}
            className="settings-checkbox"
          />
          <div className="settings-checkbox-content">
            <span className="settings-description">
              {def.description}
            </span>
          </div>
        </label>
      </div>
    );
  };

  // Render all options for the current recipe
  const renderOptions = () => {
    return availableOptions.map(key => {
      const def = getOptionDefinition(key);
      if (!def) return null;

      if (def.type === 'numeric') {
        return renderNumericField(key);
      } else if (def.type === 'string') {
        if (def.isBackendOption) {
          return renderBackendSelector(key);
        }
        return renderStringField(key);
      } else if (def.type === 'boolean') {
        return renderBooleanField(key);
      }
      return null;
    });
  };

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
        ) : (
          <div className="model-options-content">
            <div className="model-options-category-header">
              <h3>
                <span className="model-options-field-label">Name:</span>{' '}
                <span className="model-options-name-row">
                  <span className="model-options-field-value">{modelName}</span>
                  <button
                    type="button"
                    className={`model-options-copy-button ${isModelNameCopied ? 'copied' : ''}`}
                    onClick={handleCopyModelName}
                    title={isModelNameCopied ? 'Copied model name' : 'Copy model name'}
                    aria-label={isModelNameCopied ? 'Model name copied' : 'Copy model name'}
                  >
                    {isModelNameCopied ? (
                      <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
                        <path d="M 2,7 L 5.5,10.5 L 12,3" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/>
                      </svg>
                    ) : (
                      <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
                        <rect x="5" y="5" width="7" height="7" rx="1" fill="none" stroke="currentColor" strokeWidth="1.5"/>
                        <path d="M 3,9 L 2,9 C 1.45,9 1,8.55 1,8 L 1,2 C 1,1.45 1.45,1 2,1 L 8,1 C 8.55,1 9,1.45 9,2 L 9,3" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
                      </svg>
                    )}
                  </button>
                </span>
              </h3>
              <h5>
                <span className="model-options-field-label">Checkpoint:</span>{' '}
                <span className="model-options-field-value">
                  {modelUrl ? (
                    <a
                      className="model-options-checkpoint-link"
                      href={modelUrl}
                      target="_blank"
                      rel="noopener noreferrer"
                    >
                      {modelInfo?.checkpoint}
                    </a>
                  ) : modelInfo?.checkpoint}
                </span>
              </h5>
            </div>

            {renderOptions()}
          </div>
        )}

        <div className="settings-footer">
          <button
            className="settings-reset-button"
            onClick={handleReset}
            disabled={isSubmitting || isLoading}
          >
            Reset All
          </button>
          <a className="settings-save-button" ref={exportModelBtn} onClick={handleModelExport} href="" download="">Export Model</a>
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
