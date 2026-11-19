import { expect, test, type Page } from '@playwright/test';
import { enableNexusMesh, setupNexusAccount } from './nexusAccountFixture';

async function openJoinForm(page: Page) {
  await page.getByRole('button', { name: 'Enable Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Enable Mesh' });
  await dialog.getByRole('radio', { name: 'Join by invitation' }).check();
  return { dialog, invitation: dialog.getByRole('textbox', { name: 'Invitation code or token' }), submit: dialog.getByRole('button', { name: 'Enable Mesh', exact: true }) };
}
const joinBody = (fixture: Awaited<ReturnType<typeof setupNexusAccount>>) => fixture.calls.find(call => call.path.endsWith('/join'))?.body;

test('a typed invitation code with its space joins with the digits alone', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { registered: false, joined: false });
  const { dialog, invitation, submit } = await openJoinForm(page);
  await expect(dialog).toContainText('The code is 8 digits.');
  await invitation.pressSequentially('4709 6838');
  await expect(invitation).toHaveValue('4709 6838');
  await expect(submit).toBeEnabled();
  await submit.click();
  await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
  expect(joinBody(fixture)).toEqual({ code: '47096838' });
});

test('a pasted invitation code with a dash joins with the digits alone', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  const fixture = await setupNexusAccount(page, { registered: false, joined: false });
  const { invitation, submit } = await openJoinForm(page);
  await page.evaluate(() => navigator.clipboard.writeText('4709-6838'));
  await invitation.focus();
  await page.keyboard.press('ControlOrMeta+V');
  await expect(invitation).toHaveValue('4709-6838');
  await expect(submit).toBeEnabled();
  await submit.click();
  await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
  expect(joinBody(fixture)).toEqual({ code: '47096838' });
});

test('a full invite token still joins unchanged', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { registered: false, joined: false });
  const { invitation, submit } = await openJoinForm(page);
  await invitation.fill(`lnk_${'b'.repeat(64)}`);
  await expect(submit).toBeEnabled();
  await submit.click();
  await expect(page.getByRole('button', { name: 'Mesh: My laptop', exact: true })).toBeVisible();
  expect(joinBody(fixture)).toEqual({ code: `lnk_${'b'.repeat(64)}` });
});

test('a grouped code joins another network with the digits alone', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await enableNexusMesh(page);
  await page.getByRole('button', { name: 'Join another network' }).click();
  const dialog = page.getByRole('dialog', { name: 'Join another network' });
  await expect(dialog).toContainText('The code is 8 digits.');
  await dialog.getByRole('textbox', { name: 'Invitation code or token' }).fill('4709 6838');
  await dialog.getByRole('button', { name: 'Join network', exact: true }).click();
  await expect(dialog.getByRole('status')).toContainText('joined the invited network');
  expect(fixture.calls.find(call => call.path.endsWith('/invites/accept'))?.body).toEqual({ code: '47096838' });
});

const failures = [
  { status: 409, error: 'link token group no longer exists', message: 'The device that created this invitation is no longer in a network. Create a new invitation on a device that is in the mesh.' },
  { status: 403, error: 'invalid or expired link token', message: 'This code is wrong, already used, or older than 10 minutes. Create a new invitation.' },
  { status: 409, error: 'link token already spent', message: 'This code is wrong, already used, or older than 10 minutes. Create a new invitation.' },
  { status: 502, error: 'link token request failed (server unreachable)', message: 'Could not reach the Nexus servers. Check the network and try again.' },
  { status: 503, error: 'Could not establish connection to the mesh', message: 'Could not reach the Nexus servers. Check the network and try again.' },
  { status: 403, error: 'inviter is not allowed to add devices', message: 'The inviting device may not add devices. Ask an administrator of the network for an invitation.' },
  { status: 403, error: 'Nexus refused this invitation.', message: 'The inviting device may not add devices. Ask an administrator of the network for an invitation.' },
];
for (const failure of failures) {
  test(`a join refused with "${failure.error}" (${failure.status}) explains the next step`, async ({ page }) => {
    const fixture = await setupNexusAccount(page, { registered: false, joined: false });
    fixture.inviteFailure = { status: failure.status, error: failure.error };
    const { dialog, invitation, submit } = await openJoinForm(page);
    await invitation.fill('47096838');
    await submit.click();
    await expect(dialog.getByRole('alert')).toHaveText(failure.message);
    const detail = dialog.locator('details.nexus-error-detail');
    await expect(detail).not.toHaveAttribute('open', '');
    await detail.locator('summary').click();
    await expect(detail).toContainText(failure.error);
  });
}

test('an unknown join failure keeps the server text without a detail element', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { registered: false, joined: false });
  fixture.inviteFailure = { status: 409, error: 'Join the network first' };
  const { dialog, invitation, submit } = await openJoinForm(page);
  await invitation.fill('47096838');
  await submit.click();
  await expect(dialog.getByRole('alert')).toHaveText('Join the network first');
  await expect(dialog.locator('details.nexus-error-detail')).toHaveCount(0);
});

test('a refused invitation creation explains the next step', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await enableNexusMesh(page);
  fixture.inviteFailure = { status: 403, error: 'missing add_child permission on the group' };
  await page.getByRole('button', { name: 'Connect another device' }).click();
  const dialog = page.getByRole('dialog', { name: 'Connect another device' });
  await dialog.getByRole('textbox', { name: 'Device name', exact: true }).fill('My phone');
  await dialog.getByRole('button', { name: 'Create invitation', exact: true }).click();
  await expect(dialog.getByRole('alert')).toHaveText('The inviting device may not add devices. Ask an administrator of the network for an invitation.');
  await expect(dialog.locator('details.nexus-error-detail')).toContainText('missing add_child permission on the group');
});
