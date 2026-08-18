import type { ModelInfo } from '../../api';
import {
  ROUTER_RECIPE,
  type RouterDraft,
  type RouterPullRequest,
  buildRouterPullRequest,
  parseRouterPayload,
  routerDisplayName,
} from './routerTypes';

/**
 * Router definitions are persisted by lemond. These record helpers remain only
 * as compatibility shims for callers that construct/export transient records;
 * they deliberately do not read or write browser storage.
 */
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

export function loadRouterRecords(): RouterRecord[] {
  return [];
}

export function upsertRouterRecord(draft: RouterDraft): RouterRecord {
  const request = buildRouterPullRequest(draft);
  const now = Date.now();
  return {
    model_name: request.model_name,
    display_name: draft.name.trim() || routerDisplayName(request.model_name),
    request,
    createdAt: now,
    updatedAt: now,
  };
}

export function deleteRouterRecord(_modelName: string): void {}

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

export function exportRouterRecordsPayload(): Record<string, unknown> {
  return { version: 1, exportedAt: new Date().toISOString(), routers: [] };
}

export function importRouterRecords(payload: unknown): { imported: number; errors: string[] } {
  const items = Array.isArray(payload)
    ? payload
    : isRecord(payload) && Array.isArray(payload.routers)
      ? payload.routers
      : [payload];
  const errors: string[] = [];
  for (let index = 0; index < items.length; index += 1) {
    try {
      parseRouterPayload(items[index]);
      errors.push(`Entry ${index + 1}: register imported routers with lemond instead of browser storage.`);
    } catch (error) {
      errors.push(`Entry ${index + 1}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  return { imported: 0, errors };
}

export function routerRegistrationOptions(model: ModelInfo): Record<string, unknown> | undefined {
  if (String((model as any).recipe || '').toLowerCase() !== ROUTER_RECIPE) return undefined;
  const routing = (model as any).routing;
  const components = Array.isArray((model as any).components) ? (model as any).components : [];
  if (!isRecord(routing) || components.length === 0) return undefined;
  const displayName = String((model as any).display_name || '').trim();
  return {
    version: String((model as any).version || '1'),
    recipe: ROUTER_RECIPE,
    components,
    routing,
    ...(displayName ? { display_name: displayName } : {}),
  };
}
