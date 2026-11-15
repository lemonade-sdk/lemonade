import { nexusAccountController, NexusRequestError } from './nexus';

export type NexusRole = 'admin' | 'moderator' | 'user';
export interface NexusRoutePermission {
  family: string;
  method: string;
  paths: string[];
  roles: NexusRole[];
}
export interface NexusRoutePermissions {
  catalog_version: number;
  route_policy: 'enforced' | 'unavailable';
  roles: { id: NexusRole; permissions: string[] }[];
  routes: NexusRoutePermission[];
}
const roles = ['admin', 'moderator', 'user'];
const strings = (value: unknown): value is string[] => Array.isArray(value) && value.every(item => typeof item === 'string');

export async function getNexusRoutePermissions(): Promise<NexusRoutePermissions> {
  const controller = await nexusAccountController();
  let value: NexusRoutePermissions;
  try { value = await controller.request<NexusRoutePermissions>('route-permissions'); }
  catch (error) {
    if (error instanceof NexusRequestError && error.status === 404) throw new Error('Update this server to view route permissions.');
    throw error;
  }
  if (!value || !Number.isSafeInteger(value.catalog_version) || !['enforced', 'unavailable'].includes(value.route_policy) ||
      !Array.isArray(value.roles) || value.roles.length !== roles.length ||
      !roles.every(id => value.roles.filter(role => role?.id === id && strings(role.permissions)).length === 1) ||
      !Array.isArray(value.routes) || !value.routes.every(route => route && typeof route.family === 'string' &&
        ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'HEAD', 'OPTIONS'].includes(route.method) &&
        strings(route.paths) && route.paths.length > 0 && route.paths.every(path => path.startsWith('/')) &&
        strings(route.roles) && route.roles.every(role => roles.includes(role)))) {
    throw new Error('The server returned an invalid route permission list.');
  }
  return value;
}
