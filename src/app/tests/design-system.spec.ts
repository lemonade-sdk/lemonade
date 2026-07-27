/* Regression guards for the DESIGN.md contracts that are cheap to verify but
 * easy to break silently: light-theme contrast on identity colors, and the
 * modal behavior every mobile context rail is supposed to share. */
import { test, expect } from '@playwright/test';

/* Every identity and monitoring token, per DESIGN.md's color table. Tokens that
 * alias a text or accent role (--cap-chat, --backend-other) are covered by the
 * roles they point at. */
const IDENTITY_TOKENS = [
  '--cap-vision', '--cap-code', '--cap-embedding', '--cap-reranking', '--cap-image',
  '--cap-image-edit', '--cap-audio', '--cap-audio-generation', '--cap-tts', '--cap-model3d',
  '--provider-hugging-face-fg', '--provider-modelscope-fg',
  '--backend-llamacpp', '--backend-vllm', '--backend-flm', '--backend-ryzenai',
  '--backend-sd-cpp', '--backend-whispercpp', '--backend-moonshine', '--backend-kokoro',
  '--backend-acestep', '--backend-thinksound', '--backend-openmoss', '--backend-trellis',
  '--backend-collection-omni', '--backend-collection-router', '--backend-collection',
  '--chart-series-1', '--chart-series-2', '--chart-series-3', '--chart-series-4',
  '--chart-series-5', '--chart-series-6', '--chart-cache',
];

async function contrastFailures(page: import('@playwright/test').Page, tokens: string[]) {
  return page.evaluate((names: string[]) => {
    const probe = document.createElement('span');
    document.body.appendChild(probe);
    const parse = (value: string) => {
      probe.style.color = '';
      probe.style.color = value;
      const match = getComputedStyle(probe).color.match(/[\d.]+/g);
      return match ? [Number(match[0]), Number(match[1]), Number(match[2])] : null;
    };
    const luminance = ([r, g, b]: number[]) => {
      const channel = (v: number) => {
        v /= 255;
        return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
      };
      return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
    };
    const root = getComputedStyle(document.documentElement);
    const background = parse(root.getPropertyValue('--surface-base').trim());
    const failures: string[] = [];
    for (const name of names) {
      const color = parse(root.getPropertyValue(name).trim());
      if (!color || !background) { failures.push(`${name}: unresolved`); continue; }
      const a = luminance(color);
      const b = luminance(background);
      const ratio = (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
      if (ratio < 3) failures.push(`${name}: ${ratio.toFixed(2)}:1`);
    }
    probe.remove();
    return failures;
  }, tokens);
}

for (const theme of ['dark', 'light'] as const) {
  test(`identity and chart tokens clear 3:1 on the ${theme} canvas`, async ({ page }) => {
    await page.addInitScript(value => localStorage.setItem('lemonade_theme', value), theme);
    await page.goto('/#/dashboard/performance');
    await page.waitForSelector('[data-view="dashboard"]');

    const failures = await contrastFailures(page, IDENTITY_TOKENS);
    expect(
      failures,
      `WCAG 1.4.11 requires 3:1 for non-text UI components. Below threshold on ${theme}:\n${failures.join('\n')}`,
    ).toEqual([]);
  });
}

test('log filters open as a modal with backdrop, focus trap, Escape and focus return', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto('/#/dashboard/logs');
  await page.waitForSelector('.logs-main');

  const trigger = page.getByRole('button', { name: 'Open log filters', exact: true });
  await expect(trigger).toBeVisible();
  await trigger.click();

  await expect(page.getByRole('dialog', { name: 'Log filters' })).toBeVisible();
  await expect(page.locator('.logs-workspace .workspace-mobile-rail-backdrop')).toBeVisible();
  expect(await page.evaluate(() => {
    const panel = document.querySelector('.logs-rail');
    return !!panel && panel.contains(document.activeElement);
  }), 'focus must move inside the panel (useFocusTrap)').toBe(true);

  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog', { name: 'Log filters' })).toHaveCount(0);
  await expect(trigger).toBeFocused();
});

test('the log source list keeps the active filter reachable and reports what it hides', async ({ page }) => {
  await page.goto('/#/dashboard/logs');
  await page.waitForSelector('.logs-rail');
  await expect.poll(() => page.locator('.logs-sources .workspace-filter-list__item').count())
    .toBeGreaterThan(1);

  // "All sources" plus at most the ten most active.
  const items = page.locator('.logs-sources .workspace-filter-list__item');
  expect(await items.count()).toBeLessThanOrEqual(11);

  const note = page.locator('.workspace-filter-list__note');
  if (await note.count()) {
    await expect(note).toContainText(/sources? not shown/);
  }

  // Selecting a source keeps it rendered and marked active.
  const selected = items.nth(1);
  const label = await selected.locator('.workspace-filter-list__label').textContent();
  await selected.click();
  const active = page.locator('.logs-sources .workspace-filter-list__item.is-active');
  await expect(active).toHaveCount(1);
  await expect(active.locator('.workspace-filter-list__label')).toHaveText(label ?? '');
});

test('the workspace resizer advertises the width range it can actually reach', async ({ page }) => {
  await page.setViewportSize({ width: 1000, height: 900 });
  await page.goto('/#/models');
  await page.waitForSelector('.model-list-panel');

  const resizer = page.locator('.workspace-panel-resizer').first();
  const [min, max, now] = await Promise.all([
    resizer.getAttribute('aria-valuemin'),
    resizer.getAttribute('aria-valuemax'),
    resizer.getAttribute('aria-valuenow'),
  ]);
  expect(Number(now)).toBeGreaterThanOrEqual(Number(min));
  expect(Number(now)).toBeLessThanOrEqual(Number(max));

  // End must land on the advertised maximum, not a stale constant.
  await resizer.focus();
  await page.keyboard.press('End');
  await expect.poll(async () => {
    const value = await resizer.getAttribute('aria-valuenow');
    const ceiling = await resizer.getAttribute('aria-valuemax');
    return value === ceiling;
  }).toBe(true);
});
