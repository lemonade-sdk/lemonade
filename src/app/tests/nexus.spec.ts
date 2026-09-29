import { expect, test, type Page } from '@playwright/test';

const headers = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'Authorization, Content-Type', 'Access-Control-Allow-Methods': 'GET, POST, OPTIONS' };
const self = { node_id: 'self', device_name: 'My laptop', tunnel_ip: '10.8.0.2/32', management_pubkey: 'self-key', permissions: ['add_child', 'delete_node'], can_remove: false };
const peer = { node_id: 'peer', device_name: 'Kitchen PC', tunnel_ip: '10.8.0.3/32', management_pubkey: 'peer-key', permissions: [], can_remove: true };
async function setup(page: Page, desktop = true, locked = false) {
  const state = { available: true, controller_url: 'http://127.0.0.1:15555', enabled: true, registered: true, locked, mesh_up: !locked, status: locked ? 'locked' : 'running', node_id: 'self', group_node_id: 'group', device_name: 'My laptop', tunnel_ip: '10.8.0.2', peer_count: 1, members: locked ? [] : [self, peer] };
  const calls: { path: string; body: unknown; authorization?: string }[] = [];
  await page.addInitScript(({ desktop }) => {
    (window as any).api = {
      isWebApp: !desktop,
      getSettings: async () => ({ baseURL: { value: 'http://127.0.0.1:13305', type: 'string' }, apiKey: { value: 'local-secret', type: 'string' } }),
      saveSettings: async () => true,
      getPlatform: async () => 'darwin',
      getServerBaseUrl: async () => 'http://127.0.0.1:13305',
    };
  }, { desktop });
  await page.route('**/api/v1/**', async route => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (request.method() === 'OPTIONS') return route.fulfill({ status: 204, headers });
    if (path.includes('/nexus/')) {
      calls.push({ path, body: request.postDataJSON(), authorization: request.headers().authorization });
      if (path.endsWith('/remove')) state.members = [self];
      const json = path.endsWith('/egress') ? { endpoint: 'http://127.0.0.1:16666' } : state;
      return route.fulfill({ headers, json });
    }
    if (path.endsWith('/health')) return route.fulfill({ headers, json: { status: 'ok', version: 'test', all_models_loaded: [] } });
    if (path.endsWith('/models')) return route.fulfill({ headers, json: { data: [] } });
    return route.fulfill({ headers, json: {} });
  });
  await page.goto('/#/connect/devices-and-mesh');
  await expect(page.getByRole('heading', { name: 'Devices in your group' })).toBeVisible();
  return { state, calls };
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
  await expect(dialog.getByRole('button', { name: 'Confirm with passkey' })).not.toBeFocused();
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
});

test('remote server key stays separate from local network controls', async ({ page }) => {
  const { calls } = await setup(page);
  await page.getByLabel('Remote server API key (if required)').fill('peer-secret');
  const remoteRequests: string[] = [];
  page.on('request', request => {
    if (request.url().startsWith('http://127.0.0.1:16666/')) remoteRequests.push(request.headers().authorization || '');
  });
  await page.getByRole('button', { name: 'Connect to server', exact: true }).click();
  await expect(page).toHaveURL(/connect\/server/);
  expect(remoteRequests).toContain('Bearer peer-secret');
  expect(calls.find(call => call.path.endsWith('/egress'))?.authorization).toBe('Bearer local-secret');
  await page.getByRole('button', { name: 'Devices and mesh: Mesh connected' }).click();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  expect(calls.filter(call => call.path.endsWith('/status')).at(-1)?.authorization).toBe('Bearer local-secret');
});

test('header server selector switches inference to a mesh device and back to local', async ({ page }) => {
  const { calls } = await setup(page);
  const requests: string[] = [];
  await page.route('**/api/v1/health**', route => {
    const remote = new URL(route.request().url()).port === '16666';
    return route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [{ model_name: remote ? 'MeshModel' : 'LocalModel', labels: ['chat'], recipe: 'llamacpp' }] } });
  });
  await page.route('**/api/v1/models**', route => {
    const remote = new URL(route.request().url()).port === '16666';
    return route.fulfill({ headers, json: { data: [{ id: remote ? 'MeshModel' : 'LocalModel', labels: ['chat'], recipe: 'llamacpp', downloaded: true }] } });
  });
  await page.route('**/api/v1/chat/completions', route => {
    requests.push(route.request().url());
    return route.fulfill({ headers, contentType: 'text/event-stream', body: 'data: {"choices":[{"delta":{"content":"Mesh inference worked."}}]}\n\ndata: [DONE]\n\n' });
  });
  await page.locator('.titlebar__nav').getByRole('button', { name: 'Chat', exact: true }).click();
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('peer');
  await expect(selector).toHaveValue('peer');
  await expect(selector).toBeEnabled();
  await expect(page).toHaveURL(/#\/chat$/);
  await page.getByRole('textbox', { name: 'Message', exact: true }).fill('Say hello');
  await page.getByRole('button', { name: 'Send', exact: true }).click();
  await expect.poll(() => requests.length).toBe(1);
  expect(requests[0]).toBe('http://127.0.0.1:16666/api/v1/chat/completions');
  await expect(page.getByText('Mesh inference worked.', { exact: true })).toBeVisible();
  await selector.selectOption('local');
  await expect(selector).toHaveValue('local');
  await expect(selector).toBeEnabled();
  expect(calls.find(call => call.path.endsWith('/egress'))?.body).toEqual({ node_id: 'peer' });
  expect(await page.evaluate(() => localStorage.getItem('lemonade_base_url'))).not.toContain('16666');
});

test('failed mesh server selection restores the previous inference connection', async ({ page }) => {
  await setup(page);
  await page.route('http://127.0.0.1:16666/api/v1/health**', route => route.fulfill({ status: 503, headers, json: { error: 'Offline' } }));
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('peer');
  await expect(selector).toBeEnabled();
  await expect(selector).toHaveValue('local');
  await expect(page.getByRole('alert')).toBeVisible();
});

test('selector requests a protected peer key and keeps it separate when returning to local', async ({ page }) => {
  const { calls } = await setup(page);
  const remoteKeys: string[] = [];
  await page.route('http://127.0.0.1:16666/api/v1/health**', route => {
    const authorization = route.request().headers().authorization || '';
    remoteKeys.push(authorization);
    return authorization === 'Bearer peer-secret'
      ? route.fulfill({ headers, json: { status: 'ok', all_models_loaded: [] } })
      : route.fulfill({ status: 401, headers, json: { error: 'API key required' } });
  });
  const selector = page.getByRole('combobox', { name: 'Inference server', exact: true });
  await selector.selectOption('peer');
  const dialog = page.getByRole('dialog', { name: 'Server API key', exact: true });
  await expect(dialog).toBeVisible();
  await dialog.getByLabel('Remote server API key', { exact: true }).fill('peer-secret');
  await dialog.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(selector).toHaveValue('peer');
  await selector.selectOption('local');
  await expect(selector).toBeEnabled();
  await expect(selector).toHaveValue('local');
  expect(remoteKeys).toContain('Bearer peer-secret');
  expect(remoteKeys).not.toContain('Bearer local-secret');
  expect(calls.filter(call => call.path.endsWith('/egress')).every(call => call.authorization === 'Bearer local-secret')).toBe(true);
});
