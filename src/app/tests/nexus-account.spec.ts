import { expect, test, type Page } from '@playwright/test';
import { setupNexusAccount, signInNexusAccount } from './nexusAccountFixture';

const avatarFile = { name: 'profile.png', mimeType: 'image/png', buffer: Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6XcAAAAASUVORK5CYII=', 'base64') };

test('account images survive sign-in and propagate removal', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  await page.getByLabel('Profile image file', { exact: true }).setInputFiles(avatarFile);
  await expect(page.locator('.nexus-account-trigger img')).toBeVisible();
  expect(fixture.avatar.revision).toBe(1);
  const saved = fixture.avatar.image;
  await page.getByRole('button', {name: 'Sign out', exact: true}).click();
  await expect(page.locator('.nexus-account-trigger img')).toHaveCount(0);
  await signInNexusAccount(page);
  await expect(page.locator('.nexus-account-trigger img')).toHaveAttribute('src', saved!);
  fixture.avatar = {...fixture.avatar, image: null, revision: 2};
  await expect(page.locator('.nexus-account-trigger img')).toHaveCount(0, {timeout: 10000});
  await page.getByLabel('Profile image file', { exact: true }).setInputFiles(avatarFile);
  await expect(page.locator('.nexus-account-trigger img')).toBeVisible();
  await page.getByRole('button', {name: 'Remove image', exact: true}).click();
  await expect(page.locator('.nexus-account-trigger img')).toHaveCount(0);
  expect(fixture.avatar.image).toBeNull();
});

test('a failed image save preserves the current image', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  await page.getByLabel('Profile image file', {exact: true}).setInputFiles(avatarFile);
  await expect(page.locator('.nexus-account-trigger img')).toBeVisible();
  const saved = fixture.avatar.image;
  fixture.rejectWrites = true;
  await page.getByRole('button', {name: 'Remove image', exact: true}).click();
  await expect(page.getByRole('alert')).toContainText('The mesh could not save the image.');
  await expect(page.locator('.nexus-account-trigger img')).toHaveAttribute('src', saved!);
});

test('older meshes keep sign-in working and explain the profile update requirement', async ({ page }) => {
  await setupNexusAccount(page, {avatarUnsupported: true});
  await signInNexusAccount(page);
  await expect(page.getByRole('button', {name: 'Change profile image', exact: true})).toBeDisabled();
  await expect(page.getByText('This mesh needs a Nexus server update to sync profile images.')).toBeVisible();
});

test('a late image-save response cannot restore a signed-out account', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  let release!: () => void;
  fixture.avatarSaveGate = new Promise<void>(resolve => { release = resolve; });
  const response = page.waitForResponse(reply => reply.url().endsWith('/nexus/account/avatar'));
  await page.getByLabel('Profile image file', {exact: true}).setInputFiles(avatarFile);
  await expect.poll(() => fixture.calls.some(call => call.path.endsWith('/account/avatar'))).toBe(true);
  await page.getByRole('button', {name: 'Sign out', exact: true}).click();
  release();
  await response;
  await expect(page.getByRole('button', {name: 'User: Not signed in', exact: true})).toBeVisible();
  await expect(page.locator('.nexus-account-trigger img')).toHaveCount(0);
});

test('an unlocked mesh does not sign in the account automatically', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await expect(page.getByRole('button', { name: 'User: Not signed in' })).toBeVisible();
  await expect(page.getByText('Lemonade account', { exact: true })).toBeVisible();
  await expect(page.locator('.nexus-devices')).toHaveCount(0);
  expect(fixture.calls.some(call => call.path.endsWith('/account') || call.path.endsWith('/enable'))).toBe(false);
});

test('the account dropdown opens sign-in and restores focus when cancelled', async ({ page }) => {
  await setupNexusAccount(page);
  const trigger = page.getByRole('button', { name: 'User: Not signed in' });
  await trigger.click();
  const menu = page.getByRole('dialog', { name: 'Nexus Account Manager' });
  await menu.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await expect(dialog).toBeVisible();
  await expect(menu).toHaveCount(0);
  await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(trigger).toBeFocused();
});

test('browser sign-in explains the desktop requirement', async ({ page }) => {
  await setupNexusAccount(page, { desktop: false });
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await expect(dialog).toContainText('Open the Lemonade desktop app on this computer');
  await expect(dialog.getByRole('button', { name: 'Continue with passkey' })).toHaveCount(0);
});

test('creating an account registers and verifies before showing its role', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { registered: false, joined: false });
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  const submit = dialog.getByRole('button', { name: 'Create with passkey' });
  await expect(submit).toBeDisabled();
  await dialog.getByRole('textbox', { name: 'Account name', exact: true }).fill('Geramy');
  await submit.click();
  await expect(page.getByRole('button', { name: 'User: Geramy', exact: true })).toBeVisible();
  await expect(page.locator('.nexus-role')).toHaveText('Admin');
  const paths = fixture.calls.map(call => call.path);
  expect(paths.findIndex(path => path.endsWith('/passkey/register'))).toBeGreaterThan(-1);
  expect(paths.findIndex(path => path.endsWith('/account'))).toBeGreaterThan(paths.findIndex(path => path.endsWith('/passkey/unlock')));
});

