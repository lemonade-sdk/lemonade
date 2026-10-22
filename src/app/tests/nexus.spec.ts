import { expect, test, type Page } from '@playwright/test';

const headers = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'Authorization, Content-Type', 'Access-Control-Allow-Methods': 'GET, POST, OPTIONS' };
const self = { node_id: 'self', device_name: 'My laptop', tunnel_ip: '10.8.0.2/32', management_pubkey: 'self-key', permissions: ['read', 'write', 'add_child', 'edit_node', 'delete_node', 'admin'], permissions_known: true, is_admin: true, is_owner: true, is_inference: true, tier: 3, access_known: true, can_remove: false };
const peer = { node_id: 'peer', device_name: 'Kitchen PC', tunnel_ip: '10.8.0.3/32', management_pubkey: 'peer-key', permissions: ['read', 'write'], permissions_known: true, is_admin: false, is_owner: false, is_inference: false, tier: 4, access_known: true, can_remove: true };
const tablet = { node_id: 'tablet', device_name: 'Tablet', tunnel_ip: '10.8.0.4/32', management_pubkey: 'tablet-key', permissions: ['read'], permissions_known: true, is_admin: false, is_owner: false, is_inference: false, tier: 4, access_known: true, can_remove: true };
const workstation = { node_id: 'workstation', device_name: 'GPU tower', tunnel_ip: '10.8.0.5/32', management_pubkey: 'workstation-key', permissions: ['read'], permissions_known: true, is_admin: false, is_owner: false, is_inference: true, tier: 3, access_known: true, can_remove: true, is_online: true };
const memberSelf = { ...self, permissions: ['read'], is_admin: false, is_owner: false, can_remove: false };
interface Member { node_id: string; device_name: string; tunnel_ip: string; management_pubkey: string; permissions: string[]; permissions_known: boolean; is_admin: boolean; is_owner: boolean; is_inference: boolean; tier: number; access_known: boolean; can_remove: boolean; is_online?: boolean }
interface Options { members?: Member[]; accessKnown?: boolean; baseUrl?: string; apiKey?: string; remoteManagement?: boolean; refuse?: { status: number; error: string }; status?: Record<string, unknown> }
async function setup(page: Page, desktop = true, locked = false, options: Options = {}) {
  const members: Member[] = options.members ?? [self, peer];
  const baseUrl = options.baseUrl ?? 'http://127.0.0.1:13305';
  const apiKey = options.apiKey ?? 'local-secret';
  const state = { available: true, controller_url: 'http://127.0.0.1:15555', remote_management: options.remoteManagement ?? false, enabled: true, registered: true, locked, mesh_up: !locked, status: locked ? 'locked' : 'running', node_id: 'self', group_node_id: 'group', device_name: 'My laptop', tunnel_ip: '10.8.0.2', peer_count: 1, members: locked ? [] : members };
  Object.assign(state, options.status ?? {});
  const access = (nodeId: string) => ({ node_id: nodeId, device_name: state.members.find(member => member.node_id === nodeId)?.device_name, access_known: options.accessKnown ?? true, can_manage: true,
    members: state.members.filter(member => member.node_id !== nodeId && member.node_id !== 'self').map(member => ({ node_id: member.node_id, device_name: member.device_name, is_admin: member.is_admin, tier: member.tier, see: member.node_id === 'peer', connect: false, editable: options.accessKnown ?? true })) });
  const calls: { url: string; path: string; body: unknown; authorization?: string; peerAuthorization?: string }[] = [];
  const relayed: { url: string; authorization?: string; peerAuthorization?: string }[] = [];
  await page.addInitScript(({ desktop, baseUrl, apiKey }) => {
    (window as any).api = {
      isWebApp: !desktop,
      getSettings: async () => ({ baseURL: { value: baseUrl, type: 'string' }, apiKey: { value: apiKey, type: 'string' } }),
      saveSettings: async () => true,
      getPlatform: async () => 'darwin',
      getServerBaseUrl: async () => baseUrl,
    };
  }, { desktop, baseUrl, apiKey });
  await page.route('**/api/v1/**', async route => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (request.method() === 'OPTIONS') return route.fulfill({ status: 204, headers });
    if (path.includes('/nexus/peers/')) {
      relayed.push({ url: request.url(), authorization: request.headers().authorization, peerAuthorization: request.headers()['x-lemonade-peer-authorization'] });
      if (path.endsWith('/health')) return route.fulfill({ headers, json: { status: 'ok', version: 'peer', all_models_loaded: [] } });
      if (path.endsWith('/models')) return route.fulfill({ headers, json: { data: [] } });
      return route.fulfill({ headers, json: {} });
    }
    if (path.includes('/nexus/')) {
      calls.push({ url: request.url(), path, body: request.postDataJSON(), authorization: request.headers().authorization, peerAuthorization: request.headers()['x-lemonade-peer-authorization'] });
      if (options.refuse) return route.fulfill({ status: options.refuse.status, headers, json: { error: options.refuse.error } });
      if (path.endsWith('/remove')) state.members = [self];
      if (path.endsWith('/invites')) return route.fulfill({ headers, json: { code: '12345678', link_token: `lnk_${'a'.repeat(64)}`, expires_at: Math.floor(Date.now() / 1000) + 600, device_name: request.postDataJSON().device_name } });
      const target = path.match(/\/(?:devices|members)\/([^/]+)\/(?:access|role|permissions)$/)?.[1];
      const json = path.endsWith('/egress') ? { endpoint: 'http://127.0.0.1:16666' }
        : path.endsWith('/access') ? access(target!)
        : target ? state.members.find(member => member.node_id === target) : state;
      return route.fulfill({ headers, json });
    }
    if (path.endsWith('/health')) return route.fulfill({ headers, json: { status: 'ok', version: 'test', all_models_loaded: [] } });
    if (path.endsWith('/models')) return route.fulfill({ headers, json: { data: [] } });
    return route.fulfill({ headers, json: {} });
  });
  await page.goto('/#/connect/devices-and-mesh');
  await expect(page.getByRole('heading', { name: 'Devices in your group' })).toBeVisible();
  return { state, calls, relayed };
}

