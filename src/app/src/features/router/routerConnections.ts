import type { CloudProviderRow, ModelInfo } from '../../api';

export type RouterConnectionKind = 'internal' | 'external' | 'unresolved';

export interface RouterModelConnection {
  modelName: string;
  displayName: string;
  kind: RouterConnectionKind;
  recipe: string;
  backend: string;
  provider: string;
  endpoint: string;
  providerConfigured: boolean;
  authConfigured: boolean;
  allowInsecureHttp: boolean;
  modelFound: boolean;
}

function text(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

export function routerModelName(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  return text((model as any).model_name) || text(model.name) || text(model.id);
}

function providerKey(value: string): string {
  return value.trim().toLowerCase();
}

function providerFromModel(model: ModelInfo | undefined, modelName: string): string {
  if (model) {
    for (const key of ['provider', 'cloud_provider', 'provider_name', 'source_provider']) {
      const candidate = text((model as any)[key]);
      if (candidate) return candidate;
    }
  }
  const dot = modelName.indexOf('.');
  return dot > 0 ? modelName.slice(0, dot) : '';
}

function endpointFromModel(model: ModelInfo | undefined): string {
  if (!model) return '';
  for (const key of ['base_url', 'baseUrl', 'endpoint', 'endpoint_url', 'server_url', 'api_base']) {
    const candidate = text((model as any)[key]);
    if (candidate) return candidate;
  }
  return '';
}

function recipeFromModel(model: ModelInfo | undefined): string {
  if (!model) return '';
  const direct = text((model as any).recipe);
  if (direct) return direct;
  const recipes = Array.isArray(model.recipes) ? model.recipes : [];
  const first = recipes[0] as Record<string, unknown> | undefined;
  return text(first?.recipe) || text(first?.name) || text(first?.id);
}

function backendFromModel(model: ModelInfo | undefined): string {
  if (!model) return '';
  return text((model as any).backend)
    || text((model as any).backend_name)
    || recipeFromModel(model);
}

export function isCloudModel(model: ModelInfo | undefined, provider?: CloudProviderRow): boolean {
  const recipe = recipeFromModel(model).toLowerCase();
  const backend = backendFromModel(model).toLowerCase();
  const type = text((model as any)?.type).toLowerCase();
  return Boolean(provider)
    || recipe === 'cloud'
    || recipe.startsWith('cloud.')
    || backend === 'cloud'
    || backend.startsWith('cloud.')
    || type === 'cloud';
}

export function describeRouterModelConnection(
  modelName: string,
  models: ModelInfo[],
  providers: CloudProviderRow[],
): RouterModelConnection {
  const normalizedName = modelName.trim();
  const model = models.find(item => routerModelName(item).toLowerCase() === normalizedName.toLowerCase());
  const providerName = providerFromModel(model, normalizedName);
  const provider = providers.find(item => providerKey(item.name) === providerKey(providerName));
  const cloud = isCloudModel(model, provider);
  const providerConfigured = Boolean(provider);
  const endpoint = endpointFromModel(model) || text(provider?.base_url);
  const displayName = text(model?.display_name) || normalizedName;
  const recipe = recipeFromModel(model);
  const backend = backendFromModel(model);

  return {
    modelName: normalizedName,
    displayName,
    kind: cloud ? 'external' : model ? 'internal' : 'unresolved',
    recipe,
    backend,
    provider: cloud ? (text(provider?.name) || providerName) : '',
    endpoint,
    providerConfigured,
    authConfigured: Boolean(provider?.runtime_key_set || provider?.env_var_set),
    allowInsecureHttp: Boolean(provider?.allow_insecure_http),
    modelFound: Boolean(model),
  };
}

export function providerEndpointNeedsInsecureOptIn(value: string): boolean {
  try {
    const parsed = new URL(value.trim());
    const host = parsed.hostname.toLowerCase();
    const loopback = host === 'localhost' || host === '127.0.0.1' || host === '::1' || host === '[::1]';
    return parsed.protocol === 'http:' && !loopback;
  } catch {
    return false;
  }
}

export function validateProviderEndpoint(value: string, allowInsecureHttp = false): string | null {
  const endpoint = value.trim();
  if (!endpoint) return 'Endpoint is required.';
  let parsed: URL;
  try {
    parsed = new URL(endpoint);
  } catch {
    return 'Endpoint must be a valid absolute URL.';
  }
  if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
    return 'Endpoint must use http:// or https://.';
  }
  if (!parsed.hostname) return 'Endpoint must include a host.';
  if (parsed.username || parsed.password) return 'Do not embed credentials in the endpoint URL.';
  if (parsed.hash) return 'Endpoint URLs must not contain a fragment.';
  if (providerEndpointNeedsInsecureOptIn(endpoint) && !allowInsecureHttp) {
    return 'Plain HTTP for a remote provider requires an explicit insecure HTTP opt-in.';
  }
  return null;
}
