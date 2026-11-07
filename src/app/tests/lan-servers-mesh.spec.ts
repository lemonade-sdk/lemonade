import { test, expect, type Page } from '@playwright/test';
import { setupNexusAccount } from './nexusAccountFixture';

// #1221 on #3930: servers found on this network, under Devices & mesh, with Invite next to Use directly.
// The account side is the Nexus fixture; the found servers come from a fake desktop beacon list, and the
// found server's own lemond is routed separately.

const FOUND = [
  { id: 'cesarops2:13305', hostname: 'cesarops2', urls: ['http://192.168.4.24:13305/api/v1/'], zone: 'home', seen: true, secondsSinceSeen: 1, state: 'new' },
  { id: 'kitchen pc:13305', hostname: 'Kitchen PC', urls: ['http://192.168.4.30:13305/api/v1/'], zone: 'home', seen: true, secondsSinceSeen: 1, state: 'new' },
  { id: 'old-box:13305', hostname: 'old-box', urls: ['http://192.168.4.40:13305/api/v1/'], zone: 'home', seen: true, secondsSinceSeen: 2, state: 'new' },
];

async function withFoundServers(page: Page) {
  // The Nexus fixture installs its own window.api; merge the beacon list into whatever is assigned.
  await page.addInitScript(rows => {
    const lan = {
      listLanServers: async () => rows.map(r => ({ ...r })),
      setLanServerState: async () => undefined,
      onLanServersUpdated: () => () => undefined,
    };
    let current: Record<string, unknown> = { isWebApp: true, ...lan };
    Object.defineProperty(window, 'api', {
      configurable: true,
      get: () => current,
      set: value => { current = { ...(value as Record<string, unknown>), ...lan }; },
    });
  }, FOUND);
}

type Call = { path: string; body: unknown };

// A found lemond: `fresh` is a device-key server that has never joined; `plain` has no Nexus at all.
async function routeFound(page: Page, origin: string, kind: 'fresh' | 'plain') {
  const calls: Call[] = [];
  await page.route(`${origin}/**`, async route => {
    const url = new URL(route.request().url());
    const body = route.request().postDataJSON?.() ?? null;
    calls.push({ path: url.pathname, body });
    if (url.pathname === '/api/v1/health') return route.fulfill({ json: { status: 'ok' } });
    if (kind === 'plain') return route.fulfill({ status: 404, json: { error: 'not found' } });
    const path = url.pathname.replace(/^\/api\/v1\/nexus\//, '');
    if (path === 'status') return route.fulfill({ json: { available: true, enabled: true, registered: false, unlock: 'device_key', locked: true, mesh_up: false, status: 'locked', members: [] } });
    return route.fulfill({ json: { ok: true } });
  });
  return calls;
}

test('Invite joins a found server into this network from here', async ({ page }) => {
  await withFoundServers(page);
  const fixture = await setupNexusAccount(page, { deviceKey: true });
  const found = await routeFound(page, 'http://192.168.4.24:13305', 'fresh');
  const panel = page.getByRole('region', { name: 'Found on this network' });
  await expect(panel.getByText('cesarops2', { exact: true })).toBeVisible();
  // A server already in the network is marked, not offered an invite.
  const kitchen = panel.getByRole('group', { name: 'Actions for Kitchen PC' });
  await expect(panel.getByText('In your network')).toBeVisible();
  await expect(kitchen.getByRole('button', { name: 'Invite' })).toHaveCount(0);

  await panel.getByRole('group', { name: 'Actions for cesarops2' }).getByRole('button', { name: 'Invite' }).click();
  await expect(panel.getByText('cesarops2 joined your network.')).toBeVisible();
  const invite = fixture.calls.find(call => call.path.endsWith('/invites'));
  expect(invite?.body).toMatchObject({ device_name: 'cesarops2' });
  const nexus = found.filter(call => call.path.startsWith('/api/v1/nexus/')).map(call => call.path.replace('/api/v1/nexus/', ''));
  expect(nexus).toEqual(['status', 'enable', 'status', 'device', 'key/unlock', 'join']);
  expect(found.find(call => call.path.endsWith('/join'))?.body).toEqual({ code: '12345678' });
  expect(found.find(call => call.path.endsWith('/key/unlock'))?.body).toEqual({ defer_join: true });
});

test('a found server without network support keeps the code for entering there', async ({ page }) => {
  await withFoundServers(page);
  await setupNexusAccount(page, { deviceKey: true });
  await routeFound(page, 'http://192.168.4.40:13305', 'plain');
  const panel = page.getByRole('region', { name: 'Found on this network' });
  await panel.getByRole('group', { name: 'Actions for old-box' }).getByRole('button', { name: 'Invite' }).click();
  await expect(panel.getByRole('alert')).toContainText('old-box did not join');
  await expect(panel.getByRole('alert')).toContainText('Enter code 12345678 on old-box instead');
});

test('a member who cannot invite sees Use directly but no Invite', async ({ page }) => {
  await withFoundServers(page);
  await setupNexusAccount(page, { deviceKey: true, readOnly: true });
  const panel = page.getByRole('region', { name: 'Found on this network' });
  const row = panel.getByRole('group', { name: 'Actions for cesarops2' });
  await expect(row.getByRole('button', { name: 'Use directly' })).toBeVisible();
  await expect(row.getByRole('button', { name: 'Invite' })).toHaveCount(0);
});
