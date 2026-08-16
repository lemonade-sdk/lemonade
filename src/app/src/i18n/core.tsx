import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react';
import { storageKey } from '../storage';
import {
  GENERATED_LOCALES,
  GENERATED_SOURCE_LOCALE,
} from './generated/locales.generated';
import type {
  GeneratedLocaleDefinition,
  LocaleDirection,
  LocaleManifest,
  TranslationParams,
} from './types';

const LOCALE_STORAGE_KEY = storageKey('locale');
const missingTranslationWarnings = new Set<string>();

const locales: GeneratedLocaleDefinition[] = GENERATED_LOCALES.map(entry => ({
  manifest: entry.manifest as LocaleManifest,
  resources: entry.resources,
}));

const localeByCanonicalName = new Map(
  locales.map(locale => [canonicalizeLocale(locale.manifest.locale).toLowerCase(), locale]),
);

const resolvedSourceLocale = localeByCanonicalName.get(
  canonicalizeLocale(GENERATED_SOURCE_LOCALE).toLowerCase(),
) ?? locales.find(locale => locale.manifest.source);

if (!resolvedSourceLocale) {
  throw new Error('No source locale is registered. Run npm run i18n:sync and ensure exactly one manifest has source=true.');
}

// Keep a non-optional binding for closures below. TypeScript does not retain
// module-scope control-flow narrowing for a captured `T | undefined` value.
const sourceLocale: GeneratedLocaleDefinition = resolvedSourceLocale;

export interface LocaleOption {
  locale: string;
  direction: LocaleDirection;
  source: boolean;
}

export interface I18nContextValue {
  locale: string;
  direction: LocaleDirection;
  preference: string | null;
  sourceLocale: string;
  availableLocales: readonly LocaleOption[];
  setLocalePreference: (locale: string | null) => void;
  translate: (namespace: string, key: string, params?: TranslationParams) => string;
  translateOptional: (namespace: string, key: string, params?: TranslationParams) => string | null;
  formatNumber: (value: number, options?: Intl.NumberFormatOptions) => string;
  formatDate: (value: Date | number | string, options?: Intl.DateTimeFormatOptions) => string;
  formatList: (values: readonly string[], options?: Intl.ListFormatOptions) => string;
  formatRelativeTime: (value: number, unit: Intl.RelativeTimeFormatUnit, options?: Intl.RelativeTimeFormatOptions) => string;
  displayNameForLocale: (locale: string, displayLocale?: string) => string;
}

const I18nContext = createContext<I18nContextValue | null>(null);

function canonicalizeLocale(locale: string): string {
  try {
    return Intl.getCanonicalLocales(locale)[0] || locale;
  } catch {
    return locale;
  }
}

function safeLocale(locale: string): Intl.Locale | null {
  try {
    return new Intl.Locale(locale);
  } catch {
    return null;
  }
}

function localeCompatibilityScore(requested: string, candidate: string): number {
  const requestedCanonical = canonicalizeLocale(requested);
  const candidateCanonical = canonicalizeLocale(candidate);
  if (requestedCanonical.toLowerCase() === candidateCanonical.toLowerCase()) return 10_000;

  const requestedLocale = safeLocale(requestedCanonical);
  const candidateLocale = safeLocale(candidateCanonical);
  if (!requestedLocale || !candidateLocale) return -1;

  let requestedMax: Intl.Locale;
  let candidateMax: Intl.Locale;
  try {
    requestedMax = requestedLocale.maximize();
    candidateMax = candidateLocale.maximize();
  } catch {
    requestedMax = requestedLocale;
    candidateMax = candidateLocale;
  }

  if (requestedMax.language !== candidateMax.language) return -1;

  // Script is the meaningful distinction for Chinese and several other locales.
  // Do not silently map Traditional Chinese to Simplified Chinese or vice versa.
  if (requestedMax.script && candidateMax.script && requestedMax.script !== candidateMax.script) return -1;

  let score = 100;
  if (requestedLocale.language === candidateLocale.language) score += 20;
  if (requestedMax.script && requestedMax.script === candidateMax.script) score += 30;
  if (requestedLocale.region && requestedLocale.region === candidateLocale.region) score += 10;
  if (requestedMax.region && requestedMax.region === candidateMax.region) score += 5;
  return score;
}

