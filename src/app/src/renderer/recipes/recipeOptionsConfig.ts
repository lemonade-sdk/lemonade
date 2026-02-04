/**
 * Recipe Options Configuration
 *
 * This is the SINGLE SOURCE OF TRUTH for what options are available to each recipe.
 * It mirrors the C++ implementation in src/cpp/server/recipe_options.cpp
 *
 * All recipe option types, defaults, and utilities are defined here.
 * The per-recipe files have been eliminated to avoid duplication.
 */

// =============================================================================
// Base Option Types
// =============================================================================

export interface NumericOption {
  value: number;
  useDefault: boolean;
}

export interface StringOption {
  value: string;
  useDefault: boolean;
}

export interface BooleanOption {
  value: boolean;
  useDefault: boolean;
}

// =============================================================================
// Recipe-Specific Option Interfaces
// =============================================================================

export interface LlamaOptions {
  recipe: 'llamacpp';
  ctxSize: NumericOption;
  llamacppBackend: StringOption;
  llamacppArgs: StringOption;
  saveOptions: BooleanOption;
}

export interface WhisperOptions {
  recipe: 'whispercpp';
  whispercppBackend: StringOption;
  saveOptions: BooleanOption;
}

export interface FlmOptions {
  recipe: 'flm';
  ctxSize: NumericOption;
  saveOptions: BooleanOption;
}

export type OgaRecipe = 'oga-cpu' | 'oga-npu' | 'oga-hybrid';

export interface OgaOptions {
  recipe: OgaRecipe;
  ctxSize: NumericOption;
  saveOptions: BooleanOption;
}

export interface StableDiffusionOptions {
  recipe: 'sd-cpp';
  steps: NumericOption;
  cfgScale: NumericOption;
  width: NumericOption;
  height: NumericOption;
  saveOptions: BooleanOption;
}

// Union type of all recipe options
export type RecipeOptions = LlamaOptions | WhisperOptions | FlmOptions | OgaOptions | StableDiffusionOptions;

// =============================================================================
// Recipe Constants
// =============================================================================

export const OGA_RECIPES: OgaRecipe[] = ['oga-cpu', 'oga-npu', 'oga-hybrid'];

/**
 * Checks if a recipe name is an OGA recipe
 */
export function isOgaRecipe(recipe: string): boolean {
  return OGA_RECIPES.includes(recipe as OgaRecipe);
}

// =============================================================================
// Option Definition Types
// =============================================================================

export type OptionType = 'numeric' | 'string' | 'boolean';

export interface NumericOptionDef {
  type: 'numeric';
  default: number;
  min: number;
  max: number;
  step: number;
  label: string;
  description?: string;
}

export interface StringOptionDef {
  type: 'string';
  default: string;
  label: string;
  description?: string;
  isBackendOption?: boolean;
  backendRecipe?: string;
}

export interface BooleanOptionDef {
  type: 'boolean';
  default: boolean;
  label: string;
  description?: string;
}

export type OptionDef = NumericOptionDef | StringOptionDef | BooleanOptionDef;

// =============================================================================
// Option Definitions - All possible options and their properties
// =============================================================================

export const OPTION_DEFINITIONS: Record<string, OptionDef> = {
  // LLM context size option (shared by llamacpp, flm, oga-*)
  ctxSize: {
    type: 'numeric',
    default: 4096,
    min: 0,
    max: 99999999,
    step: 1,
    label: 'Context Size',
    description: 'Context size for the model',
  },

  // LlamaCpp-specific options
  llamacppBackend: {
    type: 'string',
    default: '',
    label: 'Backend',
    description: 'LlamaCpp backend to use',
    isBackendOption: true,
    backendRecipe: 'llamacpp',
  },
  llamacppArgs: {
    type: 'string',
    default: '',
    label: 'LlamaCpp Arguments',
    description: 'Custom arguments to pass to llama-server',
  },

  // WhisperCpp-specific options
  whispercppBackend: {
    type: 'string',
    default: '',
    label: 'Backend',
    description: 'WhisperCpp backend to use (npu or cpu)',
    isBackendOption: true,
    backendRecipe: 'whispercpp',
  },

  // Stable Diffusion options
  steps: {
    type: 'numeric',
    default: 20,
    min: 1,
    max: 150,
    step: 1,
    label: 'Steps',
    description: 'Number of inference steps for image generation',
  },
  cfgScale: {
    type: 'numeric',
    default: 7.0,
    min: 0,
    max: 30,
    step: 0.1,
    label: 'CFG Scale',
    description: 'Classifier-free guidance scale',
  },
  width: {
    type: 'numeric',
    default: 512,
    min: 64,
    max: 2048,
    step: 64,
    label: 'Width',
    description: 'Image width in pixels',
  },
  height: {
    type: 'numeric',
    default: 512,
    min: 64,
    max: 2048,
    step: 64,
    label: 'Height',
    description: 'Image height in pixels',
  },

  // Common option - save settings
  saveOptions: {
    type: 'boolean',
    default: true,
    label: 'Save Options',
    description: 'Save these options in lemonade server for future loads',
  },
};

