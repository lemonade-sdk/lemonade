import { nexusRequest } from './nexus';

export interface NexusTreeEntry {
  id: string;
  parent_id: string;
  name: string;
  kind: 'group' | 'node';
  node_type: string;
  tunnel_ip: string;
  is_inference: boolean;
  management_pubkey: string;
  permissions: string[];
  can_add_group: boolean;
  can_rename: boolean;
  can_manage: boolean;
  access_known: boolean;
  assignments: { management_pubkey: string; permissions: string[] }[];
}

export interface NexusTreeBranch { parent: NexusTreeEntry; children: NexusTreeEntry[] }
export type NexusTreeBranches = Record<string, NexusTreeBranch>;
export interface NexusTreeRow { entry: NexusTreeEntry; depth: number; position: number; siblings: number }

export function nexusKeysEqual(left: string, right: string): boolean {
  const canonical = (key: string) => key.replace(/^ed25519:/, '').replace(/-/g, '+').replace(/_/g, '/').replace(/=+$/, '');
  return Boolean(canonical(left)) && canonical(left) === canonical(right);
}

export function visibleTreeRows(root: NexusTreeEntry, branches: NexusTreeBranches, expanded: ReadonlySet<string>): NexusTreeRow[] {
  const rows: NexusTreeRow[] = [];
  const seen = new Set<string>();
  const pending: NexusTreeRow[] = [{ entry: root, depth: 1, position: 1, siblings: 1 }];
  while (pending.length) {
    const row = pending.pop()!;
    if (seen.has(row.entry.id)) continue;
    seen.add(row.entry.id);
    rows.push(row);
    if (row.entry.kind !== 'group' || !expanded.has(row.entry.id)) continue;
    const children = branches[row.entry.id]?.children || [];
    for (let i = children.length - 1; i >= 0; i -= 1) {
      if (children[i].parent_id === row.entry.id) pending.push({ entry: children[i], depth: row.depth + 1, position: i + 1, siblings: children.length });
    }
  }
  return rows;
}

export function treeBreadcrumbs(entry: NexusTreeEntry, entries: ReadonlyMap<string, NexusTreeEntry>): NexusTreeEntry[] {
  const path: NexusTreeEntry[] = [];
  const seen = new Set<string>();
  let current: NexusTreeEntry | undefined = entry;
  while (current && !seen.has(current.id)) {
    seen.add(current.id);
    path.unshift(current);
    current = entries.get(current.parent_id);
  }
  return path;
}

export function readNexusTreeBranch(value: unknown): NexusTreeBranch {
  const branch = value as NexusTreeBranch | null;
  const entryValid = (entry: NexusTreeEntry | undefined) => entry && typeof entry.id === 'string' && Boolean(entry.id)
    && typeof entry.parent_id === 'string' && typeof entry.name === 'string' && ['group', 'node'].includes(entry.kind)
    && Array.isArray(entry.permissions) && entry.permissions.every(permission => typeof permission === 'string')
    && Array.isArray(entry.assignments) && entry.assignments.every(assignment => assignment && typeof assignment.management_pubkey === 'string'
      && Array.isArray(assignment.permissions) && assignment.permissions.every(permission => typeof permission === 'string'))
    && typeof entry.node_type === 'string' && typeof entry.management_pubkey === 'string'
    && typeof entry.tunnel_ip === 'string' && typeof entry.is_inference === 'boolean'
    && ['can_add_group', 'can_rename', 'can_manage', 'access_known'].every(key => typeof entry[key as keyof NexusTreeEntry] === 'boolean');
  if (!branch || !entryValid(branch.parent) || branch.parent.kind !== 'group' || !Array.isArray(branch.children)
    || branch.children.some(entry => !entryValid(entry) || entry.parent_id !== branch.parent.id)) {
    throw new Error('The server returned invalid group data. Update the server and try again.');
  }
  return branch;
}
export const getNexusTreeBranch = async (id?: string) => readNexusTreeBranch(await nexusRequest(id ? `tree/${encodeURIComponent(id)}/children` : 'tree'));
export const createNexusGroup = (id: string, name: string) => nexusRequest<{ node_id: string }>(`tree/${encodeURIComponent(id)}/groups`, { name });
export const renameNexusGroup = (id: string, name: string) => nexusRequest(`tree/${encodeURIComponent(id)}/rename`, { name });
export const saveNexusGroupPermissions = (id: string, memberId: string, admin: boolean, permissions: string[]) =>
  nexusRequest<NexusTreeBranch>(`tree/${encodeURIComponent(id)}/permissions`, { member_node_id: memberId, admin, permissions });
