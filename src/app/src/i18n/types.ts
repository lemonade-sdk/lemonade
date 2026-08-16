export type LocaleDirection = 'ltr' | 'rtl';

export type TranslationPrimitive = string | number | boolean;
export type TranslationParams = Record<string, TranslationPrimitive | null | undefined>;

export interface LocaleManifest {
  locale: string;
  source: boolean;
  direction: LocaleDirection;
  complete?: boolean;
}

export type TranslationResource = Record<string, unknown>;

export interface GeneratedLocaleDefinition {
  manifest: LocaleManifest;
  resources: Record<string, TranslationResource>;
}
