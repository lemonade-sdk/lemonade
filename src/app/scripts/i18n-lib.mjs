import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const currentDir = path.dirname(fileURLToPath(import.meta.url));
export const projectRoot = path.resolve(currentDir, '..');
export const localesRoot = path.join(projectRoot, 'src', 'i18n', 'locales');
export const generatedFile = path.join(projectRoot, 'src', 'i18n', 'generated', 'locales.generated.ts');

export function canonicalizeLocale(locale) {
  try {
    const [canonical] = Intl.getCanonicalLocales(locale);
    if (!canonical) throw new Error('empty locale');
    return canonical;
  } catch {
    throw new Error(`Invalid BCP-47 locale: ${locale}`);
  }
}

export function directionForLocale(locale) {
  let maximized;
  try {
    maximized = new Intl.Locale(locale).maximize();
  } catch {
    return 'ltr';
  }
  const rtlScripts = new Set(['Adlm', 'Arab', 'Hebr', 'Mand', 'Nkoo', 'Rohg', 'Samr', 'Syrc', 'Thaa']);
  return maximized.script && rtlScripts.has(maximized.script) ? 'rtl' : 'ltr';
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    throw new Error(`Cannot parse ${path.relative(projectRoot, file)}: ${error.message}`);
  }
}

function collectLeafKeys(value, prefix = '', out = new Set()) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    if (prefix) out.add(prefix);
    return out;
  }
  const keys = Object.keys(value);
  if (isPluralObject(value)) {
    if (prefix) out.add(prefix);
    return out;
  }
  for (const key of keys) collectLeafKeys(value[key], prefix ? `${prefix}.${key}` : key, out);
  return out;
}


function valueAtPath(root, key) {
  let value = root;
  for (const part of key.split('.')) {
    if (!value || typeof value !== 'object' || !Object.prototype.hasOwnProperty.call(value, part)) return undefined;
    value = value[part];
  }
  return value;
}

function interpolationNames(value, out = new Set()) {
  if (typeof value === 'string') {
    for (const match of value.matchAll(/\{\{\s*([A-Za-z0-9_.-]+)\s*\}\}/g)) out.add(match[1]);
    return out;
  }
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    for (const child of Object.values(value)) interpolationNames(child, out);
  }
  return out;
}

const pluralCategories = new Set(['zero', 'one', 'two', 'few', 'many', 'other']);

function isPluralObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const entries = Object.entries(value);
  return entries.length > 0
    && Object.prototype.hasOwnProperty.call(value, 'other')
    && entries.every(([key, child]) => pluralCategories.has(key) && typeof child === 'string');
}

function sameStringSet(left, right) {
  if (left.size !== right.size) return false;
  return [...left].every(value => right.has(value));
}

function validateTranslationValue(sourceValue, localeValue, location) {
  // A null source value deliberately reserves an optional localized override.
  // The active locale may keep it null (so the UI falls back to dynamic remote
  // content) or provide a concrete localized string. These override slots are
  // consumed without interpolation parameters, so introducing placeholders
  // here would silently render them empty at runtime.
  if (sourceValue === null) {
    if (localeValue === null) return;
    if (typeof localeValue !== 'string' || localeValue.length === 0) {
      throw new Error(`${location} must be null or a translated string because the source key is an optional localized override.`);
    }
    const localeParams = interpolationNames(localeValue);
    if (localeParams.size > 0) {
      throw new Error(`${location} optional localized override must not introduce interpolation placeholders: [${[...localeParams].sort().join(', ')}].`);
    }
    return;
  }

  if (typeof sourceValue === 'string') {
    if (typeof localeValue !== 'string' || localeValue.length === 0) {
      throw new Error(`${location} must be a translated string.`);
    }
  } else if (isPluralObject(sourceValue)) {
    if (!isPluralObject(localeValue)) {
      throw new Error(`${location} must define plural forms using only CLDR plural categories and include 'other'.`);
    }
  } else {
    throw new Error(`Source translation ${location} must be a string, null optional override, or a valid plural object.`);
  }

  const sourceParams = interpolationNames(sourceValue);
  if (isPluralObject(localeValue)) {
    for (const [form, formValue] of Object.entries(localeValue)) {
      const formParams = interpolationNames(formValue);
      if (!sameStringSet(sourceParams, formParams)) {
        throw new Error(`${location}.${form} interpolation mismatch: expected [${[...sourceParams].sort().join(', ')}], found [${[...formParams].sort().join(', ')}].`);
      }
    }
  } else {
    const localeParams = interpolationNames(localeValue);
    if (!sameStringSet(sourceParams, localeParams)) {
      throw new Error(`${location} interpolation mismatch: expected [${[...sourceParams].sort().join(', ')}], found [${[...localeParams].sort().join(', ')}].`);
    }
  }
}

