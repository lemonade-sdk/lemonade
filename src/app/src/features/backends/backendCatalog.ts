/**
 * Typed view of lemond's `/v1/system-info` `recipes` map.
 *
 * lemond derives that map from the C++ backend descriptors precisely so clients
 * do not hardcode per-backend knowledge (see the enrichment comment in
 * src/cpp/server/system_info.cpp). Every field here comes from the wire; this
 * module never enumerates recipe names. A recipe the client has never heard of
 * must parse into a complete descriptor — the catalog falls back, it never
 * filters.
 */

export interface RecipeOptionSchema {
  name: string;
  cliFlag: string;
  defaultValue: unknown;
  typeName: string;
  help: string;
  group: string;
}

export interface SupportRow {
  backend: string;
  os: string[];
  devices: Array<{ device: string; families: string[] }>;
  deviceSummary: string;
}

export interface BackendVariant {
  backend: string;
  state: string;
  version: string;
  message: string;
  action: string;
  /** Detected on this machine, falling back to the backend's declared devices. */
  devices: string[];
  releaseUrl: string;
  downloadFilename: string;
  canUninstall: boolean;
}

export interface RecipeDescriptor {
  recipe: string;
  order: number;
  displayName: string;
  webDisplayName: string;
  modality: string;
  experimental: boolean;
  slotPolicy: string;
  selectableBackend: boolean;
  usesCtxSize: boolean;
  defaultBackend: string;
  backends: BackendVariant[];
  support: SupportRow[];
  options: RecipeOptionSchema[];
  /** The `<recipe>_backend` option, whose name is irregular for sd-cpp. */
  backendOptionName: string | null;
  argsOptionName: string | null;
  deviceOptionName: string | null;
  devices: string[];
}

export interface BackendCatalog {
  recipes: RecipeDescriptor[];
  byRecipe: Map<string, RecipeDescriptor>;
  /** Untouched payload, so consumers of `devices`/`cloud`/storage need no new shape. */
  raw: Record<string, unknown>;
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function asArray(value: unknown): unknown[] {
  return Array.isArray(value) ? value : [];
}

function asString(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function asStringArray(value: unknown): string[] {
  return asArray(value).filter((entry): entry is string => typeof entry === 'string');
}

function parseSupport(value: unknown): SupportRow[] {
  return asArray(value).map(entry => {
    const row = asRecord(entry);
    return {
      backend: asString(row.backend),
      os: asStringArray(row.os),
      devices: asArray(row.devices).map(deviceEntry => {
        const device = asRecord(deviceEntry);
        return { device: asString(device.device), families: asStringArray(device.families) };
      }),
      deviceSummary: asString(row.device_summary),
    };
  });
}

function parseOptions(value: unknown): RecipeOptionSchema[] {
  return asArray(value).map(entry => {
    const option = asRecord(entry);
    return {
      name: asString(option.name),
      cliFlag: asString(option.cli_flag),
      defaultValue: option.default,
      typeName: asString(option.type_name),
      help: asString(option.help),
      group: asString(option.group),
    };
  }).filter(option => option.name !== '');
}

function declaredDevices(support: SupportRow[], backend: string): string[] {
  const row = support.find(entry => entry.backend === backend);
  return row ? row.devices.map(device => device.device).filter(Boolean) : [];
}

function parseBackends(value: unknown, support: SupportRow[]): BackendVariant[] {
  return Object.entries(asRecord(value)).map(([backend, rawInfo]) => {
    const info = asRecord(rawInfo);
    const detected = asStringArray(info.devices).filter(Boolean);
    return {
      backend,
      state: asString(info.state) || 'unsupported',
      version: asString(info.version),
      message: asString(info.message),
      action: asString(info.action),
      devices: detected.length ? detected : declaredDevices(support, backend),
      releaseUrl: asString(info.release_url),
      downloadFilename: asString(info.download_filename),
      canUninstall: info.can_uninstall !== false,
    };
  });
}

function optionNamed(options: RecipeOptionSchema[], typeName: string): string | null {
  return options.find(option => option.typeName === typeName)?.name ?? null;
}

function parseRecipe(recipe: string, value: unknown, fallbackOrder: number): RecipeDescriptor {
  const info = asRecord(value);
  const support = parseSupport(info.support);
  const options = parseOptions(info.options);
  const backends = parseBackends(info.backends, support);
  const displayName = asString(info.display_name);

  const devices: string[] = [];
  for (const variant of backends) {
    for (const device of variant.devices) if (!devices.includes(device)) devices.push(device);
  }

  return {
    recipe,
    order: typeof info.order === 'number' ? info.order : fallbackOrder,
    displayName,
    webDisplayName: asString(info.web_display_name) || displayName,
    modality: asString(info.modality),
    experimental: info.experimental === true,
    slotPolicy: asString(info.slot_policy) || 'standard',
    selectableBackend: info.selectable_backend === true,
    usesCtxSize: info.uses_ctx_size === true,
    defaultBackend: asString(info.default_backend),
    backends,
    support,
    options,
    backendOptionName: optionNamed(options, 'BACKEND'),
    argsOptionName: options.find(option => option.name.endsWith('_args'))?.name ?? null,
    deviceOptionName: optionNamed(options, 'DEVICES'),
    devices,
  };
}

export function parseBackendCatalog(raw: unknown): BackendCatalog {
  const payload = asRecord(raw);
  const recipes = Object.entries(asRecord(payload.recipes))
    .map(([recipe, value], index) => parseRecipe(recipe, value, index))
    .sort((a, b) => a.order - b.order || a.recipe.localeCompare(b.recipe));

  return {
    recipes,
    byRecipe: new Map(recipes.map(descriptor => [descriptor.recipe, descriptor])),
    raw: payload,
  };
}
