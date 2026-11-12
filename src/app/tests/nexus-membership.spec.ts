import { expect, test } from '@playwright/test';
import { enableNexusMesh, setupNexusAccount } from './nexusAccountFixture';

test('a device with its own network joins another one as a member and can leave it', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await enableNexusMesh(page);
  const panel = page.locator('.nexus-mesh');
  await expect(panel.getByText('Networks', { exact: true })).toHaveCount(0);
  await panel.getByRole('button', { name: 'Join another network', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Join another network' });
  const submit = dialog.getByRole('button', { name: 'Join network', exact: true });
  await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('1234567');
  await expect(submit).toBeDisabled();
  await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('12345678');
  await submit.click();
  await expect(dialog).toContainText('joined the invited network as a member');
  expect(fixture.calls.find(call => call.path.endsWith('/invites/accept'))?.body).toEqual({ code: '12345678' });
  expect(fixture.calls.some(call => call.path.endsWith('/join'))).toBe(false);
  await dialog.getByRole('button', { name: 'Done', exact: true }).click();
  await expect(panel.getByText('Networks', { exact: true })).toBeVisible();
  const networks = panel.locator('.nexus-mesh__section').filter({ has: page.locator('.nexus-mesh__label', { hasText: 'Networks' }) });
  const studio = networks.locator('.nexus-mesh-row').filter({ hasText: 'Studio' });
  await expect(studio).toHaveCount(1);
  await expect(studio.locator('.nexus-pill')).toHaveText('Joined network');
  await expect(networks.locator('.nexus-mesh-row').filter({ hasText: 'Home' }).locator('.nexus-pill')).toHaveText('Home');
  const server = panel.locator('.nexus-devices li').filter({ hasText: 'Studio server' });
  await expect(server.locator('.nexus-pill').filter({ hasText: 'Server' })).toHaveCount(1);
  await expect(server.locator('.nexus-pill').filter({ hasText: 'Joined network' })).toHaveCount(1);
  await expect(panel.locator('.nexus-mesh-row--choice').filter({ hasText: 'Studio server' })).toBeVisible();
  await expect(panel.locator('.nexus-mesh__state')).toHaveText('1 server online');
  await studio.getByRole('button', { name: 'Leave network', exact: true }).click();
  await studio.getByRole('button', { name: 'Leave', exact: true }).click();
  await expect(panel.getByText('Networks', { exact: true })).toHaveCount(0);
  expect(fixture.calls.some(call => call.path.endsWith('/memberships/studio/leave'))).toBe(true);
  await expect(panel.locator('.nexus-devices li').filter({ hasText: 'Studio server' })).toHaveCount(0);
});

test('a refused invite leaves the device in its own network', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  fixture.refuseMembership = true;
  await enableNexusMesh(page);
  await page.locator('.nexus-mesh').getByRole('button', { name: 'Join another network', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Join another network' });
  await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('12345678');
  await dialog.getByRole('button', { name: 'Join network', exact: true }).click();
  await expect(dialog.getByRole('alert')).toContainText('not accepted (the invite expired)');
  await dialog.getByRole('button', { name: 'Done', exact: true }).click();
  await expect(page.locator('.nexus-mesh').getByText('Networks', { exact: true })).toHaveCount(0);
});

test('Enable Mesh by invitation on a registered device reports the membership instead of moving it', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { joined: false, deviceKey: true });
  fixture.hasOwnNetwork = true;
  await page.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Enable Mesh' });
  await expect(dialog.getByRole('radio', { name: 'Use this device’s key' })).toBeChecked();
  await dialog.getByRole('radio', { name: 'Join by invitation' }).check();
  await expect(dialog).toContainText('keeps its own network');
  await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('12345678');
  await dialog.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  await expect(page.getByRole('dialog', { name: 'Joined as a member' })).toContainText('keeps its own network');
  expect(fixture.calls.find(call => call.path.endsWith('/key/unlock'))?.body).toEqual({ defer_join: true });
  expect(fixture.calls.find(call => call.path.endsWith('/join'))?.body).toEqual({ code: '12345678' });
  await page.getByRole('button', { name: 'Done', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
});

test('Chat runs on selects a mesh server through the home server and keeps its key out of storage', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  const gpu = { ...fixture.state.members[1], node_id: 'gpu', device_name: 'GPU box', is_inference: true, can_connect: true, connection_type: 'p2p' as const };
  fixture.state.members.push(gpu);
  await enableNexusMesh(page);
  const panel = page.locator('.nexus-mesh');
  await expect(panel.locator('.nexus-mesh-row--choice').filter({ hasText: 'This device' })).toHaveAttribute('aria-pressed', 'true');
  const choice = panel.locator('.nexus-mesh-row--choice').filter({ hasText: 'GPU box' });
  await expect(choice.locator('.nexus-pill--connection')).toHaveText('P2P');
  await choice.click();
  await expect(choice).toHaveAttribute('aria-pressed', 'true');
  await panel.getByRole('textbox', { name: 'API key (only if that computer sets one)' }).fill('gpu-secret');
  await panel.getByRole('button', { name: 'Save', exact: true }).click();
  await expect.poll(() => fixture.calls.filter(call => call.origin === 'http://127.0.0.1:13305' && call.path.includes('/nexus/peers/gpu/')).length).toBeGreaterThan(0);
  expect(await page.evaluate(() => Object.values(localStorage).some(value => value.includes('gpu-secret')))).toBe(false);
  await panel.locator('.nexus-mesh-row--choice').filter({ hasText: 'This device' }).click();
  await expect(panel.locator('.nexus-mesh-row--choice').filter({ hasText: 'This device' })).toHaveAttribute('aria-pressed', 'true');
});

test('deleting the account needs the device name, calls account/delete and signs the device out', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await enableNexusMesh(page);
  await page.getByRole('button', { name: 'Delete account', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Delete account' });
  const submit = dialog.getByRole('button', { name: 'Delete account permanently', exact: true });
  await expect(submit).toBeDisabled();
  await dialog.getByRole('textbox').fill('My laptop');
  await submit.click();
  await expect(page.getByRole('button', { name: 'Mesh disabled', exact: true })).toBeVisible();
  await expect(page.getByText('Your account was deleted.', { exact: false })).toBeVisible();
  await expect(page.locator('.nexus-devices')).toHaveCount(0);
  expect(fixture.calls.find(call => call.path.endsWith('/account/delete'))?.body).toEqual({ account_id: 'group', member_id: 'fixture-identity', device_node_id: 'self', confirm: 'delete' });
});

test('a pending deletion keeps the device signed in', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  fixture.deletePending = true;
  await enableNexusMesh(page);
  await page.getByRole('button', { name: 'Delete account', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Delete account' });
  await dialog.getByRole('textbox').fill('My laptop');
  await dialog.getByRole('button', { name: 'Delete account permanently', exact: true }).click();
  await expect(dialog).toContainText('has not confirmed the deletion yet');
  await dialog.getByRole('button', { name: 'Close', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
});

for (const action of ['create', 'join'] as const) {
  test(`a fresh device key handed off by the tray waits for the user to ${action}`, async ({ page }) => {
    const fixture = await setupNexusAccount(page, { joined: false, deviceKey: true, freshKey: true });
    await page.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Enable Mesh' });
    await expect(dialog.getByRole('radio', { name: 'Use this device’s key' })).toHaveCount(0);
    await expect(dialog.getByRole('radio', { name: 'Create account' })).toBeChecked();
    if (action === 'join') {
      await dialog.getByRole('radio', { name: 'Join by invitation' }).check();
      await expect(dialog).toContainText('a device that is already in the mesh');
      await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('12345678');
    }
    await dialog.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
    const unlocks = fixture.calls.filter(call => call.path.endsWith('/key/unlock'));
    expect(unlocks.map(call => call.body)).toEqual(action === 'create' ? [{ defer_join: false }] : []);
    expect(fixture.calls.filter(call => call.path.endsWith('/join')).map(call => call.body)).toEqual(action === 'join' ? [{ code: '12345678' }] : []);
  });
}
