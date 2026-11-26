import { isNexusGrantList, isOneOf, NEXUS_ROUTE_FAMILY_NAME, nexusAccountController, NexusRequestError, type NexusGrant } from './nexus';

export const NEXUS_ROLES = ['admin', 'moderator', 'user'] as const;
export type NexusRole = typeof NEXUS_ROLES[number];
export const isNexusRole = isOneOf(NEXUS_ROLES);
export const NEXUS_ROUTE_POLICIES = ['enforced', 'unavailable'] as const;
export type NexusRoutePolicy = typeof NEXUS_ROUTE_POLICIES[number];
const isNexusRoutePolicy = isOneOf(NEXUS_ROUTE_POLICIES);
export const NEXUS_ROUTE_METHODS = ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'HEAD', 'OPTIONS'] as const;
export type NexusRouteMethod = typeof NEXUS_ROUTE_METHODS[number];
const isNexusRouteMethod = isOneOf(NEXUS_ROUTE_METHODS);
export interface NexusRoutePermission {
  family: string;
  method: NexusRouteMethod;
  paths: string[];
  roles: NexusRole[];
}
export interface NexusRoutePermissions {
  catalog_version: number;
  route_policy: NexusRoutePolicy;
  roles: { id: NexusRole; permissions: NexusGrant[] }[];
  routes: NexusRoutePermission[];
}
const strings = (value: unknown): value is string[] => Array.isArray(value) && value.every(item => typeof item === 'string');

export async function getNexusRoutePermissions(): Promise<NexusRoutePermissions> {
  const controller = await nexusAccountController();
  let value: NexusRoutePermissions;
  try { value = await controller.request<NexusRoutePermissions>('route-permissions'); }
  catch (error) {
    if (error instanceof NexusRequestError && error.status === 404) throw new Error('Update this server to view route permissions.');
    throw error;
  }
  if (!value || !Number.isSafeInteger(value.catalog_version) || !isNexusRoutePolicy(value.route_policy) ||
      !Array.isArray(value.roles) || value.roles.length !== NEXUS_ROLES.length ||
      !NEXUS_ROLES.every(id => value.roles.filter(role => role?.id === id && isNexusGrantList(role.permissions)).length === 1) ||
      !Array.isArray(value.routes) || !value.routes.every(route => route && typeof route.family === 'string' && NEXUS_ROUTE_FAMILY_NAME.test(route.family) &&
        isNexusRouteMethod(route.method) &&
        strings(route.paths) && route.paths.length > 0 && route.paths.every(path => path.startsWith('/')) &&
        Array.isArray(route.roles) && route.roles.every(isNexusRole))) {
    throw new Error('The server returned an invalid route permission list.');
  }
  return value;
}
