export type OptionalTranslation = (key: string) => string | null;

/**
 * Convert remote marketplace identifiers into safe translation-key segments.
 * This is deliberately language-agnostic: locale resources opt into overrides
 * without requiring a switch statement or a registry entry in application code.
 */
export function marketplaceTranslationSegment(value: string): string {
  const normalized = value.normalize('NFKC').trim().toLowerCase();
  const segment = normalized
    .replace(/[^\p{L}\p{N}]+/gu, '_')
    .replace(/^_+|_+$/g, '');
  return segment || 'unknown';
}

export function marketplaceAppDescriptionKey(appIdOrName: string): string {
  return `marketplace.apps.${marketplaceTranslationSegment(appIdOrName)}.description`;
}

export function marketplaceCategoryLabelKey(categoryId: string): string {
  return `marketplace.categories.${marketplaceTranslationSegment(categoryId)}`;
}

export function localizedMarketplaceDescription(
  app: { id?: string; name: string; description?: string },
  translateOptional: OptionalTranslation,
): string | undefined {
  return translateOptional(marketplaceAppDescriptionKey(app.id || app.name)) ?? app.description;
}

export function localizedMarketplaceCategory(
  categoryId: string,
  fallbackLabel: string,
  translateOptional: OptionalTranslation,
): string {
  return translateOptional(marketplaceCategoryLabelKey(categoryId)) ?? fallbackLabel;
}
