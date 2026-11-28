import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { expect, test } from '@playwright/test';

declare global {
  interface Window {
    __cspViolations: string[];
    __untrustedScriptRan?: boolean;
  }
}

test('simulated Tauri CSP permits renderer startup but blocks untrusted inline scripts in Chromium', async ({ page }) => {
  const config = JSON.parse(readFileSync(join(__dirname, '../src-tauri/tauri.conf.json'), 'utf8'));
  const directives: Record<string, string> = config.app.security.csp;
  expect(directives).toBeTruthy();

  await page.addInitScript(() => {
    window.__cspViolations = [];
    document.addEventListener('securitypolicyviolation', event => {
      window.__cspViolations.push(event.effectiveDirective);
    });
  });
  await page.route('**/app.css', route => route.fulfill({
    contentType: 'text/css',
    body: ':root { --lemonade-full-styles-loaded: 1; }',
  }));
  await page.route('/', async route => {
    const response = await route.fetch();
    const html = await response.text();
    const hashes = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/gi)]
      .map(match => `'sha256-${createHash('sha256').update(match[1].replace(/\r\n?/g, '\n')).digest('base64')}'`);
    const disabled = config.app.security.dangerousDisableAssetCspModification;
    const policy = Object.entries(directives)
      .map(([name, value]) => {
        const injected = name === 'script-src' ? ` ${hashes.join(' ')}`
          : name === 'style-src' && !disabled?.includes('style-src') ? " 'nonce-test'" : '';
        return `${name} ${value}${injected}`;
      })
      .join('; ');
    await route.fulfill({
      response,
      body: html,
      headers: { ...response.headers(), 'content-security-policy': policy },
    });
  });

  await page.goto('/');
  await expect(page.locator('#lemonade-app-css')).toHaveAttribute('rel', 'stylesheet');
  await expect(page.locator('#root')).not.toBeEmpty();
  const dynamicStyleApplied = await page.evaluate(() => {
    const style = document.createElement('style');
    style.textContent = '#csp-style-test { color: rgb(1, 2, 3); }';
    document.head.appendChild(style);
    const element = document.createElement('div');
    element.id = 'csp-style-test';
    document.body.appendChild(element);
    return getComputedStyle(element).color;
  });
  expect(dynamicStyleApplied).toBe('rgb(1, 2, 3)');
  await expect.poll(() => page.evaluate(() => window.__cspViolations)).toEqual([]);

  await page.evaluate(() => {
    const script = document.createElement('script');
    script.textContent = 'window.__untrustedScriptRan = true';
    document.head.appendChild(script);
  });
  await expect.poll(() => page.evaluate(() => window.__cspViolations)).toContain('script-src-elem');
  expect(await page.evaluate(() => window.__untrustedScriptRan)).toBeUndefined();
});
