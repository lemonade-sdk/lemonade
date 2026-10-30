import { expect, test } from '@playwright/test';
import { enableNexusMesh, setupNexusAccount } from './nexusAccountFixture';

test('connection labels follow live path changes and stay hidden for unverified devices', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { deviceKey: true });
  const direct = fixture.state.members[1];
  direct.connection_type = 'p2p';
  const treePeer = fixture.branches.group.children.find(child => child.id === direct.node_id)!;
  treePeer.tunnel_ip = direct.tunnel_ip;
  for (const [id, name, online, connection] of [
    ['relay-peer', 'Studio server', true, 'relay'],
    ['unknown-peer', 'Office laptop', true, undefined],
    ['offline-peer', 'Travel laptop', false, 'relay'],
    ['pending-peer', 'Guest laptop', undefined, undefined],
  ] as const) {
    fixture.state.members.push({ ...direct, node_id: id, device_name: name, is_online: online, connection_type: connection });
    fixture.branches.group.children.push({ ...treePeer, id, name });
  }
  await enableNexusMesh(page);
  await page.locator('.nexus-device-disclosure > summary').click();
  const compact = page.locator('.nexus-page .nexus-devices');
  await expect(compact.locator('li').filter({ hasText: 'Kitchen PC' }).locator('.nexus-connection-badge')).toHaveText('P2P');
  await expect(compact.locator('li').filter({ hasText: 'Studio server' }).locator('.nexus-connection-badge')).toHaveText('Relay');
  for (const name of ['My laptop', 'Office laptop', 'Travel laptop', 'Guest laptop']) {
    await expect(compact.locator('li').filter({ hasText: name }).locator('.nexus-connection-badge')).toHaveCount(0);
  }
  for (const name of ['My laptop', 'Kitchen PC', 'Studio server', 'Office laptop']) {
    await expect(compact.locator('li').filter({ hasText: name }).locator('.nexus-device-status')).toHaveText('Online');
  }
  await expect(compact.locator('li').filter({ hasText: 'Travel laptop' }).locator('.nexus-device-status')).toHaveText('Offline');
  await expect(compact.locator('li').filter({ hasText: 'Guest laptop' }).locator('.nexus-device-status')).toHaveText('Status unknown');
  await page.locator('.nexus-page').screenshot({ path: testInfo.outputPath('nexus-connection-compact.png') });
  await page.getByRole('button', { name: 'User: My laptop', exact: true }).click();
  const menu = page.getByRole('dialog', { name: 'Lemonade Account', exact: true });
  await menu.locator('.nexus-device-disclosure > summary').click();
  await expect(menu.locator('.nexus-devices li').filter({ hasText: 'Kitchen PC' }).locator('.nexus-connection-badge')).toHaveText('P2P');
  await expect(menu.locator('.nexus-devices li').filter({ hasText: 'Kitchen PC' }).locator('.nexus-device-status')).toHaveText('Online');
  await expect(menu.locator('.nexus-devices li').filter({ hasText: 'Travel laptop' }).locator('.nexus-device-status')).toHaveText('Offline');
  await menu.getByRole('button', { name: 'Close account menu' }).click();
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  const row = page.getByRole('treeitem', { name: 'Kitchen PC', exact: true });
  await row.click();
  const detail = page.locator('.nexus-tree-detail');
  await expect(row.locator('.nexus-connection-badge')).toHaveText('P2P');
  await expect(detail.locator('.nexus-connection-badge')).toHaveText('P2P');
  await expect(row.locator('.nexus-device-status')).toHaveText('Online');
  await expect(page.getByRole('treeitem', { name: 'Travel laptop', exact: true }).locator('.nexus-device-status')).toHaveText('Offline');
  await expect(page.getByRole('treeitem', { name: 'Guest laptop', exact: true }).locator('.nexus-device-status')).toHaveText('Status unknown');
  await expect(page.getByRole('treeitem', { name: 'Home', exact: true }).locator('.nexus-device-status')).toHaveCount(0);
  await page.locator('.nexus-hierarchy').screenshot({ path: testInfo.outputPath('nexus-connection-advanced.png') });
  direct.connection_type = 'relay';
  await expect(row.locator('.nexus-connection-badge')).toHaveText('Relay', { timeout: 15000 });
  await expect(detail.locator('.nexus-connection-badge')).toHaveText('Relay');
  direct.is_online = false;
  await expect(row.locator('.nexus-connection-badge')).toHaveCount(0, { timeout: 15000 });
  await expect(row.locator('.nexus-device-status')).toHaveText('Offline');
  await expect(detail.locator('.nexus-device-status')).toHaveText('Offline');
  await expect(detail.locator('.nexus-connection-badge')).toHaveCount(0);
  await expect(detail.getByText('Not connected', { exact: true }).first()).toBeVisible();
  direct.is_online = true;
  direct.connection_type = undefined;
  await expect(detail.getByText('Unknown', { exact: true })).toBeVisible({ timeout: 15000 });
  await expect(row.locator('.nexus-device-status')).toHaveText('Online');
  await expect(row.locator('.nexus-connection-badge')).toHaveCount(0);
  await page.setViewportSize({ width: 720, height: 1000 });
  await page.getByRole('radio', { name: 'Basic', exact: true }).check();
  await page.locator('.nexus-device-disclosure > summary').click();
  const relayBadge = compact.locator('li').filter({ hasText: 'Studio server' }).locator('.nexus-connection-badge');
  await expect(relayBadge).toBeVisible();
  const bounds = await relayBadge.boundingBox();
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(720);
  await page.locator('.nexus-page').screenshot({ path: testInfo.outputPath('nexus-connection-narrow.png') });
});
