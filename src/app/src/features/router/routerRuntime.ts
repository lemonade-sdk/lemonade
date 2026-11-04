import type { LoadedModel, ModelInfo } from '../../api';
import { ROUTER_RECIPE } from './routerTypes';

export type RouterDependencyRole = 'candidate' | 'default' | 'classifier' | 'llm-router' | 'component';

export interface RouterDependencyStatus {
  name: string;
  roles: RouterDependencyRole[];
  present: boolean;
  downloaded: boolean;
  loaded: boolean;
  recipe?: string;
}

export interface RouterPreflightResult {
  ok: boolean;
  errors: string[];
  warnings: string[];
  dependencies: RouterDependencyStatus[];
}

function record(value: unknown): Record<string, any> | null {
  return value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, any> : null;
}

export function routerModelName(model: ModelInfo | LoadedModel | null | undefined): string {
  const value = model as any;
  return String(value?.model_name || value?.name || value?.id || '').trim();
}

export function isRouterModelInfo<T extends ModelInfo | LoadedModel>(
  model: T | null | undefined,
): model is T {
  const recipe = String((model as any)?.recipe || '')
    .trim()
    .toLowerCase();
  return recipe === ROUTER_RECIPE
    || recipe.startsWith(`${ROUTER_RECIPE}.`);
}

function isDownloaded(model: ModelInfo | null | undefined): boolean {
  if (!model) return false;
  const value = model as any;
  if (value.downloaded === true || value.is_downloaded === true || value.local === true || value.installed === true) return true;
  const status = String(value.status || value.state || '').toLowerCase();
  if (['downloaded', 'ready', 'local', 'installed'].includes(status)) return true;
  return Array.isArray(value.labels) && value.labels.some((label: unknown) => ['downloaded', 'ready', 'local', 'installed'].includes(String(label).toLowerCase()));
}

export function routerDependencies(model: ModelInfo | null | undefined): Array<{ name: string; roles: RouterDependencyRole[] }> {
  if (!isRouterModelInfo(model)) return [];
  const value = model as any;
  const routing = record(value.routing) || {};
  const roles = new Map<string, { name: string; roles: Set<RouterDependencyRole> }>();
  const add = (raw: unknown, role: RouterDependencyRole) => {
    const name = String(raw || '').trim();
    if (!name) return;
    const key = name.toLowerCase();
    const current = roles.get(key) || { name, roles: new Set<RouterDependencyRole>() };
    current.roles.add(role);
    roles.set(key, current);
  };

  (Array.isArray(value.components) ? value.components : []).forEach((name: unknown) => add(name, 'component'));
  (Array.isArray(routing.candidates) ? routing.candidates : []).forEach((name: unknown) => add(name, 'candidate'));
  add(routing.default_model, 'default');
  add(record(routing.router)?.model, 'llm-router');
  (Array.isArray(routing.classifiers) ? routing.classifiers : []).forEach((item: unknown) => add(record(item)?.model, 'classifier'));

  return [...roles.values()].map(item => ({ name: item.name, roles: [...item.roles] }));
}

export function preflightRouter(
  model: ModelInfo | null | undefined,
  availableModels: readonly ModelInfo[],
  loadedModels: readonly LoadedModel[] = [],
): RouterPreflightResult {
  if (!isRouterModelInfo(model)) return { ok: true, errors: [], warnings: [], dependencies: [] };
  const value = model as any;
  const routing = record(value.routing);
  const errors: string[] = [];
  const warnings: string[] = [];
  const candidates = routing && Array.isArray(routing.candidates) ? routing.candidates.map(String).map(v => v.trim()).filter(Boolean) : [];
  const defaultModel = String(routing?.default_model || '').trim();
  const routerBlock = record(routing?.router);
  const rules = routing && Array.isArray(routing.rules) ? routing.rules : [];
  const classifiers = routing && Array.isArray(routing.classifiers) ? routing.classifiers : [];
  if (!routing) errors.push('Router routing policy is missing.');
  if (candidates.length === 0) errors.push('Router has no candidate models.');
  if (!defaultModel) errors.push('Router has no default model.');
  if (defaultModel && candidates.length > 0 && !candidates.some(name => name.toLowerCase() === defaultModel.toLowerCase())) {
    errors.push(`Default model ${defaultModel} is not in routing.candidates.`);
  }
  if (routerBlock) {
    if (String(routerBlock.type || '').trim().toLowerCase() !== 'llm') errors.push('routing.router.type must be "llm".');
    if (!String(routerBlock.model || '').trim()) errors.push('LLM Router helper model is missing.');
    if (rules.length > 0 || classifiers.length > 0) errors.push('routing.router cannot be combined with routing.rules or routing.classifiers.');
  } else if (routing && rules.length === 0) {
    errors.push('Rule Router has no routing.rules.');
  }
  for (const rawRule of rules) {
    const rule = record(rawRule);
    const target = String(rule?.route_to || '').trim();
    if (!target) errors.push('A Router rule is missing route_to.');
    else if (!candidates.some(name => name.toLowerCase() === target.toLowerCase())) errors.push(`Rule target ${target} is not in routing.candidates.`);
  }

  const available = new Map<string, ModelInfo>();
  availableModels.forEach(item => {
    const name = routerModelName(item);
    if (name) available.set(name.toLowerCase(), item);
  });
  const loaded = new Set(loadedModels.map(item => routerModelName(item).toLowerCase()).filter(Boolean));
  const components = new Set((Array.isArray(value.components) ? value.components : []).map((item: unknown) => String(item).trim().toLowerCase()).filter(Boolean));

  const dependencies = routerDependencies(model).map(dep => {
    const found = available.get(dep.name.toLowerCase()) || null;
    const status: RouterDependencyStatus = {
      ...dep,
      present: !!found,
      downloaded: isDownloaded(found),
      loaded: loaded.has(dep.name.toLowerCase()),
      recipe: found ? String((found as any).recipe || '') || undefined : undefined,
    };
    if (!found) errors.push(`Referenced ${dep.roles.join('/')} model ${dep.name} is not present in the Lemonade model registry.`);
    if (dep.roles.some(role => role !== 'component') && !components.has(dep.name.toLowerCase())) {
      errors.push(`Referenced ${dep.roles.filter(role => role !== 'component').join('/')} model ${dep.name} is missing from Router components.`);
    }
    const foundRecipe = String((found as any)?.recipe || '').toLowerCase();
    if (found && !status.downloaded && foundRecipe !== 'cloud' && !foundRecipe.startsWith('cloud.')) {
      errors.push(`${dep.name} is referenced by the Router but is not downloaded/local. Download or register it before using this Router.`);
    }
    return status;
  });

  return { ok: errors.length === 0, errors: [...new Set(errors)], warnings: [...new Set(warnings)], dependencies };
}

export function routerPreflightError(result: RouterPreflightResult): string {
  if (result.ok) return '';
  return `Router is not ready: ${result.errors.join(' ')}`;
}
