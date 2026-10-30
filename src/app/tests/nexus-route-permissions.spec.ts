import { expect, test, type Page } from '@playwright/test';
import type { NexusRole, NexusRoutePermissions } from '../src/features/nexus/routePermissions';
import { enableNexusMesh, setupNexusAccount } from './nexusAccountFixture';

function policy(): NexusRoutePermissions {
  return {
    catalog_version: 202604600302, route_policy: 'enforced',
    roles: [
      { id: 'admin', permissions: ['read', 'connect_private', 'admin'] },
      { id: 'moderator', permissions: ['read', 'write', 'connect_private', 'svc:inference', 'svc:mcp', 'svc:models_manage', 'svc:jobs'] },
      { id: 'user', permissions: ['read', 'connect_private', 'svc:inference'] },
    ],
    routes: [
      { family: 'inference', method: 'POST', paths: ['/api/v0/chat/completions', '/api/v1/chat/completions', '/v0/chat/completions', '/v1/chat/completions'], roles: ['admin', 'moderator', 'user'] },
      { family: 'inference', method: 'GET', paths: ['/api/v1/models/{id}'], roles: ['admin', 'moderator', 'user'] },
      { family: 'mcp', method: 'POST', paths: ['/mcp'], roles: ['admin', 'moderator'] },
      { family: 'models_manage', method: 'POST', paths: ['/api/v1/pull'], roles: ['admin', 'moderator'] },
      { family: 'jobs', method: 'GET', paths: ['/api/v1/jobs'], roles: ['admin', 'moderator'] },
      { family: 'backends', method: 'POST', paths: ['/api/v1/install'], roles: ['admin'] },
      { family: 'system', method: 'GET', paths: ['/metrics'], roles: ['admin'] },
      { family: 'management', method: 'POST', paths: ['/api/v1/nexus/members/{node_id}/role'], roles: ['admin'] },
    ],
  };
}

async function openRoutes(page: Page, role: NexusRole = 'admin') {
  const fixture = await setupNexusAccount(page, { apiKey: '', deviceKey: true, role });
  const response = { status: 200, body: policy() as unknown, calls: [] as { method: string; origin: string; authorization?: string }[] };
  await page.route('**/nexus/route-permissions', route => {
    response.calls.push({ method: route.request().method(), origin: new URL(route.request().url()).origin, authorization: route.request().headers().authorization });
    return route.fulfill({ status: response.status, headers: { 'Access-Control-Allow-Origin': '*' }, json: response.body });
  });
  await enableNexusMesh(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  expect(response.calls).toHaveLength(0);
  await page.locator('.nexus-route-permissions > summary').click();
  return { response, fixture };
}

test('role route lists use server data and preserve templates and aliases without writing permissions', async ({ page }, testInfo) => {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  const { response } = await openRoutes(page);
  const panel = page.locator('.nexus-route-permissions');
  await expect(panel.getByRole('radio', { name: 'User', exact: true })).toBeChecked();
  await expect(panel.getByText('2 allowed routes', { exact: true })).toBeVisible();
  await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
  await panel.getByText('Run inference', { exact: true }).click();
  await expect(panel.locator('code', { hasText: '/api/v1/models/{id}' })).toBeVisible();
  await expect(panel.getByText('Use MCP tools', { exact: true })).toHaveCount(0);
  await panel.locator('.nexus-route-aliases > summary').click();
  await expect(panel.getByText('/v1/chat/completions', { exact: true })).toBeVisible();
  await panel.screenshot({ path: testInfo.outputPath('routes-user.png') });
  await panel.getByRole('radio', { name: 'Moderator', exact: true }).check();
  await expect(panel.getByText('5 allowed routes', { exact: true })).toBeVisible();
  await expect(panel.getByText('Use MCP tools', { exact: true })).toBeVisible();
  await expect(panel.getByText('Manage models', { exact: true })).toBeVisible();
  await expect(panel.getByText('Manage jobs', { exact: true })).toBeVisible();
  await expect(panel.getByText('Manage backends', { exact: true })).toHaveCount(0);
  await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
  await panel.screenshot({ path: testInfo.outputPath('routes-moderator.png') });
  await panel.getByText('Use MCP tools', { exact: true }).click();
  await expect(panel.getByText('/mcp', { exact: true })).toBeVisible();
  await panel.getByRole('radio', { name: 'Admin', exact: true }).check();
  await expect(panel.getByText('8 allowed routes', { exact: true })).toBeVisible();
  await expect(panel.getByText('Manage backends', { exact: true })).toBeVisible();
  await expect(panel.getByText('Access system information', { exact: true })).toBeVisible();
  await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
  await panel.getByText('Manage Nexus', { exact: true }).click();
  await panel.getByRole('radio', { name: 'User', exact: true }).check();
  await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
  await panel.getByRole('radio', { name: 'Admin', exact: true }).check();
  await panel.getByLabel('Filter routes', { exact: true }).fill('nexus');
  await expect(panel.getByText('/api/v1/nexus/members/{node_id}/role', { exact: true })).toBeVisible();
  await expect(panel).toContainText('Group actions also check current group assignments.');
  await panel.locator('.nexus-route-family > summary').click();
  await expect(panel.getByText('/api/v1/nexus/members/{node_id}/role', { exact: true })).toBeHidden();
  await panel.getByLabel('Filter routes', { exact: true }).fill('members/{node_id}/role');
  await expect(panel.getByText('/api/v1/nexus/members/{node_id}/role', { exact: true })).toBeVisible();
  await panel.screenshot({ path: testInfo.outputPath('routes-admin.png') });
  await panel.getByLabel('Filter routes', { exact: true }).fill('missing-route');
  await expect(panel.getByText('No routes match your search.', { exact: true })).toBeVisible();
  expect(response.calls).toEqual([{ method: 'GET', origin: 'http://127.0.0.1:15555', authorization: undefined }]);
  expect(errors).toEqual([]);
});

test('missing, malformed and unavailable policies never display guessed permissions and can retry', async ({ page }) => {
  const { response } = await openRoutes(page);
  const panel = page.locator('.nexus-route-permissions');
  await expect(panel.getByText('2 allowed routes', { exact: true })).toBeVisible();
  await panel.locator(':scope > summary').click();
  response.status = 404; response.body = { error: 'Not found' };
  await panel.locator(':scope > summary').click();
  await expect(panel.getByRole('alert')).toContainText('Update this server to view route permissions.');
  await expect(panel.getByRole('radio')).toHaveCount(0);
  response.status = 200; response.body = {};
  await panel.getByRole('button', { name: 'Retry', exact: true }).click();
  await expect(panel.getByRole('alert')).toContainText('invalid route permission list');
  response.body = { ...policy(), route_policy: 'unavailable' };
  await panel.getByRole('button', { name: 'Retry', exact: true }).click();
  await expect(panel.getByRole('alert')).toContainText('cannot verify its route policy');
  await expect(panel.getByRole('table')).toHaveCount(0);
  response.status = 403; response.body = { error: 'Nexus administration requires an administrator' };
  await panel.getByRole('button', { name: 'Retry', exact: true }).click();
  await expect(panel.getByRole('alert')).toContainText('requires an administrator');
  response.status = 200; response.body = policy();
  await panel.getByRole('button', { name: 'Retry', exact: true }).click();
  await expect(panel.getByText('2 allowed routes', { exact: true })).toBeVisible();
});

test('route permissions fit a narrow viewport and support keyboard role selection', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openRoutes(page);
  const panel = page.locator('.nexus-route-permissions');
  await expect(panel.getByRole('radio', { name: 'User', exact: true })).toBeChecked();
  await panel.getByRole('radio', { name: 'Admin', exact: true }).focus();
  await page.keyboard.press('Space');
  await expect(panel.getByRole('radio', { name: 'Admin', exact: true })).toBeChecked();
  await panel.getByLabel('Filter routes', { exact: true }).fill('nexus');
  await expect(panel.getByText('/api/v1/nexus/members/{node_id}/role', { exact: true })).toBeVisible();
  expect(await panel.evaluate(element => element.scrollWidth <= element.clientWidth + 1)).toBe(true);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  const labels = await panel.getByRole('radio').locator('..').all();
  const boxes = await Promise.all(labels.map(label => label.boundingBox()));
  expect(new Set(boxes.map(box => box?.y)).size).toBe(1);
  await panel.screenshot({ path: testInfo.outputPath('routes-mobile.png') });
});

