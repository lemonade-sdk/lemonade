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
  recipeOptionsToApi,
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

type SpecType = 'none' | 'draft' | 'ngram-simple' | 'ngram-map-k' | 'ngram-mod';

interface SpecState {
  type: SpecType;
  draftModelCheckpoint: string;
  draftMax: number;
  draftMin: number;
  specNgramSizeN: number;
  specNgramSizeM: number;
  specNgramMinHits: number;
  draftPMin: string;
  ctxSizeDraft: string;
  deviceDraft: string;
}

interface DraftModelChoice {
  id: string;
  checkpoint: string;
}

type SpecNumericFieldKey = 'draftMax' | 'draftMin' | 'specNgramSizeN' | 'specNgramSizeM' | 'specNgramMinHits';

type SpecPresetId = 'custom' | 'safe-default' | 'ngram-simple-code' | 'ngram-map-k-keys' | 'ngram-mod-reasoning';

const SPEC_DEFAULTS: SpecState = {
  type: 'none',
  draftModelCheckpoint: '',
  draftMax: 16,
  draftMin: 0,
  specNgramSizeN: 12,
  specNgramSizeM: 48,
  specNgramMinHits: 1,
  draftPMin: '',
  ctxSizeDraft: '',
  deviceDraft: '',
};

const SPEC_VALUE_FLAGS = new Set([
  '--spec-type',
  '--draft-max',
  '--draft-min',
  '--model-draft',
  '--spec-ngram-size-n',
  '--spec-ngram-size-m',
  '--spec-ngram-min-hits',
  '--draft-p-min',
  '--ctx-size-draft',
  '--device-draft',
]);

const MANAGED_SPEC_FLAGS = new Set([
  '--no-mmproj',
]);

const SPEC_TYPES: Array<{ value: SpecType; label: string }> = [
  { value: 'none', label: 'None' },
  { value: 'draft', label: 'Draft Model' },
  { value: 'ngram-simple', label: 'Self-Spec: N-gram Simple' },
  { value: 'ngram-map-k', label: 'Self-Spec: N-gram Map-K' },
  { value: 'ngram-mod', label: 'Self-Spec: N-gram Mod' },
];

const SPEC_PRESETS: Array<{ id: SpecPresetId; label: string; state: Partial<SpecState> }> = [
  {
    id: 'safe-default',
    label: 'Safe / Default',
    state: {
      type: 'ngram-simple',
      draftMax: 16,
      draftMin: 0,
      specNgramSizeN: 12,
      specNgramSizeM: 48,
      specNgramMinHits: 1,
    },
  },
  {
    id: 'ngram-simple-code',
    label: 'N-gram Simple: Code Rewrite / Repetition',
    state: {
      type: 'ngram-simple',
      draftMax: 64,
      draftMin: 0,
    },
  },
  {
    id: 'ngram-map-k-keys',
    label: 'N-gram Map-K: Repeated Key Patterns',
    state: {
      type: 'ngram-map-k',
      draftMax: 64,
      draftMin: 0,
      specNgramMinHits: 1,
    },
  },
  {
    id: 'ngram-mod-reasoning',
    label: 'N-gram Mod: Reasoning / Summarization',
    state: {
      type: 'ngram-mod',
      specNgramSizeN: 24,
      draftMin: 48,
      draftMax: 64,
    },
  },
];

const normalizeSpecType = (value: string): SpecType => {
  if (value === 'draft' || value === 'ngram-simple' || value === 'ngram-map-k' || value === 'ngram-mod') {
    return value;
  }
  return 'none';
};