test('desktop shortcut opens device information and removes the selected peer', async ({ page }) => {
  const { calls } = await setup(page);
  await expect(page.getByRole('button', { name: 'Devices and mesh: Mesh connected' })).toBeVisible();
  await expect(page.locator('.nexus-panel__device-list')).toContainText('Kitchen PC');
  await expect(page.locator('.nexus-panel__facts')).toContainText('10.8.0.2');
  await page.getByRole('button', { name: 'Remove Kitchen PC' }).click();
  const dialog = page.getByRole('dialog', { name: 'Remove a device' });
  await expect(dialog).toContainText('Kitchen PC');
  await dialog.getByRole('button', { name: 'Remove device', exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.locator('.nexus-panel__device-list')).not.toContainText('Kitchen PC');
  expect(calls.find(call => call.path.endsWith('/devices/peer/remove'))?.body).toEqual({});
  expect(calls.find(call => call.path.endsWith('/devices/peer/remove'))?.authorization).toBe('Bearer local-secret');
});

test('invitation dialog traps focus and restores it when dismissed', async ({ page }) => {
  await setup(page);
  const trigger = page.getByRole('button', { name: 'Add a device', exact: true });
  await trigger.click();
  const dialog = page.getByRole('dialog', { name: 'Invite a device' });
  await expect(dialog).toBeVisible();
  await expect(dialog.getByRole('button', { name: 'Close dialog' })).toBeFocused();
  await page.keyboard.press('Shift+Tab');
  await expect(dialog.getByRole('button', { name: 'Create invite' })).not.toBeFocused();
  await page.keyboard.press('Escape');
  await expect(dialog).toHaveCount(0);
  await expect(trigger).toBeFocused();
});

test('joining accepts short codes and full federation tokens and rejects malformed values', async ({ page }) => {
  await setup(page, true, true);
  await page.getByRole('button', { name: 'Join a group', exact: true }).click();
  const submit = page.getByRole('button', { name: 'Confirm with passkey and join' });
  const input = page.getByLabel('Pairing code or invite token');
  await input.fill('1234567'); await expect(submit).toBeDisabled();
  await input.fill('12345678'); await expect(submit).toBeEnabled();
  await input.fill(`lnk_${'a'.repeat(64)}`); await expect(submit).toBeEnabled();
  await input.fill(`lnk_${'z'.repeat(64)}`); await expect(submit).toBeDisabled();
});

test('browser device overview exposes no desktop management actions', async ({ page }) => {
  await setup(page, false);
  await expect(page.locator('.nexus-panel__device-list')).toContainText('Kitchen PC');
  await expect(page.getByRole('button', { name: 'Add a device' })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Remove Kitchen PC' })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Connect to server', exact: true })).toHaveCount(0);
  await expect(page.getByRole('button', { name: /^Manage access for/ })).toHaveCount(0);
});