export function resolveSupportedLocale(requestedLocales: readonly string[]): string {
  // navigator.languages is preference ordered. The first requested locale that
  // can be matched wins; only candidates for that request are scored against
  // each other. This prevents an exact second-choice English locale from
  // overriding a compatible first-choice German locale such as de-AT -> de-DE.
  for (const requested of requestedLocales) {
    let best: { locale: string; score: number } | null = null;
    for (const candidate of locales) {
      const score = localeCompatibilityScore(requested, candidate.manifest.locale);
      if (score < 0) continue;
      if (!best || score > best.score) best = { locale: candidate.manifest.locale, score };
    }
    if (best) return best.locale;
  }

  return sourceLocale.manifest.locale;
}

function systemLocales(): string[] {
  if (typeof navigator === 'undefined') return [];
  const requested = [...(navigator.languages || [])];
  if (navigator.language && !requested.includes(navigator.language)) requested.push(navigator.language);
  return requested;
}

function readLocalePreference(): string | null {
  if (typeof localStorage === 'undefined') return null;
  try {
    const stored = localStorage.getItem(LOCALE_STORAGE_KEY);
    if (!stored) return null;
    const canonical = canonicalizeLocale(stored);
    return localeByCanonicalName.has(canonical.toLowerCase()) ? canonical : null;
  } catch {
    return null;
  }
}

function writeLocalePreference(locale: string | null): void {
  if (typeof localStorage === 'undefined') return;
  try {
    if (locale) localStorage.setItem(LOCALE_STORAGE_KEY, locale);
    else localStorage.removeItem(LOCALE_STORAGE_KEY);
  } catch {
    // Browser storage is best-effort. The active locale still changes in memory.
  }
}

function lookupPath(root: unknown, key: string): unknown {
  if (!root || typeof root !== 'object') return undefined;
  return key.split('.').reduce<unknown>((current, segment) => {
    if (!current || typeof current !== 'object' || Array.isArray(current)) return undefined;
    return (current as Record<string, unknown>)[segment];
  }, root);
}

const pluralCategories = new Set(['zero', 'one', 'two', 'few', 'many', 'other']);

function isPluralValue(value: unknown): value is Record<string, string> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const entries = Object.entries(value as Record<string, unknown>);
  return entries.length > 0
    && Object.prototype.hasOwnProperty.call(value, 'other')
    && entries.every(([key, child]) => pluralCategories.has(key) && typeof child === 'string');
}

function resolvePlural(value: unknown, locale: string, params?: TranslationParams): unknown {
  if (!isPluralValue(value)) return value;
  const count = params?.count;
  if (typeof count !== 'number') return value.other;
  const category = new Intl.PluralRules(locale).select(count);
  return value[category] ?? value.other;
}

function interpolate(template: string, locale: string, params?: TranslationParams): string {
  if (!params) return template;
  return template.replace(/{{\s*([\w.-]+)\s*}}/g, (_match, name: string) => {
    const value = params[name];
    if (value === null || value === undefined) return '';
    if (typeof value === 'number') return new Intl.NumberFormat(locale).format(value);
    return String(value);
  });
}

function translationValue(locale: GeneratedLocaleDefinition, namespace: string, key: string, params?: TranslationParams): string | null {
  const namespaceResource = locale.resources[namespace];
  if (!namespaceResource) return null;
  const value = resolvePlural(lookupPath(namespaceResource, key), locale.manifest.locale, params);
  return typeof value === 'string' ? interpolate(value, locale.manifest.locale, params) : null;
}

export function translateOptionalForLocale(locale: string, namespace: string, key: string, params?: TranslationParams): string | null {
  const active = localeByCanonicalName.get(canonicalizeLocale(locale).toLowerCase()) ?? sourceLocale;
  return translationValue(active, namespace, key, params)
    ?? translationValue(sourceLocale, namespace, key, params);
}