const tokenizeArgs = (input: string): string[] => {
  if (!input.trim()) return [];

  const tokens: string[] = [];
  let current = '';
  let quote: '"' | "'" | null = null;

  for (let i = 0; i < input.length; i += 1) {
    const c = input[i];

    if (c === '\\' && i + 1 < input.length) {
      const next = input[i + 1];

      if (quote) {
        if (next === quote || next === '\\') {
          current += next;
          i += 1;
          continue;
        }
      } else if (/\s|["'\\]/.test(next)) {
        current += next;
        i += 1;
        continue;
      }
    }

    if (!quote && (c === '"' || c === "'")) {
      quote = c;
      continue;
    }

    if (quote && c === quote) {
      quote = null;
      continue;
    }

    if (!quote && /\s/.test(c)) {
      if (current.length > 0) {
        tokens.push(current);
        current = '';
      }
      continue;
    }

    current += c;
  }

  if (current.length > 0) tokens.push(current);
  return tokens;
};

const quoteArgIfNeeded = (token: string): string => {
  if (!/[\s"']/.test(token)) return token;
  return `"${token.replace(/(["\\])/g, '\\$1')}"`;
};

const serializeTokens = (tokens: string[]): string => tokens.map(quoteArgIfNeeded).join(' ').trim();

const toIntOrDefault = (raw: string, fallback: number): number => {
  const parsed = parseInt(raw, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
};

const parseSpecFromArgs = (llamacppArgs: string): { state: SpecState; nonSpecTokens: string[] } => {
  const tokens = tokenizeArgs(llamacppArgs);
  const state: SpecState = { ...SPEC_DEFAULTS };
  const nonSpecTokens: string[] = [];
  let sawDraftModel = false;

  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    const eqPos = token.indexOf('=');
    const flag = eqPos >= 0 ? token.slice(0, eqPos) : token;

    if (MANAGED_SPEC_FLAGS.has(flag)) {
      // Managed runtime flags are never user-facing options in this modal.
      continue;
    }

    if (!SPEC_VALUE_FLAGS.has(flag)) {
      nonSpecTokens.push(token);
      continue;
    }

    let value = '';
    if (eqPos >= 0) {
      value = token.slice(eqPos + 1);
    } else if (i + 1 < tokens.length) {
      value = tokens[i + 1];
      i += 1;
    }

    switch (flag) {
      case '--spec-type':
        state.type = normalizeSpecType(value);
        break;
      case '--draft-max':
        state.draftMax = toIntOrDefault(value, state.draftMax);
        break;
      case '--draft-min':
        state.draftMin = toIntOrDefault(value, state.draftMin);
        break;
      case '--model-draft':
        state.draftModelCheckpoint = value;
        sawDraftModel = value.trim().length > 0;
        break;
      case '--spec-ngram-size-n':
        state.specNgramSizeN = toIntOrDefault(value, state.specNgramSizeN);
        break;
      case '--spec-ngram-size-m':
        state.specNgramSizeM = toIntOrDefault(value, state.specNgramSizeM);
        break;
      case '--spec-ngram-min-hits':
        state.specNgramMinHits = toIntOrDefault(value, state.specNgramMinHits);
        break;
      case '--draft-p-min':
        state.draftPMin = value;
        break;
      case '--ctx-size-draft':
        state.ctxSizeDraft = value;
        break;
      case '--device-draft':
        state.deviceDraft = value;
        break;
      default:
        break;
    }
  }

  // Draft mode may omit --spec-type by design, so infer it from --model-draft.
  if (sawDraftModel && state.type === 'none') {
    state.type = 'draft';
  }

  return { state, nonSpecTokens };
};

const serializeSpecToArgs = (state: SpecState, nonSpecTokens: string[]): string => {
  const tokens = [...nonSpecTokens];
  const draftCheckpoint = state.draftModelCheckpoint.trim();

  if (state.type !== 'none') {
    // Keep --spec-type draft only as a temporary UI state marker when no draft
    // checkpoint is selected yet. Once --model-draft is present, omit --spec-type.
    if (state.type !== 'draft' || !draftCheckpoint) {
      tokens.push('--spec-type', state.type);
    }
    tokens.push('--draft-max', String(Math.max(0, Math.trunc(state.draftMax))));
    tokens.push('--draft-min', String(Math.max(0, Math.trunc(state.draftMin))));

    if (state.type === 'draft' && draftCheckpoint) {
      tokens.push('--model-draft', draftCheckpoint);
    }

    if (state.type === 'ngram-simple' || state.type === 'ngram-mod') {
      tokens.push('--spec-ngram-size-n', String(Math.max(1, Math.trunc(state.specNgramSizeN))));
    }

    if (state.type === 'ngram-mod') {
      tokens.push('--spec-ngram-size-m', String(Math.max(1, Math.trunc(state.specNgramSizeM))));
    }

    if (state.type === 'ngram-map-k') {
      tokens.push('--spec-ngram-min-hits', String(Math.max(1, Math.trunc(state.specNgramMinHits))));
    }

    if (state.draftPMin.trim()) {
      tokens.push('--draft-p-min', state.draftPMin.trim());
    }
    if (state.ctxSizeDraft.trim()) {
      tokens.push('--ctx-size-draft', state.ctxSizeDraft.trim());
    }
    if (state.deviceDraft.trim()) {
      tokens.push('--device-draft', state.deviceDraft.trim());
    }
  }

  return serializeTokens(tokens);
};

const getCommittedOptions = (
  sourceOptions: RecipeOptions,
  drafts: Record<string, string>,
): RecipeOptions => {
  let committed = sourceOptions;

  for (const [key, draftValue] of Object.entries(drafts)) {
    const trimmed = draftValue.trim();
    if (trimmed === '') continue;

    const parsed = parseFloat(trimmed);
    if (Number.isNaN(parsed)) continue;

    committed = {
      ...committed,
      [key]: {
        value: clampOptionValue(key, parsed),
        useDefault: false,
      },
    } as RecipeOptions;
  }

  return committed;
};

const matchesPreset = (state: SpecState, presetState: Partial<SpecState>): boolean => {
  const entries = Object.entries(presetState) as Array<[keyof SpecState, SpecState[keyof SpecState]]>;
  return entries.every(([key, value]) => state[key] === value);
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
  const [specNumericDrafts, setSpecNumericDrafts] = useState<Partial<Record<'draftMax' | 'draftMin' | 'specNgramSizeN' | 'specNgramSizeM' | 'specNgramMinHits', string>>>({});
  const [llamacppArgsDraft, setLlamacppArgsDraft] = useState<string | null>(null);
  const [specState, setSpecState] = useState<SpecState>(SPEC_DEFAULTS);
  const [specNonTokens, setSpecNonTokens] = useState<string[]>([]);
  const [selectedSpecPreset, setSelectedSpecPreset] = useState<SpecPresetId>('custom');
  const [showSpecAdvanced, setShowSpecAdvanced] = useState(false);
  const [draftModelChoices, setDraftModelChoices] = useState<DraftModelChoice[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);
  const exportModelBtn = useRef<HTMLAnchorElement | null>(null);

  // Fetch options when modal opens
  useEffect(() => {
    if (!isOpen) return;
    let isMounted = true;
    setNumericDrafts({});
    setSpecNumericDrafts({});
    setLlamacppArgsDraft(null);
    setSpecState(SPEC_DEFAULTS);
    setSpecNonTokens([]);
    setSelectedSpecPreset('custom');
    setShowSpecAdvanced(false);
    setDraftModelChoices([]);
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

        const modelsResponse = await serverFetch('/models?show_all=true');
        const modelsPayload = await modelsResponse.json();
        const modelsList = Array.isArray(modelsPayload) ? modelsPayload : modelsPayload.data || [];

        const draftChoices: DraftModelChoice[] = modelsList
          .filter((item: any) => item?.recipe === 'llamacpp' && item?.downloaded === true && typeof item?.checkpoint === 'string')
          .map((item: any) => ({ id: String(item.id), checkpoint: String(item.checkpoint) }));

        setModelName(model);
        setModelInfo({ ...data });

        const url = `https://huggingface.co/${data.checkpoint.replace(/:.+$/, '')}`;
        if (url) setModelUrl(url);

        const recipe = data.recipe as string;
        const recipeOptions = data.recipe_options ?? {};

        if (isMounted) {
          setOptions(apiToRecipeOptions(recipe, recipeOptions));
          setDraftModelChoices(draftChoices);
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
    if (!options || options.recipe !== 'llamacpp') {
      setSpecNumericDrafts({});
      setSpecState(SPEC_DEFAULTS);
      setSpecNonTokens([]);
      setSelectedSpecPreset('custom');
      return;
    }

    if (llamacppArgsDraft !== null) {
      // Don't rewrite arguments while user is actively typing.
      return;
    }

    const currentArgs = (options as any).llamacppArgs?.value ?? '';
    const parsed = parseSpecFromArgs(currentArgs);
    const normalizedArgs = serializeSpecToArgs(parsed.state, parsed.nonSpecTokens);

    if (normalizedArgs !== currentArgs) {
      setOptions((prev) => {
        if (!prev || prev.recipe !== 'llamacpp') return prev;

        const previousArgs = (prev as any).llamacppArgs?.value ?? '';
        if (previousArgs === normalizedArgs) {
          return prev;
        }

        return {
          ...prev,
          llamacppArgs: {
            value: normalizedArgs,
            useDefault: false,
          },
        } as RecipeOptions;
      });
    }

    setSpecState(parsed.state);
    setSpecNonTokens(parsed.nonSpecTokens);

    const preset = SPEC_PRESETS.find((candidate) => matchesPreset(parsed.state, candidate.state));
    setSelectedSpecPreset(preset ? preset.id : 'custom');
  }, [options, llamacppArgsDraft]);

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

  const handleModelExport = () => {
    if (!modelInfo || !options) return;

    const committedOptions = getCommittedOptions(options, numericDrafts);
    const recipeOptions = recipeOptionsToApi(committedOptions);
    const modelId = String(modelInfo.id ?? modelName);
    const exportName = modelId.startsWith('user.') ? modelId : `user.${modelId}`;

    const modelToExport: Record<string, unknown> = {
      model_name: exportName,
      downloaded: modelInfo.downloaded,
      labels: modelInfo.labels,
      recipe: modelInfo.recipe,
      recipe_options: recipeOptions,
      size: modelInfo.size,
      checkpoints: modelInfo.checkpoints,
      image_defaults: modelInfo.image_defaults,
    };

    if (!modelInfo.checkpoints) {
      Object.assign(modelToExport, { checkpoint: modelInfo.checkpoint });
    }

    const model = JSON.stringify(modelToExport);
    const blob = new Blob([model], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    exportModelBtn!.current!.href = url;
    exportModelBtn!.current!.download = typeof modelToExport.model_name === 'string' ? `${modelToExport.model_name}.json` : 'model.json';
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };

  const handleCancel = () => {
    onCancel();
  };

  const handleSubmit = () => {
    if (!options || !modelName) return;

    onSubmit(modelName, getCommittedOptions(options, numericDrafts));
  };

  const applySpecState = (nextState: SpecState, preset: SpecPresetId = 'custom') => {
    setSpecNumericDrafts({});
    setSpecState(nextState);
    setSelectedSpecPreset(preset);

    if (options?.recipe !== 'llamacpp') {
      return;
    }

    const mergedArgs = serializeSpecToArgs(nextState, specNonTokens);
    handleStringChange('llamacppArgs', mergedArgs);
  };

  const updateSpecField = <K extends keyof SpecState>(key: K, value: SpecState[K]) => {
    if (key === 'type' && value === 'none') {
      applySpecState({ ...SPEC_DEFAULTS, type: 'none' }, 'custom');
      return;
    }
    applySpecState({ ...specState, [key]: value }, 'custom');
  };

  const handleSpecPresetChange = (presetId: SpecPresetId) => {
    if (presetId === 'custom') {
      setSelectedSpecPreset('custom');
      return;
    }

    const preset = SPEC_PRESETS.find((item) => item.id === presetId);
    if (!preset) return;

    const nextState: SpecState = {
      ...specState,
      ...preset.state,
    };
    applySpecState(nextState, presetId);
  };

  const setSpecNumericDraft = (key: SpecNumericFieldKey, input: string) => {
    if (!/^[0-9]*$/.test(input)) return;
    setSpecNumericDrafts((prev) => ({ ...prev, [key]: input }));
  };

  const clearSpecNumericDraft = (key: SpecNumericFieldKey) => {
    setSpecNumericDrafts((prev) => {
      if (!(key in prev)) return prev;
      const next = { ...prev };
      delete next[key];
      return next;
    });
  };

  const commitSpecNumericDraft = (key: SpecNumericFieldKey, minValue: number) => {
    const input = specNumericDrafts[key];
    if (input !== undefined && input.trim() !== '') {
      const currentValue = specState[key];
      const committed = Math.max(minValue, Math.trunc(toIntOrDefault(input, currentValue)));
      updateSpecField(key, committed);
    }
    clearSpecNumericDraft(key);
  };

  const renderSpecNumericField = (label: string, key: SpecNumericFieldKey, minValue: number) => {
    const draft = specNumericDrafts[key];
    return (
      <div className="form-section">
        <label className="form-label">{label}</label>
        <input
          type="text"
          className="form-input"
          inputMode="numeric"
          value={draft ?? String(specState[key])}
          onChange={(e) => setSpecNumericDraft(key, e.target.value)}
          onBlur={() => commitSpecNumericDraft(key, minValue)}
        />
      </div>
    );
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

    const isLlamacppArgs = key === 'llamacppArgs' && recipe === 'llamacpp';
    const stringValue = isLlamacppArgs && llamacppArgsDraft !== null ? llamacppArgsDraft : value;

    return (
      <div className="form-section" key={key}>
        <label className="form-label" title={def.description}>{def.label.toLowerCase()}</label>
        <input
          type="text"
          className="form-input"
          placeholder=""
          value={stringValue}
          onFocus={() => {
            if (isLlamacppArgs && llamacppArgsDraft === null) {
              setLlamacppArgsDraft(value);
            }
          }}
          onBlur={() => {
            if (!isLlamacppArgs || llamacppArgsDraft === null) return;
            handleStringChange(key, llamacppArgsDraft);
            setLlamacppArgsDraft(null);
          }}
          onChange={(e) => {
            if (isLlamacppArgs) {
              setLlamacppArgsDraft(e.target.value);
              return;
            }
            handleStringChange(key, e.target.value);
          }}
        />

        {isLlamacppArgs && (
          <div className="spec-decoding-panel">
            <div className="spec-decoding-header">Speculative Decoding</div>

            <div className="spec-row two-col">
              <div className="form-section">
                <label className="form-label">Type</label>
                <select
                  className="form-input form-select"
                  value={specState.type}
                  onChange={(e) => updateSpecField('type', normalizeSpecType(e.target.value))}
                >
                  {SPEC_TYPES.map((item) => (
                    <option key={item.value} value={item.value}>{item.label}</option>
                  ))}
                </select>
              </div>

              <div className="form-section">
                <label className="form-label">Preset</label>
                <select
                  className="form-input form-select"
                  value={selectedSpecPreset}
                  onChange={(e) => handleSpecPresetChange(e.target.value as SpecPresetId)}
                  disabled={specState.type === 'draft'}
                >
                  <option value="custom">Custom</option>
                  {SPEC_PRESETS.map((preset) => (
                    <option key={preset.id} value={preset.id}>{preset.label}</option>
                  ))}
                </select>
              </div>
            </div>

            {specState.type !== 'none' && (
              <>
                <div className="spec-row two-col">
                  {renderSpecNumericField('Draft Max', 'draftMax', 0)}
                  {renderSpecNumericField('Draft Min', 'draftMin', 0)}
                </div>

                {specState.type === 'draft' && (
                  <div className="spec-row">
                    <div className="form-section">
                      <label className="form-label">Draft Model</label>
                      <select
                        className="form-input form-select"
                        value={specState.draftModelCheckpoint}
                        onChange={(e) => updateSpecField('draftModelCheckpoint', e.target.value)}
                      >
                        <option value="">Select a downloaded llama model</option>
                        {draftModelChoices.map((choice) => (
                          <option key={choice.id} value={choice.checkpoint}>
                            {choice.id} ({choice.checkpoint})
                          </option>
                        ))}
                      </select>
                      <div className="spec-warning-box">
                        Use a draft model with the same architecture and fewer parameters for better performance.
                      </div>
                    </div>
                  </div>
                )}

                {(specState.type === 'ngram-simple' || specState.type === 'ngram-map-k' || specState.type === 'ngram-mod') && (
                  <div className="spec-row two-col">
                    {(specState.type === 'ngram-simple' || specState.type === 'ngram-mod') && (
                      renderSpecNumericField('N-gram Size N', 'specNgramSizeN', 1)
                    )}

                    {specState.type === 'ngram-mod' && (
                      renderSpecNumericField('N-gram Size M', 'specNgramSizeM', 1)
                    )}

                    {specState.type === 'ngram-map-k' && (
                      renderSpecNumericField('N-gram Min Hits', 'specNgramMinHits', 1)
                    )}
                  </div>
                )}

                <button
                  type="button"
                  className="spec-advanced-toggle"
                  onClick={() => setShowSpecAdvanced((prev) => !prev)}
                >
                  {showSpecAdvanced ? 'Hide Advanced Options' : 'Show Advanced Options'}
                </button>

                {showSpecAdvanced && (
                  <div className="spec-row three-col">
                    <div className="form-section">
                      <label className="form-label">Draft P Min</label>
                      <input
                        type="text"
                        className="form-input"
                        placeholder="0.75"
                        value={specState.draftPMin}
                        onChange={(e) => updateSpecField('draftPMin', e.target.value)}
                      />
                    </div>

                    <div className="form-section">
                      <label className="form-label">Ctx Size Draft</label>
                      <input
                        type="text"
                        className="form-input"
                        placeholder="0"
                        value={specState.ctxSizeDraft}
                        onChange={(e) => updateSpecField('ctxSizeDraft', e.target.value)}
                      />
                    </div>

                    <div className="form-section">
                      <label className="form-label">Device Draft</label>
                      <input
                        type="text"
                        className="form-input"
                        placeholder="cpu,gpu"
                        value={specState.deviceDraft}
                        onChange={(e) => updateSpecField('deviceDraft', e.target.value)}
                      />
                    </div>
                  </div>
                )}
              </>
            )}
          </div>
        )}
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
                <span className="model-options-field-value">{modelName}</span>
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
