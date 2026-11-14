import { test, expect, type Page } from '@playwright/test';

// #1221: Lemonade servers found on the local network. The desktop app's beacon
// listener is replaced by a fake window.api holding the list, so this runs in the
// plain renderer like the other specs.

type Row = {
  id: string;
  hostname: string;
  urls: string[];
  zone: 'home' | 'tailscale' | 'away' | null;
  seen: boolean;
  secondsSinceSeen: number | null;
  state: 'new' | 'accepted' | 'ignored';
};

const ROWS: Row[] = [
  { id: 'cesarops4om', hostname: 'cesarops4om', urls: ['http://10.44.0.14:13305/api/v1/', 'http://192.168.7.214:13305/api/v1/'], zone: 'home', seen: true, secondsSinceSeen: 1, state: 'new' },
  { id: 'laptop', hostname: 'laptop', urls: ['http://100.76.125.86:13305/api/v1/'], zone: 'tailscale', seen: true, secondsSinceSeen: 3, state: 'new' },
  { id: 'guest-box', hostname: 'guest-box', urls: ['http://192.168.4.99:13305/api/v1/'], zone: 'home', seen: true, secondsSinceSeen: 2, state: 'ignored' },
];

async function setup(page: Page) {
  await page.addInitScript((rows: Row[]) => {
    localStorage.setItem('lemonade:mcp_enabled', 'false');
    const state = { rows: rows.map(r => ({ ...r })), calls: [] as unknown[] };
    (window as unknown as { __lan: typeof state }).__lan = state;
    (window as unknown as { api: unknown }).api = {
      isWebApp: true,
      listLanServers: async () => state.rows.map(r => ({ ...r })),
      setLanServerState: async (id: string, s: Row['state'], url?: string) => {
        state.calls.push({ id, state: s, url: url ?? null });
        const row = state.rows.find(r => r.id === id);
        if (row) row.state = s;
      },
      onLanServersUpdated: () => () => undefined,
    };
  }, ROWS);
  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({ status: 'ok', version: 'test', model_loaded: null, all_models_loaded: [] }),
  }));
  await page.route(/\/api\/v1\/models(?:\?.*)?$/, route => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({ object: 'list', data: [] }),
  }));
  await page.goto('/#/connect/server');
  await page.waitForSelector('[data-view="connect"]');
}

const list = (page: Page) => page.getByRole('region', { name: 'Found on this network' });

test('lists LAN servers with zones and hides ignored ones', async ({ page }) => {
  await setup(page);
  const panel = list(page);
  await expect(panel.getByText('cesarops4om', { exact: true })).toBeVisible();
  await expect(panel.getByText('http://10.44.0.14:13305 (+1 more)')).toBeVisible();
  await expect(panel.getByText('Home network').first()).toBeVisible();
  await expect(panel.getByText('Tailscale', { exact: true })).toBeVisible();
  await expect(panel.getByText('guest-box', { exact: true })).toHaveCount(0);

  await panel.getByRole('button', { name: 'Show ignored (1)' }).click();
  await expect(panel.getByText('guest-box', { exact: true })).toBeVisible();
  await panel.getByRole('button', { name: 'Stop ignoring' }).click();
  await expect(panel.getByRole('button', { name: /Show ignored/ })).toHaveCount(0);
  const calls = await page.evaluate(() => (window as unknown as { __lan: { calls: unknown[] } }).__lan.calls);
  expect(calls).toEqual([{ id: 'guest-box', state: 'new', url: null }]);
});

test('Ignore hides a server and remembers the choice', async ({ page }) => {
  await setup(page);
  const panel = list(page);
  await panel.getByRole('group', { name: 'Actions for laptop' }).getByRole('button', { name: 'Ignore' }).click();
  await expect(panel.getByText('laptop', { exact: true })).toHaveCount(0);
  await expect(panel.getByRole('button', { name: 'Show ignored (2)' })).toBeVisible();
  const calls = await page.evaluate(() => (window as unknown as { __lan: { calls: unknown[] } }).__lan.calls);
  expect(calls).toEqual([{ id: 'laptop', state: 'ignored', url: null }]);
});

test('Use connects through the normal Connect flow and marks the server', async ({ page }) => {
  await setup(page);
  const panel = list(page);
  await panel.getByRole('group', { name: 'Actions for cesarops4om' }).getByRole('button', { name: 'Use' }).click();
  await expect(page.locator('#host-input')).toHaveValue('http://10.44.0.14:13305');
  await expect(page.getByText('Connected to http://10.44.0.14:13305.')).toBeVisible();
  await expect(panel.getByText('In use')).toBeVisible();
  const calls = await page.evaluate(() => (window as unknown as { __lan: { calls: unknown[] } }).__lan.calls);
  expect(calls).toEqual([{ id: 'cesarops4om', state: 'accepted', url: 'http://10.44.0.14:13305' }]);
});

test('the web app (no beacon listener) shows no LAN list', async ({ page }) => {
  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => route.fulfill({ contentType: 'application/json', body: '{"status":"ok"}' }));
  await page.goto('/#/connect/server');
  await page.waitForSelector('[data-view="connect"]');
  await expect(page.locator('#host-input')).toBeVisible();
  await expect(page.getByRole('region', { name: 'Found on this network' })).toHaveCount(0);
});

test('Use skips addresses that do not answer from this device', async ({ page }) => {
  await setup(page);
  // The first announced address is refused, as a point-to-point or firewalled interface would be.
  await page.route('http://10.44.0.14:13305/api/v1/health', route => route.abort('connectionrefused'));
  const panel = list(page);
  await panel.getByRole('group', { name: 'Actions for cesarops4om' }).getByRole('button', { name: 'Use' }).click();
  await expect(page.locator('#host-input')).toHaveValue('http://192.168.7.214:13305');
  await expect.poll(() => page.evaluate(() => (window as unknown as { __lan: { calls: unknown[] } }).__lan.calls))
    .toEqual([{ id: 'cesarops4om', state: 'accepted', url: 'http://192.168.7.214:13305' }]);
});

test('Use reports a server none of whose addresses answer, and changes nothing', async ({ page }) => {
  await setup(page);
  await page.route(/:\/\/100\.76\.125\.86:13305\/api\/v1\/health/, route => route.abort('connectionrefused'));
  const panel = list(page);
  await panel.getByRole('group', { name: 'Actions for laptop' }).getByRole('button', { name: 'Use' }).click();
  await expect(panel.getByText('None of the addresses laptop announces answered from this device.')).toBeVisible();
  await expect(page.locator('#host-input')).not.toHaveValue('http://100.76.125.86:13305');
  const calls = await page.evaluate(() => (window as unknown as { __lan: { calls: unknown[] } }).__lan.calls);
  expect(calls).toEqual([]);
});
