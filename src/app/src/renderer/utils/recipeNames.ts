/**
 * Utilitários para nomes de receitas de modelos.
 *
 * Contém constantes para as receitas de coleções (omni e router), funções para
 * verificar se uma receita é do tipo coleção, o mapeamento inicial de nomes de
 * exibição de receitas (RECIPE_DISPLAY_NAMES) e a função updateRecipeDisplayNames
 * para mesclar nomes de exibição retornados pela API /system-info (gerados a
 * partir dos descritores C++).
 */

export const COLLECTION_OMNI_MODEL_RECIPE = 'collection.omni';
export const COLLECTION_ROUTER_MODEL_RECIPE = 'collection.router';

export const isCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE;
};

export const isModelCollectionRecipe = (recipe?: string): boolean => {
  return recipe === COLLECTION_OMNI_MODEL_RECIPE || recipe === COLLECTION_ROUTER_MODEL_RECIPE;
};

export const RECIPE_DISPLAY_NAMES: Record<string, string> = {
  [COLLECTION_OMNI_MODEL_RECIPE]: 'Lemonade',
  [COLLECTION_ROUTER_MODEL_RECIPE]: 'Router',
  'cloud': 'Cloud',
};

export const updateRecipeDisplayNames = (
  recipes?: Record<string, { display_name?: string }>
): void => {
  if (!recipes) {
    return;
  }
  for (const [recipe, info] of Object.entries(recipes)) {
    if (info && typeof info.display_name === 'string' && info.display_name) {
      RECIPE_DISPLAY_NAMES[recipe] = info.display_name;
    }
  }
};