test('remote server key goes only to the selected peer through the relay', async ({ page }) => {
  const { calls, relayed } = await setup(page, true, false, { members: [self, peer, workstation] });
  await expect(page.getByRole('button', { name: 'Connect to server', exact: true })).toHaveCount(1);
  await page.getByLabel('Remote server API key (if required)').fill('peer-secret');
  await page.getByRole('button', { name: 'Connect to server', exact: true }).click();
  await expect(page).toHaveURL(/connect\/server/);
  expect(relayed.length).toBeGreaterThan(0);
  for (const call of relayed) {
    expect(call.url.startsWith('http://127.0.0.1:13305/api/v1/nexus/peers/workstation/api/v1/')).toBe(true);
    expect(call.authorization).toBe('Bearer local-secret');
    expect(call.peerAuthorization).toBe('Bearer peer-secret');
  }
  await page.getByRole('button', { name: 'Devices and mesh: Mesh connected' }).click();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  const status = calls.filter(call => call.path.endsWith('/status')).at(-1);
  expect(status?.authorization).toBe('Bearer local-secret');
  expect(status?.peerAuthorization).toBeUndefined();
  expect(calls.some(call => call.path.endsWith('/egress'))).toBe(false);
});

test('header server selector lists online inference devices and relays chat through this server', async ({ page }) => {
  await setup(page, true, false, { members: [self, peer, workstation, { ...workstation, node_id: 'sleeping', device_name: 'Sleeping GPU', is_online: false }] });
  const requests: string[] = [];
  await page.route('**/api/v1/health**', route => {
    const remote = route.request().url().includes('/nexus/peers/');
    return route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [{ model_name: remote ? 'MeshModel' : 'LocalModel', labels: ['chat'], recipe: 'llamacpp' }] } });
  });
  await page.route('**/api/v1/models**', route => {
    const remote = route.request().url().includes('/nexus/peers/');
    return route.fulfill({ headers, json: { data: [{ id: remote ? 'MeshModel' : 'LocalModel', labels: ['chat'], recipe: 'llamacpp', downloaded: true }] } });
  });
  await page.route('**/api/v1/chat/completions', route => {
    requests.push(route.request().url());
    return route.fulfill({ headers, contentType: 'text/event-stream', body: 'data: {"choices":[{"delta":{"content":"Mesh inference worked."}}]}\n\ndata: [DONE]\n\n' });
  });
  await page.locator('.titlebar__nav').getByRole('button', { name: 'Chat', exact: true }).click();
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await expect(selector.locator('option')).toHaveText(['Local Server', 'GPU tower · Tier 3 · Online']);
  await selector.selectOption('workstation');
  await expect(selector).toHaveValue('workstation');
  await expect(selector).toBeEnabled();
  await expect(page).toHaveURL(/#\/chat$/);
  await page.getByRole('textbox', { name: 'Message', exact: true }).fill('Say hello');
  await page.getByRole('button', { name: 'Send', exact: true }).click();
  await expect.poll(() => requests.length).toBe(1);
  expect(requests[0]).toBe('http://127.0.0.1:13305/api/v1/nexus/peers/workstation/api/v1/chat/completions');
  await expect(page.getByText('Mesh inference worked.', { exact: true })).toBeVisible();
  await selector.selectOption('local');
  await expect(selector).toHaveValue('local');
  await expect(selector).toBeEnabled();
  expect(await page.evaluate(() => localStorage.getItem('lemonade_base_url'))).not.toContain('/nexus/peers/');
});

test('failed mesh server selection restores the previous inference connection', async ({ page }) => {
  await setup(page, true, false, { members: [self, workstation] });
  await page.route('**/nexus/peers/workstation/api/v1/health**', route => route.fulfill({ status: 409, headers, json: { error: 'That device is offline' } }));
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('workstation');
  await expect(selector).toBeEnabled();
  await expect(selector).toHaveValue('local');
  await expect(page.getByRole('alert')).toBeVisible();
});

