import {
  BooleanOption,
  NumericOption,
  RecipeOptions,
} from "../recipeOptions";

export type NumericOptionKey = 'ctxSize';
export type BooleanOptionKey = 'saveOptions';

const numericOptionKeys: NumericOptionKey[] = ['ctxSize'];
const booleanOptionKeys: BooleanOptionKey[] = ['saveOptions'];

type OgaRecipe = 'oga-cpu' | 'oga-npu' | 'oga-hybrid' | 'oga-igpu';
export const OgaRecipies: OgaRecipe[] = ['oga-cpu', 'oga-hybrid', 'oga-npu', 'oga-igpu'];

export interface OgaOptions {
  recipe: OgaRecipe,
  ctxSize: NumericOption,
  saveOptions: BooleanOption
}

export const NUMERIC_OPTION_LIMITS: Record<NumericOptionKey, { min: number; max: number; step: number }> = {
  ctxSize: {min: 0, max: 99999999, step: 0.1},
};

export const createDefaultOptions = (): RecipeOptions => ({
  recipe: DEFAULT_OPTION_VALUES.recipe,
  ctxSize: {value: DEFAULT_OPTION_VALUES.ctxSize, useDefault: true},
  saveOptions: {value: DEFAULT_OPTION_VALUES.saveOptions, useDefault: true},
});

export const cloneOptions = (options: OgaOptions): RecipeOptions => ({
  recipe: options.recipe,
  ctxSize: {...options.ctxSize},
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
    & Record<BooleanOptionKey, boolean>
    & {
  recipe: OgaRecipe;
  saveOptions: boolean;
};

export const DEFAULT_OPTION_VALUES: DefaultOptionValues = {
  recipe: 'oga-cpu',
  ctxSize: 4096,
  saveOptions: true,
};