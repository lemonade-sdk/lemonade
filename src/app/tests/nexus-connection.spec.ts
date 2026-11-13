import { expect, test } from '@playwright/test';
import { resumedNexusMesh, setupNexusAccount } from './nexusAccountFixture';

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
  await page.reload();
  await resumedNexusMesh(page);
  const compact = page.locator('.nexus-page .nexus-devices');
  const deviceRow = (name: string) => compact.locator('li').filter({ hasText: name });
  await expect(deviceRow('Kitchen PC').locator('.nexus-pill--connection')).toHaveText('P2P');
  await expect(deviceRow('Studio server').locator('.nexus-pill--connection')).toHaveText('Relay');
  await expect(deviceRow('Office laptop').locator('.nexus-pill--connection')).toHaveText('Connecting', { ignoreCase: true });
  await expect(deviceRow('Travel laptop').locator('.nexus-pill--connection')).toHaveText('Offline');
  for (const name of ['My laptop', 'Guest laptop']) await expect(deviceRow(name).locator('.nexus-pill--connection')).toHaveCount(0);
  await expect(deviceRow('My laptop').locator('.nexus-pill').first()).toHaveText('This device');
  await expect(page.locator('.nexus-mesh__state')).toHaveText('No servers yet');
  await page.locator('.nexus-page').screenshot({ path: testInfo.outputPath('nexus-connection-compact.png') });
  await page.getByRole('button', { name: 'Mesh: My laptop', exact: true }).click();
  const menu = page.getByRole('dialog', { name: 'Lemonade Account', exact: true });
  await expect(menu.locator('.nexus-device-disclosure')).toHaveAttribute('open', '');
  await expect(menu.locator('.nexus-devices li').filter({ hasText: 'Kitchen PC' }).locator('.nexus-pill--connection')).toHaveText('P2P');
  await expect(menu.locator('.nexus-devices li').filter({ hasText: 'Travel laptop' }).locator('.nexus-pill--connection')).toHaveText('Offline');
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
  const relayBadge = compact.locator('li').filter({ hasText: 'Studio server' }).locator('.nexus-pill--connection');
  await expect(relayBadge).toBeVisible();
  const bounds = await relayBadge.boundingBox();
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(720);
  await page.locator('.nexus-page').screenshot({ path: testInfo.outputPath('nexus-connection-narrow.png') });
});
