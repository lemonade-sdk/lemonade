import { expect, test, type Page } from '@playwright/test';
import { setupNexusAccount } from './nexusAccountFixture';

async function openAdvancedTree(page: Page) {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { apiKey: '', deviceKey: true });
  await page.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  await page.getByRole('dialog', { name: 'Enable Mesh' }).getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  await expect(page.getByRole('button', { name: 'User: My laptop', exact: true })).toBeVisible();
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  return fixture;
}

test('a device takes a role, its access list is edited and it moves into another group', async ({ page }, testInfo) => {
  const fixture = await openAdvancedTree(page);
  fixture.state.members.push({ node_id: 'tablet', device_name: 'Tablet', tunnel_ip: '10.8.0.4', management_pubkey: 'tablet-key',
    permissions: ['read'], permissions_known: true, is_admin: false, is_online: true, can_remove: true });
  await page.getByRole('treeitem', { name: 'Kitchen PC' }).click();

  await page.getByLabel('Role in this group', { exact: true }).selectOption('moderator');
  await expect(page.getByText('Role saved.', { exact: true })).toBeVisible();
  const role = fixture.calls.find(call => call.path.endsWith('/members/peer/role'))!;
  expect(role.body).toEqual({ role: 'moderator' });
  expect(role.authorization).toBeUndefined();

  const connect = page.getByRole('checkbox', { name: 'Tablet can connect', exact: true });
  await expect(connect).toBeChecked();
  await connect.uncheck();
  await expect(page.getByText('Access saved.', { exact: true })).toBeVisible();
  expect(fixture.calls.find(call => call.path.endsWith('/devices/peer/access') && call.method === 'POST')!.body)
    .toEqual({ member_node_id: 'tablet', see: true, connect: false });
  await expect(connect).not.toBeChecked();
  await expect(page.getByRole('checkbox', { name: 'Tablet can see', exact: true })).toBeChecked();
  await page.screenshot({ path: testInfo.outputPath('device-management.png'), fullPage: true });

  await page.getByRole('button', { name: 'Move', exact: true }).click();
  await page.getByLabel('Destination group', { exact: true }).selectOption('team');
  await page.getByRole('button', { name: 'Move device', exact: true }).click();
  await expect(page.getByText('Device moved.', { exact: true })).toBeVisible();
  expect(fixture.calls.find(call => call.path.endsWith('/tree/peer/move'))!.body).toEqual({ parent_id: 'team' });
  await expect(page.getByRole('treeitem', { name: 'Team', exact: true })).toHaveAttribute('aria-expanded', 'true');
  expect(fixture.branches.team.children.some(child => child.id === 'peer')).toBe(true);
});

test('a group below the top level moves into another group and the top-level group cannot', async ({ page }) => {
  const fixture = await openAdvancedTree(page);
  const team = page.getByRole('treeitem', { name: 'Team', exact: true });
  await team.click();
  await team.press('ArrowRight');
  await page.getByRole('treeitem', { name: 'Lab', exact: true }).click();
  await page.getByRole('button', { name: 'Move', exact: true }).click();
  const destination = page.getByLabel('Destination group', { exact: true });
  await expect(destination.locator('option')).toHaveText(['Choose a group', 'Home']);
  await destination.selectOption('group');
  await page.getByRole('button', { name: 'Move group', exact: true }).click();
  await expect(page.getByText('Group moved.', { exact: true })).toBeVisible();
  expect(fixture.calls.find(call => call.path.endsWith('/tree/lab/move'))!.body).toEqual({ parent_id: 'group' });
  await page.getByRole('treeitem', { name: 'Home', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Move', exact: true })).toHaveCount(0);
});

test('a refused move keeps the form and shows the server reason', async ({ page }) => {
  const fixture = await openAdvancedTree(page);
  await page.getByRole('treeitem', { name: 'Kitchen PC' }).click();
  fixture.rejectWrites = true;
  await page.getByRole('button', { name: 'Move', exact: true }).click();
  await page.getByLabel('Destination group', { exact: true }).selectOption('team');
  await page.getByRole('button', { name: 'Move device', exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('You do not have permission to add to that group');
  await expect(page.getByLabel('Destination group', { exact: true })).toHaveValue('team');
});
