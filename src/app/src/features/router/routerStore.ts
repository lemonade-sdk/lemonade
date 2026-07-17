import type { ModelInfo } from '../../api';
import { scopedStorageKey } from '../accounts/accountStore';
import {
  ROUTER_RECIPE,
  type RouterDraft,
  type RouterPullRequest,
  buildRouterPullRequest,
  parseRouterPayload,
  routerDisplayName,
} from './routerTypes';

const ROUTER_STORE_KEY = 'router_collections';

export interface RouterRecord {
  model_name: string;
  display_name: string;
  request: RouterPullRequest;
  createdAt: number;
  updatedAt: number;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function isRouterRecord(value: unknown): value is RouterRecord {
  if (!isRecord(value) || !isRecord(value.request)) return false;
  return typeof value.model_name === 'string'
    && typeof value.display_name === 'string'
    && value.request.recipe === ROUTER_RECIPE;
}

export function loadRouterRecords(scope: string): RouterRecord[] {
  try {
    const raw = localStorage.getItem(scopedStorageKey(scope, ROUTER_STORE_KEY));
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return (Array.isArray(parsed?.routers) ? parsed.routers : []).filter(isRouterRecord);
  } catch {
    return [];
  }
}

function saveRouterRecords(scope: string, records: RouterRecord[]): void {
  localStorage.setItem(scopedStorageKey(scope, ROUTER_STORE_KEY), JSON.stringify({ version: 1, routers: records }));
}

export function upsertRouterRecord(scope: string, draft: RouterDraft): RouterRecord {
  const request = buildRouterPullRequest(draft);
  const current = loadRouterRecords(scope);
  const previous = current.find(item => item.model_name.toLowerCase() === request.model_name.toLowerCase());
  const now = Date.now();
  const record: RouterRecord = {
    model_name: request.model_name,
    display_name: draft.name.trim() || routerDisplayName(request.model_name),
    request,
    createdAt: previous?.createdAt || now,
    updatedAt: now,
  };
  saveRouterRecords(scope, [record, ...current.filter(item => item.model_name.toLowerCase() !== request.model_name.toLowerCase())]);
  return record;
}

export function deleteRouterRecord(scope: string, modelName: string): void {
  saveRouterRecords(scope, loadRouterRecords(scope).filter(item => item.model_name.toLowerCase() !== modelName.toLowerCase()));
}

export function routerRecordToModelInfo(record: RouterRecord): ModelInfo {
  return {
    id: record.model_name,
    name: record.model_name,
    model_name: record.model_name,
    display_name: record.display_name,
    recipe: ROUTER_RECIPE,
    type: 'chat',
    labels: ['custom', 'router', 'chat'],
    downloaded: true,
    custom: true,
    version: record.request.version,
    components: record.request.components,
    routing: record.request.routing,
    createdAt: new Date(record.createdAt).toISOString(),
  } as ModelInfo;
}

export function routerRecordToDraft(record: RouterRecord): RouterDraft {
  const draft = parseRouterPayload(record.request);
  return { ...draft, name: record.display_name };
}

export function exportRouterRecordsPayload(scope: string): Record<string, unknown> {
  return {
    version: 1,
    exportedAt: new Date().toISOString(),
    routers: loadRouterRecords(scope).map(record => record.request),
  };
}

export function importRouterRecords(scope: string, payload: unknown): { imported: number; errors: string[] } {
  const items = Array.isArray(payload)
    ? payload
    : isRecord(payload) && Array.isArray(payload.routers)
      ? payload.routers
      : [payload];
  const result = { imported: 0, errors: [] as string[] };
  for (let index = 0; index < items.length; index += 1) {
    try {
      const draft = parseRouterPayload(items[index]);
      upsertRouterRecord(scope, draft);
      result.imported += 1;
    } catch (error) {
      result.errors.push(`Entry ${index + 1}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  return result;
}

export function routerRegistrationOptions(model: ModelInfo): Record<string, unknown> | undefined {
  if (String((model as any).recipe || '').toLowerCase() !== ROUTER_RECIPE) return undefined;
  const routing = (model as any).routing;
  const components = Array.isArray((model as any).components) ? (model as any).components : [];
  if (!isRecord(routing) || components.length === 0) return undefined;
  return {
    version: String((model as any).version || '1'),
    recipe: ROUTER_RECIPE,
    components,
    routing,
  };
}
