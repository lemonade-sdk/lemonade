import { chromium } from 'playwright';
import { fileURLToPath } from 'url';
import path from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const screenshotsDir = path.resolve(__dirname, '..', 'screenshots');
const prefix = process.argv[2] || 'before';

async function main() {
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    deviceScaleFactor: 2,
  });
  const page = await context.newPage();

  await page.goto('http://localhost:8080', { waitUntil: 'networkidle' });
  await page.waitForTimeout(2000);

  await page.screenshot({ path: path.join(screenshotsDir, `${prefix}-chat-mobile.png`), fullPage: true });
  console.log(`Saved: ${prefix}-chat-mobile.png`);

  // Navigate to Models page
  const modelsBtn = page.locator('button[title="Models"], button[aria-label="Models"]').first();
  if (await modelsBtn.count() > 0) {
    await modelsBtn.click();
  } else {
    const navButtons = page.locator('.titlebar__nav button');
    if (await navButtons.count() >= 2) {
      await navButtons.nth(1).click();
    }
  }
  await page.waitForTimeout(1500);

  await page.screenshot({ path: path.join(screenshotsDir, `${prefix}-models-mobile.png`), fullPage: true });
  console.log(`Saved: ${prefix}-models-mobile.png`);

  await browser.close();
  console.log('Done!');
}

main().catch(e => { console.error(e); process.exit(1); });
