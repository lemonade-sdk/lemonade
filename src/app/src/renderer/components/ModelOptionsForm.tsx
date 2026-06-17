import React from 'react';
import { ModelInfo } from '../utils/modelData';
import {
  RecipeOptions,
  getOptionsForRecipe,
  getOptionDefinition,
  clampOptionValue,
} from '../recipes/recipeOptions';
import {
  CONTEXT_SLIDER_MIN,
  CONTEXT_SLIDER_THUMB_SIZE,
  formatContextSize,
  getBackendDisplayName,
  getContextSliderMarks,
  contextSizeToSliderValue,
  sliderValueToContextSize,
} from './modelOptionsFormShared';

export interface ModelOptionsFormProps {
  options: RecipeOptions;
  setOptions: React.Dispatch<React.SetStateAction<RecipeOptions | undefined>>;
  modelInfo?: ModelInfo;
  supportedRecipes: Record<string, string[]>;
  numericDrafts: Record<string, string>;
  setNumericDrafts: React.Dispatch<React.SetStateAction<Record<string, string>>>;
}

const ModelOptionsForm: React.FC<ModelOptionsFormProps> = ({
  options,
  setOptions,
  modelInfo,
  supportedRecipes,
  numericDrafts,
  setNumericDrafts,
}) => {
  const handleNumericChange = (key: string, rawValue: number) => {
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

  const handleStringChange = (key: string, value: string) => {
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

  const handleBooleanChange = (key: string, value: boolean) => {
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

  const recipe = options.recipe;
  const availableOptions = getOptionsForRecipe(recipe);
  const hasMultipleBackends = modelInfo?.recipe && (supportedRecipes[modelInfo.recipe]?.length ?? 0) > 1;

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
            setNumericDrafts(prev => ({ ...prev, [key]: e.target.value }));
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

  return (
    <>
      {availableOptions.map(key => {
        const def = getOptionDefinition(key);
        if (!def) return null;

        if (def.type === 'numeric') {
          return renderNumericField(key);
        }
        if (def.type === 'string') {
          if (def.isBackendOption) {
            return renderBackendSelector(key);
          }
          return renderStringField(key);
        }
        if (def.type === 'boolean') {
          return renderBooleanField(key);
        }
        return null;
      })}
    </>
  );
};

export default ModelOptionsForm;
