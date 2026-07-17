/**
 * screenshot-presets.mjs
 *
 * One-off Playwright script that captures the Presets UI at desktop and mobile
 * viewports. Outputs PNGs to docs/screenshots/presets/ for the design audit.
 *
 * Usage:
 *   node prototype/ui-redesign/scripts/screenshot-presets.mjs
 *
 * Requirements:
 *   - webpack-dev-server already running at http://localhost:8080
 *   - @playwright/test installed in prototype/ui-redesign/node_modules
 */

import { chromium } from '@playwright/test';
import path from 'path';
import { fileURLToPath } from 'url';
import fs from 'fs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const OUT_DIR = path.resolve(__dirname, '..', 'docs', 'screenshots', 'presets');
const BASE_URL = 'http://localhost:8080';

const DESKTOP = { width: 1440, height: 900 };
const MOBILE = { width: 390, height: 844 };

fs.mkdirSync(OUT_DIR, { recursive: true });

async function shot(page, name) {
  const outPath = path.join(OUT_DIR, name);
  await page.screenshot({ path: outPath, fullPage: false });
  console.log(`  ✓ ${name}`);
}

async function navigateToPresets(page) {
  // Nav button with text "Presets" (icon-only on mobile; text shows on desktop)
  const navBtn = page.locator('button[title="Presets"], button[aria-label="Presets"]').first();
  const count = await navBtn.count();
  if (count > 0) {
    await navBtn.click();
  } else {
    // Fallback: find a nav button containing text Presets
    const textBtn = page.locator('nav button, .titlebar__nav button').filter({ hasText: 'Presets' }).first();
    const fallbackCount = await textBtn.count();
    if (fallbackCount > 0) {
      await textBtn.click();
    } else {
      console.warn('  ⚠ Could not find Presets nav button; relying on URL hash');
    }
  }
  await page.waitForTimeout(600);
}

async function runDesktop(browser) {
  console.log('\n── Desktop (1440×900) ──────────────────────────');
  const ctx = await browser.newContext({ viewport: DESKTOP });
  const page = await ctx.newPage();

  await page.goto(BASE_URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(800);

  // 01 – full page on load (Chat view default)
  await shot(page, '00-initial-load-desktop.png');

  // Navigate to Presets
  await navigateToPresets(page);
  await shot(page, '01-presets-grid-desktop.png');

  // 02 – hover first starter card
  const firstCard = page.locator('.recipe-card').first();
  await firstCard.hover();
  await page.waitForTimeout(200);
  await shot(page, '03-starter-card-hover.png');

  // 03 – click a starter card (opens slideover as read-only)
  await firstCard.click();
  await page.waitForTimeout(400);
  await shot(page, '04-starter-slideover-readonly.png');

  // 04 – check for edit affordances on the starter slideover
  // Document whether Save/Edit inputs are present or disabled
  const saveBtn = page.locator('.slideover__foot button').filter({ hasText: /Save/i });
  const saveBtnCount = await saveBtn.count();
  const titleInput = page.locator('.slideover__title-input');
  const titleInputCount = await titleInput.count();
  const cloneBtn = page.locator('[data-recipe-clone]');
  const cloneBtnCount = await cloneBtn.count();

  console.log(`  ℹ Save button visible: ${saveBtnCount > 0}, Title editable: ${titleInputCount > 0}, Clone btn: ${cloneBtnCount > 0}`);
  await shot(page, '04-starter-edit-attempt.png');

  // Close slideover
  const closeBtn = page.locator('.slideover__close').first();
  if (await closeBtn.count() > 0) await closeBtn.click();
  await page.waitForTimeout(300);

  // 05 – click Default card
  const defaultCard = page.locator('[data-recipe-id="s-default"]').first();
  if (await defaultCard.count() > 0) {
    await defaultCard.click();
    await page.waitForTimeout(400);
    await shot(page, '05-default-preset-slideover.png');
    if (await closeBtn.count() > 0) await closeBtn.click();
    await page.waitForTimeout(300);
  }

  // 06 – create new custom preset
  const newBtn = page.locator('button').filter({ hasText: '+ New Preset' }).first();
  if (await newBtn.count() > 0) {
    await newBtn.click();
    await page.waitForTimeout(500);
    await shot(page, '05-custom-preset-create.png');
    if (await closeBtn.count() > 0) await closeBtn.click();
    await page.waitForTimeout(300);
  }

  // 07 – all starter cards visible (scroll to show grid)
  await shot(page, '06-starter-cards-all-desktop.png');

  // 08 – "Your presets" empty state
  const emptyState = page.locator('[data-empty="yours"]');
  if (await emptyState.count() > 0) {
    await emptyState.scrollIntoViewIfNeeded();
    await page.waitForTimeout(200);
    await shot(page, '07-your-presets-empty-state.png');
  }

  // 09 – Models page showing recipe badges
  const modelsBtn = page.locator('button[title="Models"], button[aria-label="Models"]').first();
  const modelsBtnCount = await modelsBtn.count();
  if (modelsBtnCount > 0) {
    await modelsBtn.click();
  } else {
    const textModelsBtn = page.locator('nav button, .titlebar__nav button').filter({ hasText: 'Models' }).first();
    if (await textModelsBtn.count() > 0) await textModelsBtn.click();
  }
  await page.waitForTimeout(800);
  await shot(page, '08-models-page-recipe-badges.png');

  await ctx.close();
}

async function runMobile(browser) {
  console.log('\n── Mobile (390×844) ────────────────────────────');
  const ctx = await browser.newContext({
    viewport: MOBILE,
    deviceScaleFactor: 2,
    isMobile: true,
    hasTouch: true,
  });
  const page = await ctx.newPage();

  await page.goto(BASE_URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(800);

  await navigateToPresets(page);
  await shot(page, '02-presets-grid-mobile.png');

  // Hover not meaningful on mobile, but screenshot cards
  const firstCard = page.locator('.recipe-card').first();
  if (await firstCard.count() > 0) {
    await firstCard.tap();
    await page.waitForTimeout(500);
    await shot(page, '09-starter-slideover-mobile.png');
    const closeBtn = page.locator('.slideover__close').first();
    if (await closeBtn.count() > 0) await closeBtn.tap();
    await page.waitForTimeout(300);
  }

  await shot(page, '10-presets-grid-mobile-after-close.png');
  await ctx.close();
}

(async () => {
  console.log('screenshot-presets.mjs — Lemonade Presets UI Audit');
  console.log(`Output dir: ${OUT_DIR}`);

  const browser = await chromium.launch({ headless: true });

  try {
    await runDesktop(browser);
    await runMobile(browser);
  } finally {
    await browser.close();
  }

  const files = fs.readdirSync(OUT_DIR).filter(f => f.endsWith('.png'));
  console.log(`\n✅ Done — ${files.length} screenshots in docs/screenshots/presets/`);
  files.forEach(f => console.log(`   ${f}`));
})();
