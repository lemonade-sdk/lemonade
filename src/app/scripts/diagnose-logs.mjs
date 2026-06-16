/**
 * diagnose-logs.mjs — Playwright script to capture console/network/WebSocket
 * activity when the LogViewer component mounts.
 *
 * Usage:  npx playwright test --config=playwright.config.ts scripts/diagnose-logs.mjs
 *    or:  node scripts/diagnose-logs.mjs   (standalone, requires playwright installed)
 */

import { chromium } from 'playwright';

const APP_URL = process.env.APP_URL || 'http://localhost:8080';
const WAIT_MS = 10_000;

(async () => {
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  const page = await context.newPage();

  const consoleMessages = [];
  const pageErrors = [];
  const requestFailures = [];
  const wsEvents = [];

  page.on('console', msg => {
    consoleMessages.push({ type: msg.type(), text: msg.text() });
  });

  page.on('pageerror', err => {
    pageErrors.push(err.message);
  });

  page.on('requestfailed', req => {
    requestFailures.push({ url: req.url(), failure: req.failure()?.errorText });
  });

  page.on('websocket', ws => {
    const url = ws.url();
    wsEvents.push({ type: 'created', url });
    ws.on('frameerror', err => wsEvents.push({ type: 'frameerror', url, error: err }));
    ws.on('close', () => wsEvents.push({ type: 'close', url }));
    ws.on('framesent', frame => wsEvents.push({ type: 'sent', url, data: frame.payload?.toString().substring(0, 200) }));
    ws.on('framereceived', frame => wsEvents.push({ type: 'received', url, data: frame.payload?.toString().substring(0, 200) }));
  });

  console.log(`[diagnose] Opening ${APP_URL}...`);
  await page.goto(APP_URL, { waitUntil: 'networkidle' });

  // Try to navigate to the Logs view — look for a nav link/button
  const logsNav = page.locator('[data-nav="logs"], a[href*="logs"], button:has-text("Logs")');
  if (await logsNav.count() > 0) {
    console.log('[diagnose] Clicking Logs nav...');
    await logsNav.first().click();
  } else {
    console.log('[diagnose] No Logs nav found, checking if LogViewer is already visible...');
  }

  console.log(`[diagnose] Waiting ${WAIT_MS / 1000}s for connection attempts...`);
  await page.waitForTimeout(WAIT_MS);

  // Check visible status indicator
  const statusLabel = await page.locator('.logs-status__label').textContent().catch(() => 'not found');

  console.log('\n══════════════════════════════════════════');
  console.log('         DIAGNOSTIC RESULTS');
  console.log('══════════════════════════════════════════\n');

  console.log('── Status indicator:', statusLabel);

  console.log('\n── Console messages:');
  consoleMessages.forEach(m => console.log(`  [${m.type}] ${m.text}`));

  console.log('\n── Page errors:');
  pageErrors.forEach(e => console.log(`  ${e}`));

  console.log('\n── Request failures:');
  requestFailures.forEach(r => console.log(`  ${r.url} → ${r.failure}`));

  console.log('\n── WebSocket events:');
  wsEvents.forEach(e => console.log(`  [${e.type}] ${e.url}${e.data ? ' | ' + e.data : ''}${e.error ? ' | ERR: ' + e.error : ''}`));

  await browser.close();
  console.log('\n[diagnose] Done.');
})();
