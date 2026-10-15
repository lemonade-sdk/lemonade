/**
 * Recipe Options - Re-exports from central config
 *
 * This file exists for backward compatibility.
 * All types and utilities are defined in recipeOptionsConfig.ts
 */

export {
  // Base option types
  NumericOption,
  StringOption,
  BooleanOption,

  // Recipe-specific interfaces
  LlamaOptions,
  WhisperOptions,
  MoonshineOptions,
  FlmOptions,
  RyzenAIOptions,
  RyzenAIRecipe,
  StableDiffusionOptions,
  VLLMOptions,

  // Union type
  RecipeOptions,
  LegacyRecipeOptions,
  LegacyRecipeName,
  isLegacyRecipe,

  // Constants
  RYZENAI_RECIPES,

  // Utilities
  isRyzenAIRecipe,
  getOptionsForRecipe,
  getOptionDefinition,
  clampOptionValue,
  createDefaultOptions,
  apiToRecipeOptions,
  recipeOptionsToApi,
  toApiOptionName,
  toFrontendOptionName,

  // Config
  OPTION_DEFINITIONS,
  RECIPE_OPTIONS_MAP,
} from './recipeOptionsConfig';

import type { LegacyRecipeOptions } from './recipeOptionsConfig';
import type { CatalogLoadContext } from '../utils/backendInstaller';
import type { Scalar } from './backendSchema';

export type RecipeSubmission =
  | { kind: 'legacy'; options: LegacyRecipeOptions }
  | { kind: 'catalog'; options: Record<string, Scalar>; context: CatalogLoadContext };
