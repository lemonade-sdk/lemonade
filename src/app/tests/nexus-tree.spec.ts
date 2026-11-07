import { expect, test } from '@playwright/test';
import { enableNexusMesh, setupNexusAccount } from './nexusAccountFixture';

test('device tiles contain their icon and leave the tree stem outside', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { deviceKey: true });
  fixture.branches.group.children = fixture.branches.group.children.filter(child => child.id === 'self');
  fixture.branches.group.children[0].tunnel_ip = fixture.state.members[0].tunnel_ip;
  await enableNexusMesh(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  const device = page.getByRole('treeitem', { name: 'My laptop', exact: true });
  await device.click();
  await expect(page.getByRole('treeitem')).toHaveCount(2);
  await expect(device.locator('.nexus-tree__disclosure')).toHaveCount(0);
  await expect(device.locator('.nexus-tree__guide--last')).toHaveCount(1);
  const icon = (await device.locator('.nexus-tree__icon').boundingBox())!;
  const guides = (await device.locator('.nexus-tree__guides').boundingBox())!;
  const tile = (await device.locator('.nexus-tree__tile').boundingBox())!;
  const root = page.getByRole('treeitem', { name: 'Home', exact: true });
  const rootTile = (await root.locator('.nexus-tree__tile').boundingBox())!;
  const disclosure = (await root.locator('.nexus-tree__disclosure').boundingBox())!;
  expect(rootTile.x).toBe(disclosure.x + disclosure.width);
  expect(rootTile.x).toBeLessThan(tile.x);
  expect(tile.x).toBe(guides.x + guides.width);
  await expect(device.locator('.nexus-tree__connector')).toHaveCount(0);
  expect(icon.x).toBeGreaterThan(tile.x);
  expect(icon.x + icon.width).toBeLessThan(tile.x + tile.width);
  await expect(device).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
  await expect(device.locator('.nexus-tree__tile')).not.toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
  await page.locator('.nexus-hierarchy').screenshot({ path: testInfo.outputPath('nexus-tree-single-device.png') });
  await page.locator('.nexus-tree-panel').screenshot({ path: testInfo.outputPath('nexus-tree-tile-spacing.png') });
  await root.click();
  await page.locator('.nexus-tree__rows').screenshot({ path: testInfo.outputPath('nexus-tree-root-alignment.png') });
});