test('cancelled passkey verification keeps the draft and signed-out state', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { cancelPasskey: true });
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await dialog.getByRole('textbox', { name: 'Account name', exact: true }).fill('Geramy');
  await dialog.getByRole('button', { name: 'Continue with passkey' }).click();
  await expect(dialog.getByRole('alert')).toContainText('cancelled');
  await expect(dialog.getByRole('textbox', { name: 'Account name', exact: true })).toHaveValue('Geramy');
  await expect(page.getByRole('button', { name: 'User: Not signed in' })).toBeVisible();
  expect(fixture.calls.some(call => call.path.endsWith('/account'))).toBe(false);
});

test('joining validates both invitation formats and keeps the member role', async ({ page }) => {
  const fixture = await setupNexusAccount(page, { registered: false, joined: false, readOnly: true });
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await dialog.getByRole('radio', { name: 'Join by invitation' }).check();
  await dialog.getByRole('textbox', { name: 'Account name', exact: true }).fill('Alex');
  const invitation = dialog.getByRole('textbox', { name: 'Invitation code or token' });
  const submit = dialog.getByRole('button', { name: 'Join with passkey' });
  await invitation.fill('1234567'); await expect(submit).toBeDisabled();
  await invitation.fill(`lnk_${'a'.repeat(64)}`); await expect(submit).toBeEnabled();
  await invitation.fill(`lnk_${'z'.repeat(64)}`); await expect(submit).toBeDisabled();
  await invitation.fill('12345678'); await submit.click();
  await expect(page.getByRole('button', { name: 'User: Alex', exact: true })).toBeVisible();
  await expect(page.locator('.nexus-role')).toHaveCount(0);
  expect(fixture.calls.find(call => call.path.endsWith('/join'))?.body).toEqual({ code: '12345678' });
});

test('sign-out clears account devices without locking the shared server', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  await expect(page.locator('.nexus-devices')).toContainText('Kitchen PC');
  await page.getByRole('button', { name: 'Sign out', exact: true }).click();
  await expect(page.getByRole('button', { name: 'User: Not signed in' })).toBeVisible();
  await expect(page.locator('.nexus-devices')).toHaveCount(0);
  expect(fixture.calls.some(call => /\/(lock|disable)$/.test(call.path))).toBe(false);
});

test('a saved label pre-fills the form but does not restore authentication', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  const reads = fixture.calls.filter(call => call.path.endsWith('/account')).length;
  await page.reload();
  await expect(page.getByRole('button', { name: 'User: Not signed in' })).toBeVisible();
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  const dialog = page.getByRole('dialog', { name: 'Sign in to Lemonade' });
  await expect(dialog.getByRole('textbox', { name: 'Account name', exact: true })).toHaveValue('Geramy');
  expect(fixture.calls.filter(call => call.path.endsWith('/account')).length).toBe(reads);
  const saved = await page.evaluate(() => JSON.parse(localStorage.getItem('lemonade_nexus_account_labels') || '{}'));
  expect(saved['http://127.0.0.1:13305']).toEqual({ id: 'group', displayName: 'Geramy' });
});

test('read-only members have no admin badge or invitation action', async ({ page }) => {
  await setupNexusAccount(page, { readOnly: true });
  await signInNexusAccount(page, 'Alex');
  await expect(page.locator('.nexus-role')).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Connect another device' })).toHaveCount(0);
});

test('an authorized account creates an expiring device invitation', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  await page.getByRole('button', { name: 'Connect another device' }).click();
  const dialog = page.getByRole('dialog', { name: 'Connect another device' });
  await dialog.getByRole('textbox', { name: 'Device you are inviting' }).fill('My phone');
  await dialog.getByRole('button', { name: 'Create invitation', exact: true }).click();
  await expect(dialog).toContainText('12345678');
  await expect(dialog.getByRole('timer')).toContainText('Expires in');
  await expect(dialog).toContainText('Closing this dialog does not revoke it.');
  expect(fixture.calls.find(call => call.path.endsWith('/invites'))?.body).toEqual({ device_name: 'My phone', ttl_sec: 600 });
});