export function translateForLocale(locale: string, namespace: string, key: string, params?: TranslationParams): string {
  const translated = translateOptionalForLocale(locale, namespace, key, params);
  if (translated !== null) return translated;

  const warningKey = `${namespace}:${key}`;
  if (!missingTranslationWarnings.has(warningKey)) {
    missingTranslationWarnings.add(warningKey);
    console.warn(`[i18n] Missing source translation: ${warningKey}`);
  }
  return `[${warningKey}]`;
}

function displayNameForLocale(locale: string, displayLocale = locale): string {
  const canonical = canonicalizeLocale(locale);
  try {
    const displayNames = new Intl.DisplayNames([displayLocale], { type: 'language' });
    return displayNames.of(canonical) || canonical;
  } catch {
    return canonical;
  }
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const [preference, setPreference] = useState<string | null>(() => readLocalePreference());
  const [systemLocaleRevision, setSystemLocaleRevision] = useState(0);

  const locale = useMemo(
    () => preference ?? resolveSupportedLocale(systemLocales()),
    [preference, systemLocaleRevision],
  );

  const activeDefinition = localeByCanonicalName.get(canonicalizeLocale(locale).toLowerCase()) ?? sourceLocale;

  useEffect(() => {
    document.documentElement.lang = activeDefinition.manifest.locale;
    document.documentElement.dir = activeDefinition.manifest.direction;
    document.documentElement.dataset.locale = activeDefinition.manifest.locale;
  }, [activeDefinition.manifest.direction, activeDefinition.manifest.locale]);

  useEffect(() => {
    if (preference !== null || typeof window === 'undefined') return undefined;
    const handleLanguageChange = () => setSystemLocaleRevision(revision => revision + 1);
    window.addEventListener('languagechange', handleLanguageChange);
    return () => window.removeEventListener('languagechange', handleLanguageChange);
  }, [preference]);

  const setLocalePreference = useCallback((nextLocale: string | null) => {
    if (nextLocale === null) {
      writeLocalePreference(null);
      setPreference(null);
      setSystemLocaleRevision(revision => revision + 1);
      return;
    }
    const canonical = canonicalizeLocale(nextLocale);
    if (!localeByCanonicalName.has(canonical.toLowerCase())) {
      console.warn(`[i18n] Ignoring unsupported locale preference: ${nextLocale}`);
      return;
    }
    writeLocalePreference(canonical);
    setPreference(canonical);
  }, []);

  const translate = useCallback(
    (namespace: string, key: string, params?: TranslationParams) => translateForLocale(locale, namespace, key, params),
    [locale],
  );
  const translateOptional = useCallback(
    (namespace: string, key: string, params?: TranslationParams) => translateOptionalForLocale(locale, namespace, key, params),
    [locale],
  );

  const value = useMemo<I18nContextValue>(() => ({
    locale,
    direction: activeDefinition.manifest.direction,
    preference,
    sourceLocale: sourceLocale.manifest.locale,
    availableLocales: locales.map(item => ({
      locale: item.manifest.locale,
      direction: item.manifest.direction,
      source: item.manifest.source,
    })),
    setLocalePreference,
    translate,
    translateOptional,
    formatNumber: (number, options) => new Intl.NumberFormat(locale, options).format(number),
    formatDate: (input, options) => new Intl.DateTimeFormat(locale, options).format(
      input instanceof Date ? input : new Date(input),
    ),
    formatList: (items, options) => new Intl.ListFormat(locale, options).format([...items]),
    formatRelativeTime: (number, unit, options) => new Intl.RelativeTimeFormat(locale, options).format(number, unit),
    displayNameForLocale,
  }), [activeDefinition.manifest.direction, locale, preference, setLocalePreference, translate, translateOptional]);

  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>;
}

export function useI18n(namespace?: string) {
  const context = useContext(I18nContext);
  if (!context) throw new Error('useI18n must be used inside I18nProvider.');

  const t = useCallback(
    (key: string, params?: TranslationParams) => context.translate(namespace ?? 'common', key, params),
    [context, namespace],
  );
  const tOptional = useCallback(
    (key: string, params?: TranslationParams) => context.translateOptional(namespace ?? 'common', key, params),
    [context, namespace],
  );

  return { ...context, t, tOptional };
}
