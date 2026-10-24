import { expect, test } from '@playwright/test';
import { setupNexusAccount } from './nexusAccountFixture';

test('account and downloads switch on click in both directions', async ({ page }) => {
  await setupNexusAccount(page);
  const account = page.locator('.nexus-account-trigger');
  const downloads = page.getByRole('button', { name: 'Open download manager', exact: true });
  const accountPanel = page.getByRole('dialog', { name: 'Lemonade Account', exact: true });
  const downloadPanel = page.getByRole('dialog', { name: 'Download manager', exact: true });
  await account.click();
  await downloads.focus();
  await expect(accountPanel).toBeVisible();
  await downloads.hover();
  await page.mouse.down();
  await expect(accountPanel).toBeVisible();
  await expect(downloadPanel).toHaveCount(0);
  await page.mouse.up();
  await expect(downloadPanel).toBeVisible();
  await expect(accountPanel).toHaveCount(0);
  await account.focus();
  await account.hover();
  await page.mouse.down();
  await expect(downloadPanel).toBeVisible();
  await expect(accountPanel).toHaveCount(0);
  await page.mouse.up();
  await expect(accountPanel).toBeVisible();
  await expect(downloadPanel).toHaveCount(0);
});

for (const desktop of [true, false]) {
  test(`${desktop ? 'desktop' : 'browser'} search responds to control width`, async ({ page }) => {
    await page.setViewportSize({ width: 1800, height: 800 });
    await setupNexusAccount(page, { desktop });
    const header = page.locator('.titlebar');
    await expect(header).not.toHaveClass(/titlebar--search-collapsed/);
    await expect(page.locator('.nexus-account-trigger__label')).toBeVisible();
    await page.locator('.titlebar__right').evaluate(right => {
      const control = document.createElement('button');
      control.id = 'extra-header-control';
      control.textContent = 'Extra control';
      control.style.cssText = 'width: 400px; flex: 0 0 400px';
      right.append(control);
    });
    await expect(header).toHaveClass(/titlebar--search-collapsed/);
    await expect(page.locator('.nexus-account-trigger__label')).toBeHidden();
    await page.locator('.titlebar__search-toggle').click();
    await expect(page.locator('.titlebar__search input')).toBeVisible();
    await page.keyboard.press('Escape');
    const gap = await header.evaluate(element => element.querySelector('.titlebar__right')!.getBoundingClientRect().left
      - element.querySelector('.titlebar__nav')!.getBoundingClientRect().right);
    expect(gap).toBeGreaterThanOrEqual(16);
    await page.locator('#extra-header-control').evaluate(control => control.remove());
    await expect(header).not.toHaveClass(/titlebar--search-collapsed/);
    await expect(page.locator('.nexus-account-trigger__label')).toBeVisible();
  });

  test(`${desktop ? 'desktop' : 'browser'} header fits without resizing window controls`, async ({ page }) => {
    await setupNexusAccount(page, { desktop });
    let baseline: { width: number; height: number; iconWidth: number; iconHeight: number }[] | undefined;
    for (const theme of ['dark', 'light']) {
      await page.setViewportSize({ width: 1280, height: 800 });
      if (theme === 'light') {
        await page.getByRole('button', { name: 'User: Not signed in', exact: true }).click();
        await page.getByRole('button', { name: 'Toggle theme', exact: true }).click();
        await page.keyboard.press('Escape');
      }
      for (const width of [1280, 1101, 1100, 901, 900, 821, 820, 769, 768, 600, 481, 480, 400, ...(desktop ? [] : [375, 360, 320])]) {
        await page.setViewportSize({ width, height: 800 });
        const layout = await page.locator('.titlebar').evaluate(header => {
          const nav = header.querySelector<HTMLElement>('.titlebar__nav')!;
          const right = header.querySelector<HTMLElement>('.titlebar__right')!;
          const account = header.querySelector<HTMLElement>('.nexus-account-trigger')!;
          const accountBounds = account.getBoundingClientRect();
          const bounds = nav.getBoundingClientRect();
          return {
            scrollWidth: nav.scrollWidth, clientWidth: nav.clientWidth, height: bounds.height,
            navRight: bounds.right, controlsLeft: right.getBoundingClientRect().left,
            controlsRight: right.getBoundingClientRect().right,
            accountContentsFit: Array.from(account.children).every(child => {
              if (!child.getClientRects().length) return true;
              const childBounds = child.getBoundingClientRect();
              return childBounds.left >= accountBounds.left && childBounds.right <= accountBounds.right;
            }),
            windowControlsSeparated: Array.from(header.querySelectorAll('.titlebar__window-btn')).every((button, index, buttons) => {
              const buttonBounds = button.getBoundingClientRect();
              const previous = buttons[index - 1]?.getBoundingClientRect();
              const icon = button.querySelector('svg')!.getBoundingClientRect();
              return (!previous || previous.right <= buttonBounds.left + 0.01)
                && icon.left >= buttonBounds.left && icon.right <= buttonBounds.right
                && icon.top >= buttonBounds.top && icon.bottom <= buttonBounds.bottom;
            }),
            buttonsFit: Array.from(nav.querySelectorAll('button')).every(button => {
              const buttonBounds = button.getBoundingClientRect();
              return buttonBounds.left >= bounds.left && buttonBounds.right <= bounds.right;
            }),
            windowControls: Array.from(header.querySelectorAll('.titlebar__window-btn')).map(button => {
              const buttonBounds = button.getBoundingClientRect();
              const icon = button.querySelector('svg')!.getBoundingClientRect();
              return { width: buttonBounds.width, height: buttonBounds.height, iconWidth: icon.width, iconHeight: icon.height };
            }),
          };
        });
        expect(layout.scrollWidth, `${theme} at ${width}px`).toBeLessThanOrEqual(layout.clientWidth);
        expect(layout.height).toBeLessThan(40);
        expect(layout.buttonsFit).toBe(true);
        expect(layout.navRight).toBeLessThanOrEqual(layout.controlsLeft);
        expect(layout.controlsRight).toBeLessThanOrEqual(width + 1);
        expect(layout.accountContentsFit).toBe(true);
        expect(layout.windowControlsSeparated).toBe(true);
        if (desktop) {
          baseline ??= layout.windowControls;
          expect(layout.windowControls).toHaveLength(3);
          layout.windowControls.forEach((control, index) => {
            for (const key of ['width', 'height', 'iconWidth', 'iconHeight'] as const) {
              expect(control[key]).toBeCloseTo(baseline![index][key], 2);
            }
          });
        }
      }
      await page.getByRole('button', { name: 'User: Not signed in', exact: true }).click();
      await expect(page.getByRole('dialog', { name: 'Lemonade Account', exact: true })).toBeVisible();
      await expect(page.getByRole('button', { name: 'Toggle theme', exact: true })).toBeVisible();
      await page.keyboard.press('Escape');
      await page.getByRole('button', { name: 'App controls', exact: true }).click();
      await expect(page.getByRole('button', { name: 'Open download manager', exact: true })).toBeVisible();
      await page.keyboard.press('Escape');
    }
  });
}
