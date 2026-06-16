import { chromium } from 'playwright';
import { fileURLToPath } from 'url';
import path from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const screenshotsDir = path.resolve(__dirname, '..', 'screenshots');

async function main() {
  const browser = await chromium.launch({ headless: true });

  // --- Mobile screenshots (390x844, iPhone 14) ---
  const mobileCtx = await browser.newContext({
    viewport: { width: 390, height: 844 },
    deviceScaleFactor: 2,
  });
  const mobilePage = await mobileCtx.newPage();
  await mobilePage.goto('http://localhost:8080', { waitUntil: 'networkidle' });
  await mobilePage.waitForTimeout(2000);

  // Sheet closed
  await mobilePage.screenshot({ path: path.join(screenshotsDir, 'sheet-closed-mobile.png') });
  console.log('Saved: sheet-closed-mobile.png');

  // Open the bottom sheet by clicking the mobile trigger
  const trigger = mobilePage.locator('.chat__mobile-rail-trigger');
  if (await trigger.count() > 0) {
    await trigger.click();
    await mobilePage.waitForTimeout(400);
  }

  await mobilePage.screenshot({ path: path.join(screenshotsDir, 'sheet-open-mobile.png') });
  console.log('Saved: sheet-open-mobile.png');

  await mobileCtx.close();

  // --- Desktop screenshot (1280x800) ---
  const desktopCtx = await browser.newContext({
    viewport: { width: 1280, height: 800 },
    deviceScaleFactor: 1,
  });
  const desktopPage = await desktopCtx.newPage();
  await desktopPage.goto('http://localhost:8080', { waitUntil: 'networkidle' });
  await desktopPage.waitForTimeout(2000);

  await desktopPage.screenshot({ path: path.join(screenshotsDir, 'sheet-desktop-check.png') });
  console.log('Saved: sheet-desktop-check.png');

  await desktopCtx.close();
  await browser.close();
  console.log('Done!');
}

main().catch(e => { console.error(e); process.exit(1); });