// =============================================================================
// Recipe Configuration - Maps recipes to their available options
// =============================================================================

export type RecipeName = 'llamacpp' | 'whispercpp' | 'flm' | 'oga-cpu' | 'oga-npu' | 'oga-hybrid' | 'sd-cpp';

/**
 * Maps recipe names to the option keys they support.
 * This mirrors the C++ get_keys_for_recipe() function in recipe_options.cpp
 */
export const RECIPE_OPTIONS_MAP: Record<RecipeName, string[]> = {
  'llamacpp': ['ctxSize', 'llamacppBackend', 'llamacppArgs', 'saveOptions'],
  'whispercpp': ['whispercppBackend', 'saveOptions'],
  'flm': ['ctxSize', 'saveOptions'],
  'oga-cpu': ['ctxSize', 'saveOptions'],
  'oga-npu': ['ctxSize', 'saveOptions'],
  'oga-hybrid': ['ctxSize', 'saveOptions'],
  'sd-cpp': ['steps', 'cfgScale', 'width', 'height', 'saveOptions'],
};

/**
 * Gets the option keys for a given recipe
 */
export function getOptionsForRecipe(recipe: string): string[] {
  if (recipe in RECIPE_OPTIONS_MAP) {
    return RECIPE_OPTIONS_MAP[recipe as RecipeName];
  }
  return [];
}

/**
 * Gets the option definition for a given option key
 */
export function getOptionDefinition(key: string): OptionDef | undefined {
  return OPTION_DEFINITIONS[key];
}

// =============================================================================
// API Field Mapping - Maps between frontend camelCase and API snake_case
// =============================================================================

const FRONTEND_TO_API_MAP: Record<string, string> = {
  ctxSize: 'ctx_size',
  llamacppBackend: 'llamacpp_backend',
  llamacppArgs: 'llamacpp_args',
  whispercppBackend: 'whispercpp_backend',
  cfgScale: 'cfg_scale',
  saveOptions: 'save_options',
};

const API_TO_FRONTEND_MAP: Record<string, string> = Object.fromEntries(
  Object.entries(FRONTEND_TO_API_MAP).map(([k, v]) => [v, k])
);

export function toApiOptionName(frontendName: string): string {
  return FRONTEND_TO_API_MAP[frontendName] ?? frontendName;
}

export function toFrontendOptionName(apiName: string): string {
  return API_TO_FRONTEND_MAP[apiName] ?? apiName;
}

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * Clamps a numeric value to the defined limits for an option
 */
export function clampOptionValue(key: string, value: number): number {
  const def = OPTION_DEFINITIONS[key];
  if (!def || def.type !== 'numeric') {
    return value;
  }

  if (!Number.isFinite(value)) {
    return def.default;
  }

  return Math.min(Math.max(value, def.min), def.max);
}

/**
 * Creates default options for a recipe
 */
export function createDefaultOptions(recipe: string): RecipeOptions {
  const optionKeys = getOptionsForRecipe(recipe);
  const result: Record<string, unknown> = { recipe };

  for (const key of optionKeys) {
    const def = OPTION_DEFINITIONS[key];
    if (def) {
      result[key] = { value: def.default, useDefault: true };
    }
  }

  return result as unknown as RecipeOptions;
}

/**
 * Converts API response options to RecipeOptions format
 */
export function apiToRecipeOptions(recipe: string, apiOptions?: Record<string, unknown>): RecipeOptions {
  const optionKeys = getOptionsForRecipe(recipe);
  const result: Record<string, unknown> = { recipe };

  for (const key of optionKeys) {
    const def = OPTION_DEFINITIONS[key];
    if (!def) continue;

    const apiKey = toApiOptionName(key);
    const apiValue = apiOptions?.[apiKey];
    const value = apiValue !== undefined ? apiValue : def.default;

    result[key] = { value, useDefault: true };
  }

  return result as unknown as RecipeOptions;
}

/**
 * Converts RecipeOptions to API format for the /load endpoint
 */
export function recipeOptionsToApi(options: RecipeOptions): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  const optionsRecord = options as unknown as Record<string, { value: unknown; useDefault: boolean }>;

  for (const [key, option] of Object.entries(optionsRecord)) {
    if (key === 'recipe') continue;
    if (option && typeof option === 'object' && 'value' in option) {
      const apiKey = toApiOptionName(key);
      result[apiKey] = option.value;
    }
  }

  return result;
}
