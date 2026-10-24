import { expect, type Page } from '@playwright/test';
import type { NexusDevice, NexusStatus } from '../src/features/nexus/nexus';
import type { NexusTreeBranch, NexusTreeEntry } from '../src/features/nexus/tree';

const headers = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'Authorization, Content-Type', 'Access-Control-Allow-Methods': 'GET, POST, OPTIONS' };
const self: NexusDevice = { node_id: 'self', device_name: 'My laptop', tunnel_ip: '10.8.0.2', management_pubkey: 'self-key', permissions: ['read', 'admin'], permissions_known: true, is_admin: true, is_online: true, can_remove: false };
const peer: NexusDevice = { node_id: 'peer', device_name: 'Kitchen PC', tunnel_ip: '10.8.0.3', management_pubkey: 'peer-key', permissions: ['read'], permissions_known: true, is_admin: false, is_online: true, can_remove: true };

export async function setupNexusAccount(page: Page, options: {
  desktop?: boolean; registered?: boolean; joined?: boolean; readOnly?: boolean; cancelPasskey?: boolean; unauthorized?: boolean;
  avatarUnsupported?: boolean;
} = {}) {
  const members = [{ ...self, ...(options.readOnly ? { is_admin: false, permissions: ['read'] } : {}) }, peer];
  const state: NexusStatus = { available: true, controller_url: 'http://127.0.0.1:15555', enabled: true,
    registered: options.registered ?? true, locked: options.joined === false, mesh_up: options.joined !== false,
    status: options.joined === false ? 'locked' : 'running', node_id: 'self', group_node_id: options.joined === false ? '' : 'group',
    device_name: 'My laptop', members };
  const entry = (id: string, name: string, kind: 'group' | 'node', parent_id: string): NexusTreeEntry => ({
    id, name, kind, parent_id, node_type: kind === 'group' ? 'customer' : 'endpoint', tunnel_ip: '', is_inference: false,
    management_pubkey: `${id}-key`, permissions: options.readOnly ? ['read'] : ['admin'],
    can_add_group: kind === 'group' && !options.readOnly, can_rename: kind === 'group' && !options.readOnly,
    can_manage: kind === 'group' && !options.readOnly, access_known: !options.readOnly,
    assignments: members.map(member => ({ management_pubkey: member.management_pubkey, permissions: member.permissions })),
  });
  const root = entry('group', 'Home', 'group', '');
  const team = entry('team', 'Team', 'group', 'group');
  const lab = entry('lab', 'Lab', 'group', 'team');
  const branches: Record<string, NexusTreeBranch> = {
    group: { parent: root, children: [team, entry('self', 'My laptop', 'node', 'group'), entry('peer', 'Kitchen PC', 'node', 'group')] },
    team: { parent: team, children: [lab] },
    lab: { parent: lab, children: [entry('workstation', 'Lab workstation', 'node', 'lab')] },
  };
  const calls: { path: string; method: string; body: Record<string, unknown> | null; authorization?: string }[] = [];
  const fixture = { state, calls, accountError: false, rejectWrites: false, invalidTree: false,
    avatarSaveGate: undefined as Promise<void> | undefined,
    avatar: {state: 'ready', image: null as string | null, revision: 0, can_write: !options.readOnly, pending: false} };
  await page.addInitScript(({ desktop, cancelPasskey }) => {
    localStorage.setItem('lemonade_theme', 'dark');
    (window as any).api = { isWebApp: !desktop,
      getSettings: async () => ({ baseURL: { value: 'http://127.0.0.1:13305', type: 'string' }, apiKey: { value: 'fixture-server-key', type: 'string' } }),
      saveSettings: async () => true, getPlatform: async () => 'darwin', getServerBaseUrl: async () => 'http://127.0.0.1:13305' };
    Object.defineProperty(navigator, 'credentials', { configurable: true, value: {
      get: async () => cancelPasskey ? null : { rawId: new Uint8Array([1, 2, 3, 4]).buffer,
        response: { authenticatorData: new Uint8Array(37).buffer, clientDataJSON: new TextEncoder().encode('{}').buffer, signature: new Uint8Array([1]).buffer } },
      create: async () => {
        if (cancelPasskey) return null;
        const pair = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
        const spki = await crypto.subtle.exportKey('spki', pair.publicKey);
        return { rawId: new Uint8Array([1, 2, 3, 4]).buffer, response: { getPublicKey: () => spki } };
      },
    } });
  }, { desktop: options.desktop !== false, cancelPasskey: Boolean(options.cancelPasskey) });
  await page.route('**/api/v1/**', async route => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    const body = request.method() === 'POST' ? request.postDataJSON() as Record<string, unknown> : null;
    if (request.method() === 'OPTIONS') return route.fulfill({ status: 204, headers });
    if (!path.includes('/nexus/')) return route.fulfill({ headers, json: path.endsWith('/health') ? { status: 'ok', version: 'test', all_models_loaded: [] } : path.endsWith('/models') ? { data: [] } : {} });
    calls.push({ path, method: request.method(), body, authorization: request.headers().authorization });
    if (options.unauthorized) return route.fulfill({ status: 401, headers, json: { error: 'Unauthorized' } });
    if (path.endsWith('/events')) return route.fulfill({ status: 204, headers });
    if (path.endsWith('/passkey/challenge')) return route.fulfill({ headers, json: {
      challenge: 'AQIDBA', user_id: 'fixture-identity', rp_id: new URL(page.url()).hostname, credential_id: state.registered ? 'AQIDBA' : '', registration: !state.registered,
    } });
    if (path.endsWith('/passkey/register')) state.registered = true;
    if (path.endsWith('/passkey/unlock')) { state.locked = false; if (!body?.defer_join) { state.mesh_up = true; state.group_node_id = 'group'; } }
    if (path.endsWith('/join')) { state.mesh_up = true; state.group_node_id = 'group'; }
    if (path.endsWith('/account')) return route.fulfill(fixture.accountError
      ? { status: 409, headers, json: { error: 'Account access ended' } }
      : { headers, json: { id: state.group_node_id, member_id: 'fixture-identity', device_node_id: 'self', role: members[0].is_admin ? 'admin' : 'user', devices: members,
        avatar: options.avatarUnsupported ? {state: 'unsupported'} : fixture.avatar } });
    if (path.endsWith('/account/avatar')) {
      await fixture.avatarSaveGate;
      if (fixture.rejectWrites) return route.fulfill({status: 502, headers, json: {error: 'The mesh could not save the image.'}});
      if (options.readOnly) return route.fulfill({status: 403, headers, json: {error: 'Read only account'}});
      if (body?.revision !== fixture.avatar.revision) return route.fulfill({status: 409, headers, json: {error: 'The profile image changed.'}});
      fixture.avatar = {...fixture.avatar, image: body?.image as string | null, pending: false, revision: fixture.avatar.revision + 1};
      return route.fulfill({headers, json: fixture.avatar});
    }
    if (path.endsWith('/invites')) return route.fulfill({ headers, json: { code: '12345678', link_token: `lnk_${'a'.repeat(64)}`, expires_at: Math.floor(Date.now() / 1000) + 600, device_name: body?.device_name } });
    const mutation = path.match(/\/tree\/([^/]+)\/(groups|rename|permissions)$/);
    if (mutation) {
      if (fixture.rejectWrites) return route.fulfill({ status: 403, headers, json: { error: 'This group cannot be changed.' } });
      const [, id, action] = mutation;
      const branch = branches[id];
      if (action === 'groups') {
        const child = entry(`created-${calls.length}`, String(body?.name), 'group', id);
        branch.children.push(child); branches[child.id] = { parent: child, children: [] };
        return route.fulfill({ headers, json: { node_id: child.id } });
      }
      if (action === 'rename') branch.parent.name = String(body?.name);
      return route.fulfill({ headers, json: {} });
    }
    if (path.includes('/tree')) {
      const id = path.match(/\/tree\/([^/]+)\/children$/)?.[1] || 'group';
      return route.fulfill({ headers, json: fixture.invalidTree ? {} : branches[id] });
    }
    return route.fulfill({ headers, json: state });
  });
  await page.goto('/#/connect/nexus');
  await expect(page.getByRole('heading', { name: 'Nexus', exact: true })).toBeVisible();
  return fixture;
}

export async function signInNexusAccount(page: Page, name = 'Geramy') {
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await dialog.getByRole('textbox', { name: 'Account name', exact: true }).fill(name);
  await dialog.getByRole('button', { name: 'Continue with passkey' }).click();
  await expect(page.getByRole('button', { name: `User: ${name}`, exact: true })).toBeVisible();
}