test('Advanced navigates recursive groups and leaves have no group actions', async ({ page }) => {
  await setupNexusAccount(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  const team = page.getByRole('treeitem', { name: 'Team', exact: true });
  await team.click(); await team.press('ArrowRight');
  const lab = page.getByRole('treeitem', { name: 'Lab', exact: true });
  await lab.click(); await lab.press('ArrowRight');
  await page.getByRole('treeitem', { name: 'Lab workstation', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Add group', exact: true })).toHaveCount(0);
  await expect(page.locator('.nexus-tree-breadcrumbs')).toContainText('Home');
  await expect(page.locator('.nexus-tree-breadcrumbs')).toContainText('Lab workstation');
});

test('Advanced creates and renames groups using the existing controller key', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  await page.getByRole('button', { name: 'Add group', exact: true }).click();
  await page.getByRole('textbox', { name: 'Group name', exact: true }).fill('Design');
  await page.getByRole('button', { name: 'Create group', exact: true }).click();
  await page.getByRole('treeitem', { name: 'Design', exact: true }).click();
  await page.getByRole('button', { name: 'Rename', exact: true }).click();
  await page.getByRole('textbox', { name: 'Group name', exact: true }).fill('Design team');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.getByRole('treeitem', { name: 'Design team', exact: true })).toBeVisible();
  const writes = fixture.calls.filter(call => call.method === 'POST' && call.path.includes('/tree/'));
  expect(writes.map(call => call.body)).toEqual([{ name: 'Design' }, { name: 'Design team' }]);
  expect(writes.every(call => call.authorization === 'Bearer fixture-server-key')).toBe(true);
});

test('a rejected group write retains its draft without a phantom node', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  fixture.rejectWrites = true;
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  await page.getByRole('button', { name: 'Add group', exact: true }).click();
  await page.getByRole('textbox', { name: 'Group name', exact: true }).fill('Design');
  await page.getByRole('button', { name: 'Create group', exact: true }).click();
  await expect(page.locator('.nexus-group-editor').getByRole('alert')).toContainText('cannot be changed');
  await expect(page.getByRole('textbox', { name: 'Group name', exact: true })).toHaveValue('Design');
  await expect(page.getByRole('treeitem', { name: 'Design', exact: true })).toHaveCount(0);
});

test('read-only tree capabilities hide mutation controls', async ({ page }) => {
  await setupNexusAccount(page, { readOnly: true });
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  await expect(page.getByText('You have read-only access to this group.')).toBeVisible();
  for (const name of ['Add group', 'Rename', 'Permissions']) await expect(page.getByRole('button', { name, exact: true })).toHaveCount(0);
});

test('invalid tree data is recoverable', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  fixture.invalidTree = true;
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  await expect(page.locator('.nexus-hierarchy').getByRole('alert')).toContainText('invalid group data');
  fixture.invalidTree = false;
  await page.getByRole('button', { name: 'Retry', exact: true }).click();
  await expect(page.getByRole('treeitem', { name: 'Home', exact: true })).toBeVisible();
});

test('Basic and Advanced retain the same width and toggle position', async ({ page }) => {
  await page.setViewportSize({ width: 1920, height: 1080 });
  await setupNexusAccount(page);
  const toggle = page.getByRole('group', { name: 'Management mode', exact: true });
  const basic = await toggle.boundingBox();
  const width = await page.locator('.nexus-page').evaluate(el => el.getBoundingClientRect().width);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  expect(await toggle.boundingBox()).toEqual(basic);
  expect(await page.locator('.nexus-page').evaluate(el => el.getBoundingClientRect().width)).toBe(width);
});

async function selectionStyle(page: Page) {
  return page.locator('.workspace-nav button.is-active').evaluate(el => {
    const row = getComputedStyle(el), icon = getComputedStyle(el.querySelector('.workspace-nav__icon')!);
    return [row.backgroundColor, row.borderColor, row.color, row.boxShadow, icon.backgroundColor, icon.color];
  });
}

test('Nexus uses the shared Settings selection colors in both themes', async ({ page }) => {
  await setupNexusAccount(page);
  await page.addStyleTag({ content: '.workspace-nav button, .workspace-nav__icon { transition: none !important; }' });
  for (const theme of ['dark', 'light']) {
    if (theme === 'light') await page.getByRole('button', { name: 'Toggle theme', exact: true }).click();
    const nexus = await selectionStyle(page);
    await page.getByRole('button', { name: 'MCP gateway', exact: true }).click();
    expect(await selectionStyle(page)).toEqual(nexus);
    await page.getByRole('button', { name: 'Nexus', exact: true }).click();
  }
});

test('a rejected server API key gives actionable sign-in guidance', async ({ page }) => {
  await setupNexusAccount(page, { unauthorized: true });
  await page.getByRole('button', { name: 'Connect to Mesh', exact: true }).click();
  await expect(page.getByRole('dialog', { name: 'Sign in to Lemonade' }).getByRole('alert')).toContainText('Settings > Server');
});

test('lost account access clears the signed-in identity', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  await signInNexusAccount(page);
  fixture.accountError = true;
  await expect(page.getByRole('button', { name: 'User: Not signed in' })).toBeVisible({ timeout: 12000 });
  await expect(page.getByText('Account access ended. Sign in again to continue.')).toBeVisible();
});


test('an offline image source shows waiting guidance without blocking account access', async ({ page }) => {
  const fixture = await setupNexusAccount(page);
  fixture.avatar = {...fixture.avatar, revision:1, pending:true};
  await signInNexusAccount(page);
  await expect(page.getByText('Connect a device with this profile image to download it.')).toBeVisible();
  await expect(page.getByRole('button', { name:'Change profile image', exact:true })).toBeEnabled();
  await expect(page.locator('.nexus-account-summary img')).toHaveCount(0);
  fixture.avatar.pending = false;
  await expect(page.getByText('Connect a device with this profile image to download it.')).toHaveCount(0, {timeout:12000});
});