test('selector requests a protected peer key and keeps it separate when returning to local', async ({ page }) => {
  await setup(page, true, false, { members: [self, workstation] });
  const peerKeys: string[] = [];
  const serverKeys: string[] = [];
  await page.route('**/nexus/peers/workstation/api/v1/health**', route => {
    const peerKey = route.request().headers()['x-lemonade-peer-authorization'] || '';
    peerKeys.push(peerKey);
    serverKeys.push(route.request().headers().authorization || '');
    return peerKey === 'Bearer peer-secret'
      ? route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [] } })
      : route.fulfill({ status: 401, headers, json: { error: 'API key required' } });
  });
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('workstation');
  const dialog = page.getByRole('dialog', { name: 'Server API key', exact: true });
  await expect(dialog).toBeVisible();
  await dialog.getByLabel('Remote server API key', { exact: true }).fill('peer-secret');
  await dialog.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(selector).toHaveValue('workstation');
  await selector.selectOption('local');
  await expect(selector).toBeEnabled();
  await expect(selector).toHaveValue('local');
  expect(peerKeys).toContain('Bearer peer-secret');
  expect(peerKeys).not.toContain('Bearer local-secret');
  expect(serverKeys.every(key => key === 'Bearer local-secret')).toBe(true);
});

test('member list shows admins and device tiers', async ({ page }) => {
  await setup(page);
  const list = page.locator('.nexus-panel__device-list');
  const own = list.locator('li', { hasText: 'My laptop' });
  await expect(own.locator('.nexus-panel__badge')).toHaveText('Admin');
  await expect(own).toContainText('Tier 3 · Inference server');
  const kitchen = list.locator('li', { hasText: 'Kitchen PC' });
  await expect(kitchen.locator('.nexus-panel__badge')).toHaveCount(0);
  await expect(kitchen).toContainText('Tier 4 · Client');
});

test('admin promotes a member and grants another member access to its device', async ({ page }) => {
  const { calls } = await setup(page, true, false, { members: [self, peer, tablet] });
  await page.getByRole('button', { name: 'Manage access for Kitchen PC' }).click();
  const dialog = page.getByRole('dialog', { name: 'Manage access' });
  await expect(dialog).toContainText('Kitchen PC · Tier 4 · Client');
  await expect(dialog.getByRole('checkbox', { name: 'Tablet can see this device' })).toBeEnabled();
  await dialog.getByRole('radio', { name: 'Admin' }).check();
  await expect(dialog.getByRole('checkbox', { name: 'Invite devices' })).toBeDisabled();
  await dialog.getByRole('checkbox', { name: 'Tablet can connect to this device' }).check();
  await expect(dialog.getByRole('checkbox', { name: 'Tablet can see this device' })).toBeChecked();
  await dialog.getByRole('button', { name: 'Save access' }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.getByText('Access updated for Kitchen PC.')).toBeVisible();
  expect(calls.find(call => call.path.endsWith('/members/peer/role'))?.body).toEqual({ role: 'admin' });
  expect(calls.some(call => call.path.endsWith('/members/peer/permissions'))).toBe(false);
  expect(calls.find(call => call.path.endsWith('/devices/peer/access') && call.body)?.body).toEqual({ member_node_id: 'tablet', see: true, connect: true });
  expect(calls.filter(call => call.path.includes('/members/') || call.path.includes('/access')).every(call => call.authorization === 'Bearer local-secret')).toBe(true);
});

test('admin changes a member group permission without changing its role', async ({ page }) => {
  const { calls } = await setup(page);
  await page.getByRole('button', { name: 'Manage access for Kitchen PC' }).click();
  const dialog = page.getByRole('dialog', { name: 'Manage access' });
  await expect(dialog.getByRole('checkbox', { name: 'See the group' })).toBeDisabled();
  await dialog.getByRole('checkbox', { name: 'Invite devices' }).check();
  await dialog.getByRole('button', { name: 'Save access' }).click();
  await expect(dialog).toHaveCount(0);
  expect(calls.find(call => call.path.endsWith('/members/peer/permissions'))?.body).toEqual({ permissions: ['read', 'write', 'add_child'] });
  expect(calls.some(call => call.path.endsWith('/members/peer/role'))).toBe(false);
});

test('admin cannot change its own role but controls who uses this device', async ({ page }) => {
  const { calls } = await setup(page);
  await page.getByRole('button', { name: 'Manage access for My laptop' }).click();
  const dialog = page.getByRole('dialog', { name: 'Manage access' });
  await expect(dialog.getByRole('radio', { name: 'Member' })).toBeDisabled();
  await expect(dialog.getByRole('radio', { name: 'Admin' })).toBeDisabled();
  await expect(dialog).toContainText('You can’t change your own role or group permissions. Ask another admin.');
  await expect(dialog.getByRole('checkbox', { name: 'Invite devices' })).toBeDisabled();
  await dialog.getByRole('checkbox', { name: 'Kitchen PC can connect to this device' }).check();
  await dialog.getByRole('button', { name: 'Save access' }).click();
  await expect(dialog).toHaveCount(0);
  expect(calls.some(call => call.path.endsWith('/members/self/role') || call.path.endsWith('/members/self/permissions'))).toBe(false);
  expect(calls.find(call => call.path.endsWith('/devices/self/access') && call.body)?.body).toEqual({ member_node_id: 'peer', see: true, connect: true });
});