export function loadLocaleDefinitions(root = localesRoot) {
  if (!fs.existsSync(root)) throw new Error(`Locales directory not found: ${root}`);
  const folders = fs.readdirSync(root, { withFileTypes: true })
    .filter(entry => entry.isDirectory())
    .map(entry => entry.name)
    .sort((a, b) => a.localeCompare(b));

  if (folders.length === 0) throw new Error('No locales are registered.');

  const definitions = folders.map(folder => {
    const localeDir = path.join(root, folder);
    const manifestFile = path.join(localeDir, 'manifest.json');
    if (!fs.existsSync(manifestFile)) throw new Error(`${folder} is missing manifest.json`);
    const manifest = readJson(manifestFile);
    const canonical = canonicalizeLocale(manifest.locale);
    if (folder !== canonical) {
      throw new Error(`Locale directory '${folder}' must match canonical manifest locale '${canonical}'.`);
    }
    if (typeof manifest.source !== 'boolean') throw new Error(`${folder}/manifest.json must define boolean source.`);
    if (!['ltr', 'rtl'].includes(manifest.direction)) throw new Error(`${folder}/manifest.json must define direction as ltr or rtl.`);
    if (manifest.complete !== undefined && typeof manifest.complete !== 'boolean') {
      throw new Error(`${folder}/manifest.json complete must be boolean when provided.`);
    }

    const resourceFiles = fs.readdirSync(localeDir)
      .filter(name => name.endsWith('.json') && name !== 'manifest.json')
      .sort((a, b) => a.localeCompare(b));
    const resources = Object.fromEntries(resourceFiles.map(file => [path.basename(file, '.json'), readJson(path.join(localeDir, file))]));
    return { folder, localeDir, manifest, resources, resourceFiles };
  });

  const sourceLocales = definitions.filter(definition => definition.manifest.source);
  if (sourceLocales.length !== 1) {
    throw new Error(`Exactly one locale must have source=true; found ${sourceLocales.length}.`);
  }
  const source = sourceLocales[0];
  const sourceNamespaces = new Set(Object.keys(source.resources));
  if (sourceNamespaces.size === 0) throw new Error(`Source locale ${source.folder} has no translation namespaces.`);

  const sourceKeys = Object.fromEntries(
    Object.entries(source.resources).map(([namespace, resource]) => [namespace, collectLeafKeys(resource)]),
  );

  // Validate the source contract itself. Leaves may be non-empty strings,
  // strict plural objects, or null optional-override slots for dynamic remote
  // content. Running the same compatibility check against the source also
  // catches placeholder drift between plural forms.
  for (const namespace of sourceNamespaces) {
    for (const key of sourceKeys[namespace]) {
      const sourceValue = valueAtPath(source.resources[namespace], key);
      validateTranslationValue(sourceValue, sourceValue, `${source.folder}/${namespace}.json ${key}`);
    }
  }

  for (const definition of definitions) {
    for (const namespace of Object.keys(definition.resources)) {
      if (!sourceNamespaces.has(namespace)) {
        throw new Error(`${definition.folder}/${namespace}.json has no matching source namespace.`);
      }
      const unknownKeys = [...collectLeafKeys(definition.resources[namespace])]
        .filter(key => !sourceKeys[namespace].has(key));
      if (unknownKeys.length > 0) {
        throw new Error(`${definition.folder}/${namespace}.json contains unknown keys: ${unknownKeys.slice(0, 8).join(', ')}`);
      }

      // Partial locales may omit source keys, but every key they do provide must
      // obey the same value shape and interpolation contract as the source.
      for (const key of sourceKeys[namespace]) {
        const localeValue = valueAtPath(definition.resources[namespace], key);
        if (localeValue === undefined) continue;
        const sourceValue = valueAtPath(source.resources[namespace], key);
        validateTranslationValue(sourceValue, localeValue, `${definition.folder}/${namespace}.json ${key}`);
      }
    }
  }

  for (const definition of definitions.filter(item => item.manifest.complete)) {
    for (const namespace of sourceNamespaces) {
      if (!definition.resources[namespace]) {
        throw new Error(`${definition.folder} is marked complete but is missing ${namespace}.json.`);
      }
      const localeKeys = collectLeafKeys(definition.resources[namespace]);
      const missingKeys = [...sourceKeys[namespace]].filter(key => !localeKeys.has(key));
      if (missingKeys.length > 0) {
        throw new Error(`${definition.folder}/${namespace}.json is marked complete but is missing keys: ${missingKeys.slice(0, 8).join(', ')}`);
      }
    }
  }

  return { definitions, source, sourceNamespaces: [...sourceNamespaces].sort() };
}

function identifierPart(value) {
  return value.replace(/[^a-zA-Z0-9]+(.)?/g, (_, next = '') => next.toUpperCase()).replace(/^[^a-zA-Z_]/, '_$&');
}

export function generateRegistry(definitions, source) {
  const lines = [
    '/* This file is generated by scripts/i18n-sync.mjs. Do not edit manually. */',
    "import type { GeneratedLocaleDefinition } from '../types';",
    '',
  ];

  const entries = [];
  for (const definition of definitions) {
    const localeId = identifierPart(definition.folder);
    const manifestId = `${localeId}Manifest`;
    lines.push(`import ${manifestId} from '../locales/${definition.folder}/manifest.json';`);
    const resources = [];
    for (const file of definition.resourceFiles) {
      const namespace = path.basename(file, '.json');
      const resourceId = `${localeId}${identifierPart(namespace[0].toUpperCase() + namespace.slice(1))}`;
      lines.push(`import ${resourceId} from '../locales/${definition.folder}/${file}';`);
      resources.push({ namespace, resourceId });
    }
    entries.push({ manifestId, resources });
  }

  lines.push('', 'export const GENERATED_LOCALES = [');
  for (const entry of entries) {
    lines.push('  {');
    lines.push(`    manifest: ${entry.manifestId},`);
    lines.push('    resources: {');
    for (const { namespace, resourceId } of entry.resources) lines.push(`      ${JSON.stringify(namespace)}: ${resourceId},`);
    lines.push('    },');
    lines.push('  },');
  }
  lines.push('] as unknown as readonly GeneratedLocaleDefinition[];', '');
  lines.push(`export const GENERATED_SOURCE_LOCALE = ${JSON.stringify(source.manifest.locale)};`, '');
  return lines.join('\n');
}

export function writeRegistry() {
  const { definitions, source } = loadLocaleDefinitions();
  const content = generateRegistry(definitions, source);
  fs.mkdirSync(path.dirname(generatedFile), { recursive: true });
  fs.writeFileSync(generatedFile, content, 'utf8');
  return { definitions, source, content };
}
