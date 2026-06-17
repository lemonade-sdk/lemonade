/**
 * Model Settings Modal
 *
 * Edit persisted model defaults and user-defined API aliases for downloaded models.
 */

import React, { useState, useEffect, useRef } from 'react';
import { serverFetch } from './utils/serverConfig';
import { ModelInfo, updateModelSettings } from './utils/modelData';
import { useSystem } from './hooks/useSystem';
import { writeClipboard } from './utils/clipboardUtils';
import {
  RecipeOptions,
  createDefaultOptions,
  apiToRecipeOptions,
  recipeOptionsToApi,
  clampOptionValue,
} from './recipes/recipeOptions';
import ModelOptionsForm from './components/ModelOptionsForm';
import { formatAliasInput, parseAliasInput } from './components/modelOptionsFormShared';

interface ModelSettingsModalProps {
  isOpen: boolean;
  onCancel: () => void;
  onSaved?: () => void;
  model: string | null;
}

const ModelSettingsModal: React.FC<ModelSettingsModalProps> = ({
  isOpen,
  onCancel,
  onSaved,
  model,
}) => {
  const { supportedRecipes, ensureSystemInfoLoaded } = useSystem();
  const [modelInfo, setModelInfo] = useState<ModelInfo>();
  const [modelName, setModelName] = useState('');
  const [modelUrl, setModelUrl] = useState('');
  const [options, setOptions] = useState<RecipeOptions>();
  const [aliasInput, setAliasInput] = useState('');
  const [numericDrafts, setNumericDrafts] = useState<Record<string, string>>({});
  const [isLoading, setIsLoading] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [isSaving, setIsSaving] = useState(false);
  const [isModelNameCopied, setIsModelNameCopied] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);
  const modelNameCopyTimeoutIdRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    if (!isOpen) return;
    let isMounted = true;
    setNumericDrafts({});
    setIsModelNameCopied(false);
    if (modelNameCopyTimeoutIdRef.current) {
      clearTimeout(modelNameCopyTimeoutIdRef.current);
      modelNameCopyTimeoutIdRef.current = null;
    }
    setLoadError(null);
    setSaveError(null);
    setModelInfo(undefined);
    setModelName(model ?? '');
    setModelUrl('');
    setOptions(undefined);
    setAliasInput('');
    void ensureSystemInfoLoaded();

    const fetchSettings = async () => {
      if (isMounted) setIsLoading(true);
      if (!model) {
        if (isMounted) {
          setLoadError('No model selected.');
          setIsLoading(false);
        }
        return;
      }

      try {
        const response = await serverFetch(`/models/${encodeURIComponent(model)}`);
        if (!response.ok) {
          throw new Error(`Failed to load model settings (${response.status})`);
        }
        const data = await response.json();

        if (!isMounted) return;

        setModelName(model);
        setModelInfo({ ...data });

        const checkpoint = typeof data.checkpoint === 'string' ? data.checkpoint : '';
        setModelUrl(checkpoint ? `https://huggingface.co/${checkpoint.replace(/:.+$/, '')}` : '');

        const recipe = data.recipe as string;
        const recipeOptions = data.recipe_options ?? {};
        setOptions(apiToRecipeOptions(recipe, recipeOptions));

        const aliases = Array.isArray(data.aliases)
          ? data.aliases.filter((entry: unknown): entry is string => typeof entry === 'string')
          : [];
        setAliasInput(formatAliasInput(aliases));
      } catch (error) {
        console.error('Failed to load model settings:', error);
        if (isMounted) {
          setLoadError('Failed to load model settings.');
        }
      } finally {
        if (isMounted) setIsLoading(false);
      }
    };

    void fetchSettings();
    return () => { isMounted = false; };
  }, [isOpen, model, ensureSystemInfoLoaded]);

  useEffect(() => {
    return () => {
      if (modelNameCopyTimeoutIdRef.current) {
        clearTimeout(modelNameCopyTimeoutIdRef.current);
      }
    };
  }, []);

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

  const buildSubmitOptions = (): RecipeOptions | undefined => {
    if (!options) return undefined;

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

    return submitOptions;
  };

  const handleSave = async () => {
    if (!modelName) return;
    const submitOptions = buildSubmitOptions();
    if (!submitOptions) return;

    setSaveError(null);
    setIsSaving(true);
    try {
      await updateModelSettings(modelName, {
        recipe_options: recipeOptionsToApi(submitOptions),
        aliases: parseAliasInput(aliasInput),
      });
      onSaved?.();
      onCancel();
    } catch (error) {
      console.error('Failed to save model settings:', error);
      setSaveError(error instanceof Error ? error.message : 'Failed to save model settings.');
    } finally {
      setIsSaving(false);
    }
  };

  if (!isOpen) return null;

  const renderHeader = () => (
    <div className="settings-header">
      <h3>Model Settings</h3>
      <button className="settings-close-button" onClick={onCancel} title="Close">
        <svg width="14" height="14" viewBox="0 0 14 14">
          <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
        </svg>
      </button>
    </div>
  );

  if (!options) {
    return (
      <div className="settings-overlay">
        <div className="settings-modal" ref={cardRef} onMouseDown={(e) => e.stopPropagation()}>
          {renderHeader()}
          <div className="settings-loading">
            {loadError ?? 'Loading settings...'}
          </div>
          {loadError && (
            <div className="settings-footer">
              <button className="settings-save-button" onClick={onCancel}>Cancel</button>
            </div>
          )}
        </div>
      </div>
    );
  }

  return (
    <div className="settings-overlay">
      <div className="settings-modal" ref={cardRef} onMouseDown={(e) => e.stopPropagation()}>
        {renderHeader()}

        {isLoading ? (
          <div className="settings-loading">Loading settings...</div>
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

            <div className="form-section">
              <label className="form-label" htmlFor="model-alias-input">
                aliases
              </label>
              <input
                id="model-alias-input"
                type="text"
                className="form-input"
                value={aliasInput}
                onChange={(e) => setAliasInput(e.target.value)}
                placeholder="gemma4, gemma4:latest"
              />
              <span className="settings-description">
                API names that resolve to this model (comma-separated).
              </span>
            </div>

            <ModelOptionsForm
              options={options}
              setOptions={setOptions}
              modelInfo={modelInfo}
              supportedRecipes={supportedRecipes}
              numericDrafts={numericDrafts}
              setNumericDrafts={setNumericDrafts}
            />
          </div>
        )}

        {saveError && (
          <div className="settings-export-error">{saveError}</div>
        )}
        <div className="settings-footer">
          <button
            className="settings-reset-button"
            onClick={handleReset}
            disabled={isSaving || isLoading}
          >
            Reset All
          </button>
          <button
            className="settings-save-button"
            onClick={onCancel}
            disabled={isSaving || isLoading}
          >
            Cancel
          </button>
          <button
            className="settings-save-button"
            onClick={handleSave}
            disabled={isSaving || isLoading}
          >
            {isSaving ? 'Saving...' : 'Save'}
          </button>
        </div>
      </div>
    </div>
  );
};

export default ModelSettingsModal;