test('nested branches stay aligned through collapse, growth, deep nesting and narrow layouts', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 1440, height: 1100 });
  const fixture = await setupNexusAccount(page, { deviceKey: true });
  const template = fixture.branches.group.children.find(child => child.id === 'peer')!;
  const remote = { ...fixture.branches.team.parent, id: 'remote', name: 'Remote office', parent_id: 'group' };
  fixture.branches.group.children.push(remote);
  fixture.branches.remote = { parent: remote, children: [
    { ...template, id: 'studio', name: 'Studio server', parent_id: remote.id },
    { ...template, id: 'travel', name: 'Travel laptop', parent_id: remote.id },
  ] };
  const member = fixture.state.members[1];
  member.connection_type = 'p2p';
  for (const [id, name, online, connection] of [
    ['workstation', 'Lab workstation', true, 'p2p'],
    ['studio', 'Studio server', true, 'relay'],
    ['travel', 'Travel laptop', false, undefined],
  ] as const) fixture.state.members.push({ ...member, node_id: id, device_name: name, is_online: online, connection_type: connection, tunnel_ip: `10.8.0.${fixture.state.members.length + 2}` });
  Object.values(fixture.branches).forEach(branch => branch.children.forEach(child => {
    child.tunnel_ip = fixture.state.members.find(device => device.node_id === child.id)?.tunnel_ip || '';
  }));
  await enableNexusMesh(page);
  await page.getByRole('radio', { name: 'Advanced', exact: true }).check();
  const row = (name: string) => page.getByRole('treeitem', { name, exact: true });
  for (const name of ['Team', 'Lab', 'Remote office']) {
    await row(name).click();
    await row(name).press('ArrowRight');
    await expect(row(name)).toHaveAttribute('aria-expanded', 'true');
  }
  await expect(row('Travel laptop')).toBeVisible();
  await expect(row('Lab workstation').locator('.nexus-tree__guide--continued')).toHaveCount(1);
  await expect(row('Studio server').locator('.nexus-tree__guide--continued')).toHaveCount(0);
  await expect(row('Travel laptop').locator('.nexus-tree__guide--last')).toHaveCount(1);
  const teamName = (await row('Team').locator('.nexus-tree__name').boundingBox())!;
  const labName = (await row('Lab').locator('.nexus-tree__name').boundingBox())!;
  expect(labName.x - teamName.x).toBe(20);
  await row('Lab workstation').click();
  await expect(page.locator('.nexus-tree-detail h4')).toHaveText('Lab workstation');
  for (const item of await page.getByRole('treeitem').all()) {
    const tile = (await item.locator('.nexus-tree__tile').boundingBox())!;
    const entryIcon = item.locator('.nexus-tree__tile .nexus-tree__icon');
    await expect(entryIcon).toHaveCount(1);
    const icon = (await entryIcon.boundingBox())!;
    expect(icon.x).toBeGreaterThan(tile.x);
    expect(icon.x + icon.width).toBeLessThan(tile.x + tile.width);
    if (await item.getAttribute('aria-expanded') === null) {
      const guides = (await item.locator('.nexus-tree__guides').boundingBox())!;
      expect(tile.x).toBe(guides.x + guides.width);
    }
  }
  await page.locator('.nexus-hierarchy').screenshot({ path: testInfo.outputPath('nexus-tree-expanded.png') });
  await row('Team').click();
  await row('Team').press('ArrowLeft');
  await expect(row('Lab workstation')).toHaveCount(0);
  await row('Team').press('ArrowDown');
  await expect(row('My laptop')).toBeFocused();
  await row('Team').locator('.nexus-tree__disclosure').click();
  await expect(row('Lab workstation')).toBeVisible();
  await page.setViewportSize({ width: 720, height: 1100 });
  await row('Travel laptop').click();
  await page.locator('.nexus-hierarchy').screenshot({ path: testInfo.outputPath('nexus-tree-narrow.png') });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.setViewportSize({ width: 480, height: 1100 });

  fixture.branches.remote.children.push(...Array.from({ length: 32 }, (_, index) => ({ ...template,
    id: `additional-${index}`, name: `Workstation ${index + 1}`, parent_id: remote.id,
  })));
  let parent = fixture.branches.lab.parent;
  for (let depth = 4; depth <= 12; depth += 1) {
    const child = { ...parent, id: `depth-${depth}`, name: `Level ${depth}`, parent_id: parent.id };
    fixture.branches[parent.id].children.push(child);
    fixture.branches[child.id] = { parent: child, children: [] };
    parent = child;
  }
  await page.locator('.nexus-hierarchy').getByRole('button', { name: 'Refresh', exact: true }).click();
  for (const name of ['Remote office', 'Team', 'Lab', ...Array.from({ length: 8 }, (_, i) => `Level ${i + 4}`)]) {
    await row(name).click();
    await row(name).press('ArrowRight');
  }
  await expect(row('Level 12')).toBeAttached();
  const level11 = (await row('Level 11').boundingBox())!;
  const level12 = (await row('Level 12').boundingBox())!;
  const icon11 = (await row('Level 11').locator('.nexus-tree__disclosure').boundingBox())!;
  const icon12 = (await row('Level 12').locator('.nexus-tree__disclosure').boundingBox())!;
  expect((icon12.x - level12.x) - (icon11.x - level11.x)).toBe(20);
  const scroll = await page.getByRole('tree').evaluate(element => ({ height: element.clientHeight, contentHeight: element.scrollHeight, width: element.clientWidth, contentWidth: element.scrollWidth }));
  expect(scroll.contentHeight).toBeGreaterThan(scroll.height);
  expect(scroll.contentWidth).toBeGreaterThan(scroll.width);
  await row('Level 12').click();
  await row('Level 12').press('End');
  await expect(row('Workstation 32')).toBeFocused();
  await expect(row('Workstation 32')).toBeInViewport();
  await row('Workstation 32').press('Home');
  await expect(row('Home')).toBeFocused();
  await expect(row('Home')).toBeInViewport();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});
