import { expect, test, type Page } from '@playwright/test';
import { resumedNexusMesh, setupNexusAccount } from './nexusAccountFixture';

const MAPPED = 'Reachable through your router (PCP)';

async function openLocalAccount(page: Page) {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { apiKey: '', deviceKey: true });
  await resumedNexusMesh(page);
  return fixture;
}

test('the local device card shows the router mapping and its endpoint', async ({ page }, testInfo) => {
  const fixture = await openLocalAccount(page);
  fixture.state.mesh_udp_port = 51900; fixture.state.dataplane_port = 51900;
  fixture.state.mapped_endpoint = '203.0.113.5:51900'; fixture.state.mapping_protocol = 'pcp';
  await page.reload();
  await resumedNexusMesh(page);
  const local = page.locator('.nexus-mesh .nexus-devices li').first();
  await expect(local).toContainText('My laptop');
  await expect(local.locator('.nexus-pill').filter({ hasText: 'This device' })).toHaveCount(1);
  await expect(local.getByText(MAPPED)).toBeVisible();
  await expect(local.locator('code.nexus-mesh-row__endpoint')).toHaveText('203.0.113.5:51900');
  await expect(page.locator('.nexus-mesh').getByText(MAPPED)).toHaveCount(1);
  await page.locator('.nexus-mesh').screenshot({ path: testInfo.outputPath('mesh-router-mapping.png') });
});

test('a device without a router mapping shows no mapped line', async ({ page }) => {
  await openLocalAccount(page);
  await expect(page.locator('.nexus-mesh .nexus-devices li').first()).toContainText('My laptop');
  await expect(page.getByText('Reachable through your router')).toHaveCount(0);
});

test('the mesh UDP port saves through the device route and shows the port in use', async ({ page }, testInfo) => {
  const fixture = await openLocalAccount(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  const setting = page.locator('.nexus-port-setting');
  const field = setting.getByRole('textbox', { name: 'Mesh UDP port', exact: true });
  const save = setting.getByRole('button', { name: 'Save', exact: true });
  await expect(field).toHaveValue('0');
  await expect(setting).toContainText('In use: 60123');
  await expect(save).toBeDisabled();
  await field.fill('51900');
  await save.click();
  await expect(setting).toContainText('In use: 51900');
  const write = fixture.calls.find(call => call.path.endsWith('/device'))!;
  expect(write.body).toEqual({ mesh_udp_port: 51900 });
  expect(write.authorization).toBeUndefined();
  await expect(field).toHaveValue('51900');
  await page.locator('.nexus-port-setting').screenshot({ path: testInfo.outputPath('mesh-udp-port-saved.png') });
  await field.fill('70000');
  await save.click();
  await expect(setting.getByRole('alert')).toHaveText('Enter a UDP port (1-65535), or 0 for any port');
  await expect(field).toHaveValue('70000');
  await expect(setting).toContainText('In use: 51900');
  expect(fixture.calls.filter(call => call.path.endsWith('/device')).at(-1)?.body).toEqual({ mesh_udp_port: 70000 });
});

test('a bind failure keeps the port field editable while the device is locked', async ({ page }) => {
  const fixture = await openLocalAccount(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  fixture.state.mesh_udp_port = 51900; fixture.state.dataplane_port = 0; fixture.state.locked = true; fixture.state.mesh_up = false; fixture.state.status = 'locked';
  fixture.state.error = 'Could not bind the mesh dataplane to UDP port 51900. Choose a free port, or 0 for any port.'; fixture.state.error_retryable = false;
  const setting = page.locator('.nexus-port-setting');
  const field = setting.getByRole('textbox', { name: 'Mesh UDP port', exact: true });
  await expect(page.locator('.nexus-mesh').getByRole('alert')).toContainText('Could not bind the mesh dataplane to UDP port 51900', { timeout: 15000 });
  await expect(field).toHaveValue('51900');
  await expect(field).toBeEnabled();
  await expect(setting).not.toContainText('In use:');
  await field.fill('0');
  await setting.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(field).toHaveValue('0');
  expect(fixture.calls.filter(call => call.path.endsWith('/device')).at(-1)?.body).toEqual({ mesh_udp_port: 0 });
});
