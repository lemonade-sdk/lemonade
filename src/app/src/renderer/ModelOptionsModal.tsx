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

interface SettingsModalProps {
  isOpen: boolean;
  onSubmit: (modelName: string, options: RecipeOptions) => void;
  onCancel: () => void;
  model: string | null;
}

const ModelOptionsModal: React.FC<SettingsModalProps> = ({ isOpen, onCancel, onSubmit, model }) => {
  const { supportedRecipes } = useSystem();
  const [modelInfo, setModelInfo] = useState<ModelInfo>();
  const [modelName, setModelName] = useState("");
  const [modelUrl, setModelUrl] = useState<string>("");
  const [options, setOptions] = useState<RecipeOptions>();
  const [isLoading, setIsLoading] = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);

  // Fetch options when modal opens
  useEffect(() => {
    if (!isOpen) return;
    let isMounted = true;

    const fetchOptions = async () => {
      if (isMounted) setIsLoading(true);
      if (!model) return;

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
  }, [isOpen]);

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
    setOptions(createDefaultOptions(options.recipe));
  };

  const handleCancel = () => {
    onCancel();
  };

  const handleSubmit = () => {
    if (!options || !modelName) return;
    onSubmit(modelName, options);
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

  // Render a numeric input field
  const renderNumericField = (key: string) => {
    const def = getOptionDefinition(key);
    if (!def || def.type !== 'numeric') return null;

    const value = getOptionValue<number>(key);
    if (value === undefined) return null;

    return (
      <div className="form-section" key={key}>
        <label className="form-label" title={def.description}>{def.label.toLowerCase()}</label>
        <input
          type="text"
          value={value}
          onChange={(e) => {
            if (e.target.value === 'auto' || e.target.value === '') return;
            const parsed = parseFloat(e.target.value);
            if (Number.isNaN(parsed)) return;
            handleNumericChange(key, parsed);
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
              <h3>Name: {modelName}</h3>
              <h5>Checkpoint: {modelUrl ? (
                <a href={modelUrl} target="_blank">{modelInfo?.checkpoint}</a>
              ) : modelInfo?.checkpoint}</h5>
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
