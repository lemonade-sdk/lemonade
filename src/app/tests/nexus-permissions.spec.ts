import { expect, test, type Page } from '@playwright/test';
import { setupNexusAccount } from './nexusAccountFixture';

async function openLocalAccount(page: Page) {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { apiKey: '', deviceKey: true });
  await page.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  await page.getByRole('dialog', { name: 'Enable Mesh' }).getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  await expect(page.getByRole('button', { name: 'User: My laptop', exact: true })).toBeVisible();
  return fixture;
}

async function openPermissions(page: Page) {
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  await page.getByRole('button', { name: 'Permissions', exact: true }).click();
  await page.getByLabel('Device identity', { exact: true }).selectOption('peer');
  await expect(page.getByRole('checkbox', { name: 'Run inference', exact: true })).toBeHidden();
  await page.locator('.nexus-service-permissions > summary').click();
}

test('mesh service permissions save through the local controller without an API key and reload', async ({ page }, testInfo) => {
  const fixture = await openLocalAccount(page);
  fixture.branches.group.parent.assignments[1].permissions = ['read', 'connect_private', 'svc:inference', 'svc:future_service'];
  await openPermissions(page);
  await expect(page.getByRole('checkbox', { name: 'Run inference', exact: true })).toBeChecked();
  await page.getByRole('checkbox', { name: 'Use MCP tools', exact: true }).check();
  await page.getByRole('checkbox', { name: 'Manage models', exact: true }).check();
  await page.screenshot({ path: testInfo.outputPath('mesh-service-permissions.png'), fullPage: true });
  await page.locator('.nexus-service-permissions > summary').click();
  await page.screenshot({ path: testInfo.outputPath('mesh-service-access-collapsed.png'), fullPage: true });
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.getByText('Permissions saved.', { exact: true })).toBeVisible();
  const write = fixture.calls.find(call => call.path.endsWith('/tree/group/permissions'))!;
  expect(write.body).toEqual({ member_node_id: 'peer', admin: false,
    permissions: ['read', 'connect_private', 'svc:inference', 'svc:future_service', 'svc:mcp', 'svc:models_manage'] });
  expect(write.origin).toBe('http://127.0.0.1:15555');
  expect(write.authorization).toBeUndefined();
  await openPermissions(page);
  await expect(page.getByRole('checkbox', { name: 'Use MCP tools', exact: true })).toBeChecked();
  await page.getByRole('checkbox', { name: 'Run inference', exact: true }).uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.getByText('Permissions saved.', { exact: true })).toBeVisible();
  expect(fixture.calls.filter(call => call.path.endsWith('/permissions')).at(-1)?.body?.permissions).not.toContain('svc:inference');
});

test('administrator access includes all service families and denied writes retain the draft', async ({ page }, testInfo) => {
  const fixture = await openLocalAccount(page);
  await openPermissions(page);
  await page.getByRole('checkbox', { name: 'Group administrator', exact: true }).check();
  for (const checkbox of await page.locator('.nexus-service-permissions').getByRole('checkbox').all()) {
    await expect(checkbox).toBeChecked();
    await expect(checkbox).toBeDisabled();
  }
  await page.screenshot({ path: testInfo.outputPath('mesh-administrator-permissions.png'), fullPage: true });
  fixture.rejectWrites = true;
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.locator('.nexus-group-editor').getByRole('alert')).toContainText('cannot be changed');
  await expect(page.getByRole('checkbox', { name: 'Group administrator', exact: true })).toBeChecked();
  await expect(page.getByText('Permissions saved.', { exact: true })).toHaveCount(0);
});

test('moderator invitations delegate their preset to the server and display denials', async ({ page }, testInfo) => {
  const fixture = await openLocalAccount(page);
  await page.locator('.nexus-device-disclosure > summary').click();
  await page.getByRole('button', { name: 'Connect another device', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Connect another device', exact: true });
  await dialog.getByRole('textbox', { name: 'Device you are inviting', exact: true }).fill('Lab workstation');
  await dialog.getByLabel('Invitation access', { exact: true }).selectOption('moderator');
  await expect(dialog).toContainText('Administrator access can only be granted after joining.');
  await page.screenshot({ path: testInfo.outputPath('mesh-moderator-invitation.png') });
  fixture.rejectWrites = true;
  await dialog.getByRole('button', { name: 'Create invitation', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('Nexus refused');
  await expect(dialog.getByLabel('Invitation access', { exact: true })).toHaveValue('moderator');
  await expect(dialog.getByLabel('Invitation code', { exact: true })).toHaveCount(0);
  fixture.rejectWrites = false;
  await dialog.getByRole('button', { name: 'Create invitation', exact: true }).click();
  await expect(dialog.getByLabel('Invitation code', { exact: true })).toBeVisible();
  const write = fixture.calls.filter(call => call.path.endsWith('/invites')).at(-1)!;
  expect(write.body).toEqual({ device_name: 'Lab workstation', ttl_sec: 600, role: 'moderator' });
  expect(write.authorization).toBeUndefined();
});
