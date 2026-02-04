import {
  BooleanOption,
  NumericOption,
  StringOption,
  RecipeOptions,
} from "../recipeOptions";

export type NumericOptionKey = 'ctxSize';
export type StringOptionKey = 'whispercppBackend';
export type BooleanOptionKey = 'saveOptions';

const numericOptionKeys: NumericOptionKey[] = ['ctxSize'];
const stringOptionKeys: StringOptionKey[] = ['whispercppBackend'];
const booleanOptionKeys: BooleanOptionKey[] = ['saveOptions'];

export interface WhisperOptions {
  recipe: 'whispercpp'
  ctxSize: NumericOption,
  whispercppBackend: StringOption,
  saveOptions: BooleanOption
}

export const NUMERIC_OPTION_LIMITS: Record<NumericOptionKey, { min: number; max: number; step: number }> = {
  ctxSize: {min: 0, max: 99999999, step: 0.1},
};

export const createDefaultOptions = (): RecipeOptions => ({
  recipe: 'whispercpp',
  ctxSize: {value: DEFAULT_OPTION_VALUES.ctxSize, useDefault: true},
  whispercppBackend: {value: DEFAULT_OPTION_VALUES.whispercppBackend, useDefault: true},
  saveOptions: {value: DEFAULT_OPTION_VALUES.saveOptions, useDefault: true},
});

export const cloneOptions = (options: WhisperOptions): RecipeOptions => ({
  recipe: 'whispercpp',
  ctxSize: {...options.ctxSize},
  whispercppBackend: {...options.whispercppBackend},
  saveOptions: {...options.saveOptions},
});

export const clampNumericOptionValue = (key: NumericOptionKey, value: number): number => {
  const {min, max} = NUMERIC_OPTION_LIMITS[key];

  if (!Number.isFinite(value)) {
    return DEFAULT_OPTION_VALUES[key];
  }

  return Math.min(Math.max(value, min), max);
};

export const mergeWithDefaultOptions = (incoming?: Partial<RecipeOptions>): RecipeOptions => {
  const defaults = createDefaultOptions();

  if (!incoming) {
    return defaults;
  }

  if (!incoming.recipe) {
    return defaults;
  }
  defaults['recipe'] = incoming.recipe;

  numericOptionKeys.forEach((key) => {
    const rawOption = incoming[key];
    if (!rawOption || typeof rawOption !== 'object') {
      return;
    }

    const numericValue =
        typeof rawOption.value === 'number'
            ? clampNumericOptionValue(key, rawOption.value)
            : defaults[key].value;

    defaults[key] = {
      value: numericValue,
      useDefault: false
    };
  });

  stringOptionKeys.forEach((key) => {
    const incomingWhisper = incoming as Partial<WhisperOptions>;
    const rawOption = incomingWhisper[key];
    if (!rawOption || typeof rawOption !== 'object') {
      return;
    }

    const defaultsWhisper = defaults as WhisperOptions;
    const value =
        typeof rawOption.value === 'string'
            ? rawOption.value
            : defaultsWhisper[key].value;

    defaultsWhisper[key] = {
      value: value,
      useDefault: false
    };
  });

  booleanOptionKeys.forEach((key) => {
    const rawOption = incoming[key];
    if (!rawOption || typeof rawOption !== 'object') {
      return;
    }

    const value =
        typeof rawOption.value === 'boolean'
            ? rawOption.value
            : defaults[key].value;

    defaults[key] = {
      value: value,
      useDefault: false
    };
  });

  return defaults;
};

type DefaultOptionValues =
    Record<NumericOptionKey, number>
    & Record<StringOptionKey, string>
    & Record<BooleanOptionKey, boolean>;

export const DEFAULT_OPTION_VALUES: DefaultOptionValues = {
  ctxSize: 4096,
  whispercppBackend: "",  // Empty string = auto
  saveOptions: true,
};
