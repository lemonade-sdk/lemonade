import { test, expect, type Page } from '@playwright/test';
import { setupNexusAccount, enableNexusMesh } from './nexusAccountFixture';

// A mesh inference server with a P2P path, as lemond lists it once the mesh is up.
const gpuBox = { node_id: 'gpu', device_name: 'GPU box', tunnel_ip: '10.8.0.9', management_pubkey: 'gpu-key', permissions: ['read'], permissions_known: true,
  is_admin: false, is_online: true, can_remove: false, is_inference: true, can_connect: true, connection_type: 'p2p' as const };
const gpuModel = { id: 'Qwen-on-gpu', model_name: 'Qwen-on-gpu', labels: ['chat'], recipe: 'llamacpp', downloaded: true };

async function openPickerWithMeshHost(page: Page, prepare: (fixture: Awaited<ReturnType<typeof setupNexusAccount>>) => void) {
  const fixture = await setupNexusAccount(page);
  fixture.state.members.push(gpuBox);
  fixture.peerModels = [gpuModel];
  prepare(fixture);
  await enableNexusMesh(page);
  await page.getByRole('button', { name: 'Chat', exact: true }).first().click();
  await page.waitForSelector('.chat');
  await page.locator('.composer__model-button').click();
  return { fixture, host: page.getByRole('group', { name: 'GPU box' }) };
}

test('the picker opens every mesh host and lists its models without a click', async ({ page }) => {
  const { host } = await openPickerWithMeshHost(page, () => {});
  await expect(host.getByRole('option', { name: 'Qwen-on-gpu, GPU box' })).toBeVisible();
});

test('a host whose first answer is missing is read again instead of showing an error', async ({ page }) => {
  const { fixture, host } = await openPickerWithMeshHost(page, fixture => { fixture.peerFailures = 2; });
  await expect(host.getByText('Connecting to this device’s server…')).toBeVisible();
  await expect(host.getByRole('option', { name: 'Qwen-on-gpu, GPU box' })).toBeVisible({ timeout: 15000 });
  expect(fixture.peerFailures).toBe(0);
  expect(host.getByText("Cannot reach that device's server through the mesh")).toHaveCount(0);
});

test('a host that refuses this device names the permission and offers no retry loop', async ({ page }) => {
  const { fixture, host } = await openPickerWithMeshHost(page, fixture => { fixture.peerRefused = true; });
  await expect(host.getByText('That device does not allow this device to use its models', { exact: false })).toBeVisible();
  const refusals = fixture.calls.filter(call => call.path.includes('/nexus/peers/gpu/')).length;
  await page.waitForTimeout(2000);
  expect(fixture.calls.filter(call => call.path.includes('/nexus/peers/gpu/')).length).toBe(refusals);
  fixture.peerRefused = false;
  await host.getByRole('button', { name: 'Try again' }).click();
  await expect(host.getByRole('option', { name: 'Qwen-on-gpu, GPU box' })).toBeVisible();
});