test('hidden device access lists explain why they cannot be edited', async ({ page }) => {
  await setup(page, true, false, { accessKnown: false });
  await page.getByRole('button', { name: 'Manage access for Kitchen PC' }).click();
  const dialog = page.getByRole('dialog', { name: 'Manage access' });
  await expect(dialog).toContainText('You can’t see this device’s access list. Ask the admin who invited it.');
  await expect(dialog.getByRole('checkbox', { name: 'My laptop can see this device' })).toHaveCount(0);
});

test('members without admin cannot manage access', async ({ page }) => {
  await setup(page, true, false, { members: [memberSelf, { ...peer, is_admin: true, is_owner: true }] });
  await expect(page.locator('.nexus-panel__device-list li', { hasText: 'Kitchen PC' }).locator('.nexus-panel__badge')).toHaveText('Admin');
  await expect(page.getByRole('button', { name: /^Manage access for/ })).toHaveCount(0);
  await expect(page.locator('.nexus-panel__details')).toContainText('Member');
});

test('remote GUI manages the group through the server API key without local-only controls', async ({ page }) => {
  const { calls } = await setup(page, true, false, { baseUrl: 'http://192.168.1.50:13305', apiKey: 'remote-secret', remoteManagement: true });
  await expect(page.getByText('You are managing this server remotely.', { exact: false })).toBeVisible();
  for (const name of ['Turn mesh off', 'Lock network', 'Save name', 'Unlock with passkey', 'Connect to server']) {
    await expect(page.getByRole('button', { name, exact: true })).toHaveCount(0);
  }
  await expect(page.getByRole('combobox', { name: 'Inference server', exact: true }).locator('option')).toHaveText(['Local Server']);
  await expect(page.getByLabel('Remote server API key (if required)')).toHaveCount(0);

  await page.getByRole('button', { name: 'Manage access for Kitchen PC' }).click();
  const access = page.getByRole('dialog', { name: 'Manage access' });
  await access.getByLabel('Admin', { exact: true }).check();
  await access.getByRole('button', { name: 'Save access', exact: true }).click();
  await expect(access).toHaveCount(0);
  expect(calls.find(call => call.path.endsWith('/members/peer/role'))?.body).toEqual({ role: 'admin' });

  await page.getByRole('button', { name: 'Add a device', exact: true }).click();
  const invite = page.getByRole('dialog', { name: 'Invite a device' });
  await invite.getByLabel('Device you’re inviting').fill('Office PC');
  await invite.getByRole('button', { name: 'Create invite', exact: true }).click();
  await expect(invite).toContainText('1234 5678');
  expect(calls.find(call => call.path.endsWith('/invites'))?.body).toEqual({ device_name: 'Office PC', ttl_sec: 600 });

  expect(calls.length).toBeGreaterThan(0);
  for (const call of calls) {
    expect(new URL(call.url).host).toBe('192.168.1.50:13305');
    expect(call.authorization).toBe('Bearer remote-secret');
  }
  expect(calls.some(call => /\/(passkey|events|enable|lock|device)(\/|$)/.test(call.path.replace(/^.*\/nexus\//, '/')))).toBe(false);
});

test('remote GUI explains when the server has no API key for remote management', async ({ page }) => {
  await setup(page, false, false, { baseUrl: 'http://192.168.1.50:13305', apiKey: '',
    refuse: { status: 403, error: 'Remote network management is off. Set LEMONADE_API_KEY on this server to turn it on.' } });
  await expect(page.getByRole('alert')).toContainText('LEMONADE_API_KEY');
  await expect(page.getByRole('button', { name: 'Add a device' })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Join a group' })).toHaveCount(0);
  await expect(page.getByRole('button', { name: /^Manage access for/ })).toHaveCount(0);
});

test('remote GUI selects a mesh inference device through the remote server relay', async ({ page }) => {
  const { relayed } = await setup(page, true, false, { baseUrl: 'http://192.168.1.50:13305', apiKey: 'remote-secret', remoteManagement: true, members: [self, peer, workstation] });
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await expect(selector.locator('option')).toHaveText(['Local Server', 'GPU tower · Tier 3 · Online']);
  await selector.selectOption('workstation');
  await expect(selector).toHaveValue('workstation');
  await expect(selector).toBeEnabled();
  expect(relayed.length).toBeGreaterThan(0);
  for (const call of relayed) {
    expect(call.url.startsWith('http://192.168.1.50:13305/api/v1/nexus/peers/workstation/api/v1/')).toBe(true);
    expect(call.authorization).toBe('Bearer remote-secret');
  }
});

test('browser GUI selects a mesh inference device through its server relay', async ({ page }) => {
  const { relayed } = await setup(page, false, false, { members: [self, peer, workstation] });
  await expect(page.getByRole('button', { name: /^Manage access for/ })).toHaveCount(0);
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('workstation');
  await expect(selector).toHaveValue('workstation');
  await expect(selector).toBeEnabled();
  expect(relayed.length).toBeGreaterThan(0);
  expect(relayed.every(call => call.url.startsWith('http://127.0.0.1:13305/api/v1/nexus/peers/workstation/api/v1/'))).toBe(true);
});

async function chatThroughSelector(page: Page) {
  const requests: { url: string; peerKey?: string }[] = [];
  await page.route('**/api/v1/health**', route => route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [{ model_name: 'LocalModel', labels: ['chat'], recipe: 'llamacpp' }] } }));
  await page.route('**/api/v1/models**', route => route.fulfill({ headers, json: { data: [{ id: 'LocalModel', labels: ['chat'], recipe: 'llamacpp', downloaded: true }] } }));
  await page.route('**/api/v1/chat/completions', route => {
    requests.push({ url: route.request().url(), peerKey: route.request().headers()['x-lemonade-peer-authorization'] });
    return route.fulfill({ headers, contentType: 'text/event-stream', body: 'data: {"choices":[{"delta":{"content":"Local inference worked."}}]}\n\ndata: [DONE]\n\n' });
  });
  await page.reload();
  await page.locator('.titlebar__nav').getByRole('button', { name: 'Chat', exact: true }).click();
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  const send = async () => {
    await page.getByRole('textbox', { name: 'Message', exact: true }).fill('Say hello');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await expect.poll(() => requests.length).toBeGreaterThan(0);
  };
  return { requests, selector, send };
}

for (const [name, options] of [
  ['the build has no Nexus support', { status: { available: false, enabled: false, locked: true, mesh_up: false, status: 'disabled', members: [] } }],
  ['the mesh is off', { status: { enabled: false, locked: true, mesh_up: false, status: 'disabled', members: [] } }],
  ['the group has no inference devices', { members: [self, peer] }],
  ['the server has no Nexus routes', { refuse: { status: 404, error: 'Not found' } }],
] as [string, Options][]) {
  test(`selector offers Local Server when ${name}`, async ({ page }) => {
    await setup(page, true, false, options);
    const { requests, selector, send } = await chatThroughSelector(page);
    await expect(selector.locator('option')).toHaveText(['Local Server']);
    await expect(selector).toHaveValue('local');
    await send();
    expect(requests[0].url).toBe('http://127.0.0.1:13305/api/v1/chat/completions');
    expect(requests[0].peerKey).toBeUndefined();
  });
}

test('selecting Local Server after a peer restores the local connection and drops the peer key', async ({ page }) => {
  await setup(page, true, false, { members: [self, workstation] });
  const { requests, selector, send } = await chatThroughSelector(page);
  await page.route('**/nexus/peers/workstation/api/v1/health**', route => route.request().headers()['x-lemonade-peer-authorization'] === 'Bearer peer-secret'
    ? route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [] } })
    : route.fulfill({ status: 401, headers, json: { error: 'API key required' } }));
  await selector.selectOption('workstation');
  const dialog = page.getByRole('dialog', { name: 'Server API key', exact: true });
  await dialog.getByLabel('Remote server API key', { exact: true }).fill('peer-secret');
  await dialog.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(selector).toHaveValue('workstation');
  await selector.selectOption('local');
  await expect(selector).toHaveValue('local');
  await expect(selector).toBeEnabled();
  await send();
  expect(requests.at(-1)?.url).toBe('http://127.0.0.1:13305/api/v1/chat/completions');
  expect(requests.at(-1)?.peerKey).toBeUndefined();
});