for (const role of ['admin', 'moderator', 'user'] as const) {
  test(`${role} sees only its own and lower role controls`, async ({ page }, testInfo) => {
    await openRoutes(page, role);
    const panel = page.locator('.nexus-route-permissions');
    const labels = role === 'admin' ? ['Admin', 'Moderator', 'User'] : role === 'moderator' ? ['Moderator', 'User'] : ['User'];
    await expect(panel.getByRole('radio')).toHaveCount(labels.length);
    for (const label of ['Admin', 'Moderator', 'User']) {
      await expect(panel.getByRole('radio', { name: label, exact: true })).toHaveCount(labels.includes(label) ? 1 : 0);
    }
    await panel.getByRole('radio', { name: labels[0], exact: true }).check();
    await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
    await panel.getByText('Run inference', { exact: true }).click();
    await panel.screenshot({ path: testInfo.outputPath(`visible-controls-${role}.png`) });
  });
}

test('account refresh removes a selected higher role without leaving stale controls', async ({ page }) => {
  await page.clock.install();
  const { fixture } = await openRoutes(page);
  const panel = page.locator('.nexus-route-permissions');
  await panel.getByRole('radio', { name: 'Admin', exact: true }).check();
  await panel.getByText('Manage Nexus', { exact: true }).click();
  fixture.accountRole = 'moderator';
  fixture.state.members[0].is_admin = false;
  fixture.state.members[0].permissions = ['read', 'write', 'add_child', 'connect_private', 'svc:inference', 'svc:mcp', 'svc:models_manage', 'svc:jobs'];
  await page.clock.fastForward(15001);
  await expect(panel.getByRole('radio', { name: 'Admin', exact: true })).toHaveCount(0);
  await expect(panel.getByRole('radio', { name: 'Moderator', exact: true })).toBeChecked();
  await expect(panel.locator('.nexus-route-family[open]')).toHaveCount(0);
  fixture.accountRole = 'user';
  fixture.state.members[0].permissions = ['read', 'connect_private', 'svc:inference'];
  await page.clock.fastForward(15001);
  await expect(panel.getByRole('radio')).toHaveCount(1);
  await expect(panel.getByRole('radio', { name: 'User', exact: true })).toBeChecked();
  fixture.accountError = true;
  await page.clock.fastForward(15001);
  await expect(panel).toHaveCount(0);
});
