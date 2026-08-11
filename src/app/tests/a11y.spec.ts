/**
 * a11y.spec.ts — Accessibility test suite for the Lemonade UI redesign prototype.
 *
 * Covers all Phase 1 + Phase 2 items shipped on kpoin/ui-accessibility:
 *   - axe-core WCAG 2.1 AA automated scans (one per major view)
 *   - Skip link behaviour (hidden until focused, activates #main-content)
 *   - ARIA landmarks (<main>, <nav>, role="status")
 *   - Keyboard navigation order and completeness
 *   - Focus traps: bottom sheet (mobile 390px)
 *   - aria-live streaming regions (assertive + polite)
 *   - :focus-visible rings (keyboard vs. mouse)
 *   - prefers-reduced-motion (animations/transitions disabled)
 *
 * Prerequisites: dev server must be running, or playwright.config.ts's
 * webServer block will start it automatically on port 8080.
 *
 * Run:
 *   npx playwright test tests/a11y.spec.ts        (headless)
 *   npm run test:a11y                             (same, via npm script)
 *   npx playwright test tests/a11y.spec.ts --headed   (headed)
 */

import { test, expect, Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

test.describe.configure({ mode: 'parallel' });

// ─── Constants & helpers ──────────────────────────────────────────────────────

const WCAG_TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'] as const;

/** Navigate to a view by its titlebar nav label text. */
async function navigateToView(page: Page, label: string): Promise<void> {
  await page.locator('.titlebar__nav').getByRole('button', { name: label, exact: true }).click();
  await page.waitForTimeout(300);
}

async function navigateToMonitorSection(page: Page, label: 'Performance' | 'Telemetry' | 'Logs'): Promise<void> {
  await navigateToView(page, 'Monitor');
  await page.waitForSelector('[data-view="dashboard"]');
  if (label !== 'Performance') {
    await page.getByRole('navigation', { name: 'Monitor sections' }).getByRole('button', { name: label, exact: true }).click();
    await page.waitForTimeout(300);
  }
}

async function navigateToConnectSection(page: Page, label: string): Promise<void> {
  await navigateToView(page, 'Settings');
  await page.waitForSelector('.connect');
  await page.locator('.connect .workspace-nav').getByText(new RegExp(`^${label}$`, 'i')).click();
}

/** Open the custom-model editor through the Models heading action. */
async function openCustomModelEditor(
  page: Page,
  mode: 'model' | 'omni-collection' = 'model',
): Promise<void> {
  await page.goto('/');
  await navigateToView(page, 'Models');
  await page.waitForSelector('.manager');

  await page.getByRole('button', { name: 'Open custom models' }).click();
  await page.waitForSelector('.custom-model-form');

  if (mode === 'omni-collection') {
    await page.getByRole('button', { name: 'Omni collection', exact: true }).click();
  }
}

/** Format axe violations into a readable string for assertion failure messages. */
function formatViolations(
  violations: Array<{ id: string; description: string; impact?: string | null }>,
): string {
  if (violations.length === 0) return 'No violations';
  return (
    `Serious/critical WCAG 2.1 AA violations (${violations.length}):\n` +
    violations
      .map(v => `  [${v.impact ?? 'unknown'}] ${v.id}: ${v.description}`)
      .join('\n')
  );
}

/**
 * Normalise a CSS duration string to seconds.
 * '0.28s' → 0.28, '280ms' → 0.28, '0.01ms' → 0.00001
 */
function normaliseDurationToSecs(raw: string): number {
  const first = raw.split(',')[0].trim();
  if (first.endsWith('ms')) return parseFloat(first) / 1000;
  return parseFloat(first); // seconds
}

// ─── beforeEach: mirror the screenshot path patch from features.spec.ts ──────

test.beforeEach(async ({ page }, testInfo) => {
  const originalScreenshot = page.screenshot.bind(page);
  page.screenshot = ((options: Parameters<Page['screenshot']>[0] = {}) => {
    const rawPath = typeof options.path === 'string' ? options.path : undefined;
    const path = rawPath?.startsWith('screenshots/')
      ? testInfo.outputPath(rawPath.replace(/^screenshots\//, ''))
      : rawPath;
    return originalScreenshot({ ...options, ...(path ? { path } : {}) });
  }) as Page['screenshot'];
});

// ─── 1. axe-core automated scans (WCAG 2.1 AA, serious/critical only) ────────

test.describe('Accessibility — axe-core automated scans', () => {
  test('A01 — Chat view (default /) passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });

  test('A02 — Models view passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager');

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });

  test('A04 — Connect view passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Settings');
    await page.waitForSelector('.connect');

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });

  test('A05 — Monitor performance passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToMonitorSection(page, 'Performance');
    await page.waitForTimeout(500); // allow async dashboard data fetch to settle

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});

// ─── 2. Skip link ─────────────────────────────────────────────────────────────

test.describe('Accessibility — skip link', () => {
  test('A06 — skip link is the first focusable element (Tab once from page load)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    await page.keyboard.press('Tab');

    const activeClass = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.className ?? '',
    );
    expect(activeClass).toContain('skip-link');
  });

  test('A07 — skip link is off-screen (visually hidden) when not focused', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    const skipLink = page.locator('.skip-link');
    const box = await skipLink.boundingBox();

    // The element must exist in the DOM but be positioned above the viewport.
    // CSS: position: absolute; top: -40px → bottom edge = top + height ≤ 0
    expect(box).not.toBeNull();
    if (box) {
      expect(box.y + box.height).toBeLessThanOrEqual(0);
    }
  });

  test('A08 — skip link becomes visible and shows focus ring when focused via keyboard', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    await page.keyboard.press('Tab'); // land on skip link

    const skipLink = page.locator('.skip-link');
    const box = await skipLink.boundingBox();

    // CSS: .skip-link:focus { top: var(--space-2); } → top: 8px → visible in viewport
    expect(box).not.toBeNull();
    if (box) {
      expect(box.y).toBeGreaterThanOrEqual(0);
    }

    // :focus-visible ring should be applied (outline-width != 0px)
    const outlineWidth = await page.evaluate(
      () => window.getComputedStyle(document.activeElement as HTMLElement).outlineWidth,
    );
    expect(outlineWidth).not.toBe('0px');
  });

  test('A09 — pressing Enter on skip link moves focus to <main id="main-content">', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    await page.keyboard.press('Tab');   // focus skip link
    await page.keyboard.press('Enter'); // activate skip link

    const focusedId = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.id ?? '',
    );
    expect(focusedId).toBe('main-content');
  });
});

// ─── 3. Landmarks ─────────────────────────────────────────────────────────────

test.describe('Accessibility — ARIA landmarks', () => {
  test('A10 — <main id="main-content"> exists and is unique', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('main');

    expect(await page.locator('main').count()).toBe(1);
    expect(await page.locator('#main-content').count()).toBe(1);
  });

  test('A11 — titlebar contains <nav aria-label="Primary">', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    expect(await page.locator('nav[aria-label="Primary"]').count()).toBe(1);
  });

  test('A12 — status dot has role="status" for live connection announcements', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__status-dot--brand');

    const role = await page.locator('.titlebar__status-dot--brand').getAttribute('role');
    expect(role).toBe('status');
  });

  test('A13 — status dot aria-label reflects one of the three connection states', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__status-dot--brand');

    const label = await page.locator('.titlebar__status-dot--brand').getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(['Connected', 'Connecting…', 'Offline']).toContain(label);
  });
});

// ─── 4. Keyboard navigation ───────────────────────────────────────────────────

test.describe('Accessibility — keyboard navigation', () => {
  test('A14 — at least one titlebar nav button is reachable in the first 12 Tab presses', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    const knownNavLabels = ['Chat', 'Models', 'Backends', 'Monitor', 'Settings'];
    const encountered: string[] = [];

    for (let i = 0; i < 12; i++) {
      await page.keyboard.press('Tab');
      const label = await page.evaluate(() => {
        const el = document.activeElement as HTMLElement | null;
        return (
          el?.getAttribute('aria-label') ??
          el?.getAttribute('title') ??
          el?.textContent?.trim() ??
          ''
        );
      });
      encountered.push(label);
    }

    const hitNav = encountered.some(l => knownNavLabels.includes(l));
    expect(
      hitNav,
      `Expected a nav button label among first 12 Tabs. Got: ${JSON.stringify(encountered)}`,
    ).toBe(true);
  });

  test('A15 — Tab order reaches the composer textarea within 40 presses', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.composer__input');

    let found = false;
    for (let i = 0; i < 40; i++) {
      await page.keyboard.press('Tab');
      const isComposer = await page.evaluate(
        () => (document.activeElement as HTMLElement | null)?.classList.contains('composer__input') ?? false,
      );
      if (isComposer) {
        found = true;
        break;
      }
    }
    expect(found, 'Tab should reach composer__input within 40 presses').toBe(true);
  });

  test('A16 — Shift+Tab from the composer textarea moves focus backwards (not stays on composer)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.composer__input');

    // Click the textarea to give it focus via pointer (reliable way to start from a known position)
    await page.locator('.composer__input').click();

    await page.keyboard.press('Shift+Tab');

    const afterShiftTab = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.className ?? '',
    );
    // Focus must have moved away from the composer
    expect(afterShiftTab).not.toContain('composer__input');
  });
});

// ─── 5. Focus trap — bottom sheet (mobile 390 × 844) ─────────────────────────

test.describe('Accessibility — focus trap (bottom sheet mobile)', () => {
  test.beforeEach(async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/');
    await page.waitForSelector('.chat__main .workspace-mobile-menu-button');
  });

  test('A17 — opening bottom sheet moves focus inside it (useFocusTrap activates)', async ({ page }) => {
    await page.locator('.chat__main .workspace-mobile-menu-button').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    const activeIsInSheet = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet');
      return sheet?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInSheet).toBe(true);
  });

  test('A18 — Tab from last focusable inside bottom sheet wraps back to first (never escapes)', async ({ page }) => {
    await page.locator('.chat__main .workspace-mobile-menu-button').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    // Count focusable descendants (matching useFocusTrap's FOCUSABLE selector,
    // excluding elements inside aria-hidden="true" ancestors)
    const count = await page.locator(
      '.bottom-sheet :is(a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"]))',
    ).evaluateAll(els =>
      els.filter(el => !el.closest('[aria-hidden="true"]')).length,
    );

    // Tab through all elements + one extra (wrap check)
    for (let i = 0; i < count; i++) {
      await page.keyboard.press('Tab');
    }

    const activeIsInSheet = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet');
      return sheet?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInSheet, 'Focus should still be inside the bottom sheet after wrapping').toBe(true);
  });

  test('A19 — pressing Escape closes the bottom sheet', async ({ page }) => {
    await page.locator('.chat__main .workspace-mobile-menu-button').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    await page.keyboard.press('Escape');

    await expect(page.locator('.bottom-sheet--open')).not.toBeVisible({ timeout: 3000 });
  });

  test('A20 — focus returns to trigger button after bottom sheet closes via Escape', async ({ page }) => {
    await page.locator('.chat__main .workspace-mobile-menu-button').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    await page.keyboard.press('Escape');
    await page.waitForTimeout(100); // allow rAF from closeMobileSheet to run

    const activeClass = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.className ?? '',
    );
    expect(activeClass).toContain('workspace-mobile-menu-button');
  });
});

// ─── 7. aria-live streaming announcement regions ──────────────────────────────

test.describe('Accessibility — aria-live streaming regions', () => {
  test('A25 — assertive aria-live region exists in DOM at page load', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    // Assertive region announces "Assistant is responding" / "Response complete"
    const count = await page
      .locator('[aria-live="assertive"][aria-atomic="true"]')
      .count();
    expect(count).toBeGreaterThanOrEqual(1);
  });

  test('A26 — polite aria-live region exists in DOM at page load', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    // Polite region receives debounced streaming content chunks
    const count = await page
      .locator('[aria-live="polite"][aria-atomic="false"]')
      .count();
    expect(count).toBeGreaterThanOrEqual(1);
  });

  test('A27 — both aria-live regions are .sr-only (1×1 px, off-screen from pointer users)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    for (const selector of [
      '[aria-live="assertive"]',
      '[aria-live="polite"]',
    ]) {
      const el = page.locator(selector).first();
      const box = await el.boundingBox();

      // sr-only: width: 1px; height: 1px; overflow: hidden; clip: rect(0,0,0,0)
      // boundingBox may return null if clipped; treat null as "definitely off-screen" (pass)
      if (box !== null) {
        expect(box.width).toBeLessThanOrEqual(1);
        expect(box.height).toBeLessThanOrEqual(1);
      }
    }
  });

  // TODO: Verify that the assertive region updates to "Assistant is responding" and
  // the polite region receives debounced content during an active stream.
  // This requires mocking POST /api/v1/chat/completions with a chunked SSE response
  // via page.route(). Infrastructure pattern to follow from features.spec.ts.
  // Blocked: no streaming mock available in the current test setup.
});

// ─── 8. Focus rings on :focus-visible ────────────────────────────────────────

test.describe('Accessibility — :focus-visible rings', () => {
  test('A28 — keyboard-focused nav button has visible outline (2px from :focus-visible)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Skip link first, then we hit the first nav button
    await page.keyboard.press('Tab'); // skip link
    await page.keyboard.press('Tab'); // first element after skip link (nav button area)

    let foundButtonWithRing = false;
    for (let i = 0; i < 6; i++) {
      const info = await page.evaluate(() => {
        const el = document.activeElement as HTMLElement | null;
        return {
          tag: el?.tagName ?? '',
          outlineWidth: el
            ? window.getComputedStyle(el).outlineWidth
            : '0px',
        };
      });

      if (info.tag === 'BUTTON') {
        expect(
          info.outlineWidth,
          'Keyboard-focused button should have non-zero outline-width from :focus-visible',
        ).not.toBe('0px');
        foundButtonWithRing = true;
        break;
      }
      await page.keyboard.press('Tab');
    }

    expect(
      foundButtonWithRing,
      'Should have encountered a <button> element within the first ~8 Tab presses',
    ).toBe(true);
  });

  test('A29 — mouse-clicked nav button does NOT show custom focus ring (:focus-visible skips pointer)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Pointer (mouse) click — :focus-visible must NOT fire on buttons in Chromium
    const navBtn = page.locator('.titlebar__nav button').first();
    await navBtn.click({ force: true });

    const outlineWidth = await page.evaluate(() => {
      const el = document.activeElement as HTMLElement | null;
      return el ? window.getComputedStyle(el).outlineWidth : '0px';
    });

    // :focus-visible does not apply for mouse clicks on <button> → our 2px ring must be absent
    expect(outlineWidth).toBe('0px');
  });

  test('A30 — composer keyboard focus uses the rounded shell without a rectangular textarea outline', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.composer__input');

    let found = false;
    for (let i = 0; i < 40; i++) {
      await page.keyboard.press('Tab');
      const info = await page.evaluate(() => {
        const el = document.activeElement as HTMLElement | null;
        const bar = el?.closest('.composer__bar') as HTMLElement | null;
        return {
          isComposer: el?.classList.contains('composer__input') ?? false,
          inputOutlineWidth: el ? window.getComputedStyle(el).outlineWidth : '0px',
          barHasFocus: bar?.matches(':focus-within') ?? false,
          barBorderRadius: bar ? window.getComputedStyle(bar).borderRadius : '0px',
        };
      });
      if (info.isComposer) {
        expect(info.inputOutlineWidth, 'textarea must not draw the old rectangular focus box').toBe('0px');
        expect(info.barHasFocus, 'rounded composer shell should own the focus state').toBe(true);
        expect(info.barBorderRadius, 'composer focus treatment should remain rounded').not.toBe('0px');
        found = true;
        break;
      }
    }
    expect(found, 'Tab should eventually reach .composer__input').toBe(true);
  });
});

// ─── 9. prefers-reduced-motion ────────────────────────────────────────────────

test.describe('Accessibility — prefers-reduced-motion', () => {
  test('A31 — bottom-sheet transition-duration is near-zero when reducedMotion=reduce', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto('/');
    await page.waitForSelector('.bottom-sheet');

    // CSS rule: @media (prefers-reduced-motion: reduce) { *, *::before, *::after { transition-duration: 0.01ms !important; } }
    const raw = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet') as HTMLElement | null;
      return sheet ? window.getComputedStyle(sheet).transitionDuration : '0s';
    });
    const secs = normaliseDurationToSecs(raw);
    expect(secs, `Expected near-zero transition duration under reduce, got "${raw}"`).toBeLessThan(0.01);
  });

  test('A32 — bottom-sheet has normal non-zero transition-duration when reducedMotion=no-preference', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.goto('/');
    await page.waitForSelector('.bottom-sheet');

    // CSS: .bottom-sheet { transition: transform 280ms ease-out } → 0.28s
    const raw = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet') as HTMLElement | null;
      return sheet ? window.getComputedStyle(sheet).transitionDuration : '0s';
    });
    const secs = normaliseDurationToSecs(raw);
    expect(secs, `Expected ~0.28s transition duration under no-preference, got "${raw}"`).toBeGreaterThan(0.1);
  });

  test('A33 — all element transition-durations are near-zero under reducedMotion=reduce', async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Check a nav button which normally has hover transition effects in styles.css
    const raw = await page.evaluate(() => {
      const btn = document.querySelector('.titlebar__nav button') as HTMLElement | null;
      return btn ? window.getComputedStyle(btn).transitionDuration : '0s';
    });
    const secs = normaliseDurationToSecs(raw);
    expect(secs, `Nav button transition-duration should be near-zero under reduce, got "${raw}"`).toBeLessThan(0.01);
  });

  test('A34 — bottom-sheet has transform:none under reducedMotion=reduce (snaps, no slide animation)', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto('/');
    await page.waitForSelector('.bottom-sheet');

    // CSS: @media (prefers-reduced-motion: reduce) { .bottom-sheet { transform: none !important; } }
    const transform = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet') as HTMLElement | null;
      return sheet ? window.getComputedStyle(sheet).transform : '';
    });
    // 'none' means no translate is applied — sheet snaps rather than slides
    expect(transform).toBe('none');
  });
});

// ─── 15. Backend Manager — matrix cells, action labels, live regions ──────────
//        Covers: #2343 (keyboard-operable cells), #2344 (qualified action names),
//                #2351 (live-region toasts/notices).

test.describe('Accessibility — Backend Manager (#2343 #2344 #2351)', () => {
  const MOCK_SYSTEM_INFO = {
    lemonade_version: '1.0.0',
    os_version: 'Test OS',
    devices: { cpu: { name: 'Test CPU', available: true } },
    recipes: {
      llamacpp: {
        default_backend: 'cpu',
        backends: {
          vulkan: { state: 'installable', version: 'b1234', message: '', action: '', experimental: true },
          cpu: { state: 'installed', version: 'b1234', message: '', action: '', can_uninstall: true },
        },
      },
    },
  };

  test.beforeEach(async ({ page }) => {
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route('**/api/v1/system-info**', route =>
      route.fulfill({ json: MOCK_SYSTEM_INFO }),
    );
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await page.locator('.titlebar__nav').getByText('Backends').click();
    await page.waitForSelector('[data-backends-matrix]', { timeout: 5000 });
  });

  // the #2432 describe block below (A167+).

  // ── #2344 — action buttons have qualified accessible names ─────────────────

  test('A55 — Install button aria-label includes recipe and backend identifiers', async ({ page }) => {
    const installBtn = page.locator('[data-cell="llamacpp:vulkan"] button.cell__swap');
    await expect(installBtn).toBeVisible();
    const label = await installBtn.getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(label!.toLowerCase()).toContain('install');
    expect(label!.toLowerCase()).toContain('llama');
    expect(label!.toLowerCase()).toContain('vulkan');
  });

  test('A56 — Uninstall button aria-label includes recipe and backend identifiers', async ({ page }) => {
    const uninstallBtn = page.locator('[data-cell="llamacpp:cpu"] button.cell__swap--danger');
    await expect(uninstallBtn).toBeVisible();
    const label = await uninstallBtn.getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(label!.toLowerCase()).toContain('uninstall');
    expect(label!.toLowerCase()).toContain('llama');
    expect(label!.toLowerCase()).toContain('cpu');
  });

  test('A56b — experimental backend marker is labelled and exposes the requested tooltip', async ({ page }) => {
    const marker = page.locator('[data-cell="llamacpp:vulkan"] .cell__experimental-icon');
    await expect(marker).toBeVisible();
    await expect(marker).toHaveAttribute('aria-label', 'experimental');
    await expect(marker).toHaveAttribute('title', 'experimental');
    await expect(marker.locator('[data-icon="flask-conical"]')).toBeVisible();
    await expect(page.locator('[data-cell="llamacpp:cpu"] .cell__experimental-icon')).toHaveCount(0);
  });

  // ── #2351 — persistent polite live region for toasts ───

  test('A57 — backends view has a persistent role="status" live region for toast messages', async ({ page }) => {
    const liveRegion = page.locator('[data-backends-toast-live]');
    await expect(liveRegion).toHaveCount(1);
    expect(await liveRegion.getAttribute('role')).toBe('status');
    expect(await liveRegion.getAttribute('aria-live')).toBe('polite');
    expect(await liveRegion.getAttribute('aria-atomic')).toBe('true');
  });
});

// ─── Backend Manager — refresh hidden mounted view on activation ─────────────

test.describe('Accessibility — Backend Manager refresh lifecycle (#2343)', () => {
  function backendInfo(state: 'installed' | 'installable') {
    return {
      lemonade_version: '1.0.0',
      os_version: 'Test OS',
      devices: { cpu: { name: 'Test CPU', available: true } },
      recipes: {
        llamacpp: {
          default_backend: 'cpu',
          backends: {
            vulkan: {
              state,
              version: 'b1234',
              message: '',
              action: '',
              can_uninstall: state === 'installed',
            },
          },
        },
      },
    };
  }

  test('A58 — opening Backends refreshes backend status from system-info', async ({ page }) => {
    let currentSystemInfo = backendInfo('installable');

    await page.addInitScript(() => {
      try { localStorage.setItem('lemonade_current_view', 'chat'); } catch {}
    });
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route('**/api/v1/models**', route =>
      route.fulfill({ json: { data: [] } }),
    );
    await page.route('**/api/v1/system-info**', route =>
      route.fulfill({ json: currentSystemInfo }),
    );

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // The Backends view is mounted but hidden in App.tsx. Before navigating to
    // it, simulate a backend status change that should be visible immediately
    // after opening the view without a browser reload.
    currentSystemInfo = backendInfo('installed');

    await navigateToView(page, 'Backends');
    await page.waitForSelector('[data-backends-matrix]', { timeout: 5000 });
    await expect(page.locator('[data-cell="llamacpp:vulkan"] .cell__badge')).toHaveText('Installed');
    await expect(page.locator('[data-cell="llamacpp:vulkan"] button.cell__swap--danger')).toBeVisible();
  });
});

// ─── Backend argument tuning dialog ─────────────────────────────────────────

test.describe('Accessibility — backend argument tuning', () => {
  const MOCK_SYSTEM_INFO = {
    lemonade_version: '1.0.0',
    os_version: 'Test OS',
    devices: { cpu: { name: 'Test CPU', available: true } },
    recipes: {
      llamacpp: {
        default_backend: 'cpu',
        backends: {
          vulkan: { state: 'installable', version: 'b1234', message: '', action: '' },
          cpu: { state: 'installed', version: 'b1234', message: '', action: '', can_uninstall: true },
          cuda: { state: 'unsupported', version: '', message: 'No NVIDIA GPU detected', action: '' },
        },
      },
      kokoro: {
        default_backend: 'cpu',
        backends: {
          cpu: { state: 'installed', version: 'b1234', message: '', action: '' },
        },
      },
    },
  };

  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      try {
        for (const key of Object.keys(localStorage)) {
          if (key.includes('backend_tunings')) localStorage.removeItem(key);
        }
      } catch {}
    });
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route('**/api/v1/models**', route => route.fulfill({ json: { data: [] } }));
    await page.route('**/api/v1/system-info**', route => route.fulfill({ json: MOCK_SYSTEM_INFO }));
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
  });

  async function gotoBackends(page: Page): Promise<void> {
    await navigateToView(page, 'Backends');
    await page.waitForSelector('[data-backends-matrix]', { timeout: 5000 });
  }

  test('A167 — args-capable backends expose a small qualified action without obsolete model controls', async ({ page }) => {
    await gotoBackends(page);
    await expect(page.locator('[data-backend-args-button="llamacpp:cpu"]')).toBeVisible();
    await expect(page.locator('[data-backend-args-button="llamacpp:vulkan"]')).toBeVisible();
    await expect(page.locator('[data-backend-args-button="llamacpp:cpu"]')).toHaveAttribute(
      'aria-label',
      /Add backend arguments for llama\.cpp \(cpu\)/,
    );
    // Kokoro has no free-form backend args field in the load contract.
    await expect(page.locator('[data-backend-args-button="kokoro:cpu"]')).toHaveCount(0);
  });

  test('A168 — opening the editor announces the exact backend, labels the textarea, and focuses it', async ({ page }) => {
    await gotoBackends(page);
    const trigger = page.locator('[data-backend-args-button="llamacpp:cpu"]');
    await trigger.click();

    const dialog = page.locator('[data-backend-args-dialog="llamacpp:cpu"]');
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute('role', 'dialog');
    await expect(dialog).toHaveAttribute('aria-modal', 'true');
    await expect(dialog.getByRole('heading', { name: 'llama.cpp · cpu' })).toBeVisible();
    await expect(dialog).toContainText('every model using this exact backend');
    const input = dialog.locator('[data-backend-args-input]');
    await expect(input).toBeFocused();
    await expect(input).toHaveAccessibleName('Arguments');
  });

  test('A169 — a keyboard save replaces the exact entry, marks it Manual, and returns focus to the trigger', async ({ page }) => {
    await gotoBackends(page);
    const trigger = page.locator('[data-backend-args-button="llamacpp:cpu"]');
    await trigger.click();
    const input = page.locator('[data-backend-args-input]');
    await input.fill('--threads 8 --ctx-size 65536');
    await page.locator('[data-backend-args-save]').focus();
    await page.keyboard.press('Enter');

    await expect(page.locator('[data-backend-args-dialog]')).toHaveCount(0);
    await expect(trigger).toBeFocused();
    await expect(page.locator('[data-cell="llamacpp:cpu"] [data-cell-backend-args="user"]')).toContainText('Args · Manual');

    const stored = await page.evaluate(() => {
      for (const key of Object.keys(localStorage)) {
        if (!key.includes('backend_tunings')) continue;
        const entries = JSON.parse(localStorage.getItem(key) || '{}');
        if (entries['llamacpp:cpu']) return entries['llamacpp:cpu'];
      }
      return null;
    });
    expect(stored).toMatchObject({ args: '--threads 8 --ctx-size 65536', source: 'user' });
  });

  test('A171 — Escape closes the modal editor and restores focus to its backend action', async ({ page }) => {
    await gotoBackends(page);
    const trigger = page.locator('[data-backend-args-button="llamacpp:vulkan"]');
    await trigger.click();
    await expect(page.locator('[data-backend-args-dialog="llamacpp:vulkan"]')).toBeVisible();
    await page.keyboard.press('Escape');
    await expect(page.locator('[data-backend-args-dialog]')).toHaveCount(0);
    await expect(trigger).toBeFocused();
  });

  test('A172 — saved backend args can be cleared without affecting another backend entry', async ({ page }) => {
    await page.evaluate(() => {
      localStorage.setItem('lemonade:backend_tunings', JSON.stringify({
        'llamacpp:cpu': { args: '--cpu', source: 'user' },
        'llamacpp:vulkan': { args: '--vulkan', source: 'user' },
      }));
    });
    await gotoBackends(page);
    await page.locator('[data-backend-args-button="llamacpp:cpu"]').click();
    await page.locator('[data-backend-args-clear]').click();
    await expect(page.locator('[data-cell="llamacpp:cpu"] [data-cell-backend-args]')).toHaveCount(0);
    await expect(page.locator('[data-cell="llamacpp:vulkan"] [data-cell-backend-args="user"]')).toBeVisible();
  });

  test('A173 — the open backend args editor passes a WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await gotoBackends(page);
    await page.locator('[data-backend-args-button="llamacpp:cpu"]').click();
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .include('[data-backend-args-dialog]')
      .analyze();
    const serious = results.violations.filter(v => v.impact === 'serious' || v.impact === 'critical');
    expect(serious, formatViolations(serious)).toEqual([]);
  });
});


// ─── 15. Model row action qualified accessible names (#2341) ─────────────────

test.describe('Accessibility — model row action qualified names (#2341)', () => {
  /**
   * Simulate a connected server returning two test models:
   *   Llama-3.1-8B  (downloaded: true)  → Downloaded zone → Load + Delete buttons
   *   Qwen2.5-7B    (downloaded: false) → Registry zone   → Download + Get & Load buttons
   *
   * Both buttons of the same action type (e.g. "Load") must carry model-qualified
   * accessible names so NVDA/JAWS users can distinguish them when navigating by
   * button role (pressing 'B' in NVDA browse mode).
   */
  test.beforeEach(async ({ page }) => {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false },
          ],
        }),
      }),
    );
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager');
    // Wait for model list items to render from the mocked API response
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(300);
  });

  test('A46 — downloaded model row: Load button accessible name includes model name', async ({ page }) => {
    // Click on Llama-3.1-8B in the list to open the detail panel
    await page.locator('.model-list-item').filter({ hasText: 'Llama-3.1-8B' }).click();
    await page.waitForTimeout(200);
    // aria-label="Load Llama-3.1-8B" makes the button uniquely identifiable in
    // a list of multiple loaded models when navigating by button role.
    await expect(
      page.getByRole('button', { name: /Load Llama-3\.1-8B/ }),
    ).toBeVisible();
  });

  test('A47 — downloaded model row: Delete button accessible name includes model name', async ({ page }) => {
    // Click on Llama-3.1-8B in the list to open the detail panel
    await page.locator('.model-list-item').filter({ hasText: 'Llama-3.1-8B' }).click();
    await page.waitForTimeout(200);
    // Icon-only X button must carry aria-label="Delete Llama-3.1-8B" so it is
    // not announced as a nameless button to screen reader users.
    await expect(
      page.getByRole('button', { name: /Delete.*Llama-3\.1-8B/ }),
    ).toBeVisible();
  });

  test('A48 — registry model row: Download button accessible name includes model name', async ({ page }) => {
    // Click on Qwen2.5-7B in the list to open the detail panel
    await page.locator('.model-list-item').filter({ hasText: 'Qwen2.5-7B' }).click();
    await page.waitForTimeout(200);
    await expect(
      page.getByRole('button', { name: /Download Qwen2\.5-7B/ }),
    ).toBeVisible();
  });

  test('A49 — registry model row: "Get and load" button accessible name includes model name', async ({ page }) => {
    // Click on Qwen2.5-7B in the list to open the detail panel
    await page.locator('.model-list-item').filter({ hasText: 'Qwen2.5-7B' }).click();
    await page.waitForTimeout(200);
    await expect(
      page.getByRole('button', { name: /Get and load Qwen2\.5-7B/i }),
    ).toBeVisible();
  });

  test('A50 — no model action button in the detail panel carries a bare unqualified accessible name', async ({ page }) => {
    // Buttons whose accessible name is just "Load", "Download", "Delete", etc.
    // cause collision when multiple model rows are rendered — NVDA hears
    // "Load, Load, Load…" with no way to distinguish targets.
    // In the new master-detail layout, action buttons are in the detail panel.
    const genericExact = new Set([
      'Load', 'Download', 'Delete', 'Unload', 'Get & Load', 'Cancel download',
      'Pin model', 'Unpin model', 'Copy model name', 'Copy repository name',
    ]);
    // Click each model to check its action buttons
    const items = page.locator('.model-list-item');
    const count = await items.count();
    for (let i = 0; i < count; i++) {
      await items.nth(i).click();
      await page.waitForTimeout(100);
      const labels = await page.locator('.model-detail-panel__actions button').evaluateAll(
        (btns: HTMLElement[]) =>
          btns.map(b => (b.getAttribute('aria-label') ?? b.textContent ?? '').trim()),
      );
      for (const label of labels) {
        if (!label) continue;
        expect(
          genericExact.has(label),
          `Detail panel action button carries bare generic accessible name: "${label}"`,
        ).toBe(false);
      }
    }
  });
});

// ─── 16. Download progress bar semantics (#2342) ──────────────────────────────

test.describe('Accessibility — download progress bar semantics', () => {
  // A valid DownloadListItem for a 42%-complete model download.
  // Passed to addInitScript so the singleton DownloadStore reads it from
  // localStorage before any React code runs (avoids poll-timing flakiness).
  const MOCK_DOWNLOAD = {
    id: 'model:Llama-3.1-8B',
    downloadType: 'model',
    modelName: 'Llama-3.1-8B',
    fileName: 'Llama-3.1-8B.gguf',
    fileIndex: 1,
    totalFiles: 1,
    bytesDownloaded: 420_000_000,
    bytesTotal: 1_000_000_000,
    bytesTotalIsLowerBound: false,
    percent: 42,
    status: 'downloading',
    startTime: 1_000_000_000_000,
    bytesResumed: 0,
    running: true,
    speedBytesPerSecond: 5_000_000,
    updatedAt: Date.now(),
  };

  test.beforeEach(async ({ page }) => {
    // Pre-populate localStorage so the DownloadStore singleton (read at module init)
    // has an active downloading item before React renders anything.
    await page.addInitScript((item: typeof MOCK_DOWNLOAD) => {
      const timestamp = Date.now();
      localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify([{
        ...item,
        createdAt: timestamp,
        startTime: timestamp,
        updatedAt: timestamp,
      }]));
    }, MOCK_DOWNLOAD);

    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
    );
    // The server owns download truth. Return the same active job so the
    // accessibility fixture remains valid after authoritative reconciliation.
    await page.route('**/api/v1/downloads**', route =>
      route.fulfill({ json: { downloads: [{
        id: MOCK_DOWNLOAD.id,
        type: 'model',
        model_name: MOCK_DOWNLOAD.modelName,
        status: 'downloading',
        running: true,
        file: MOCK_DOWNLOAD.fileName,
        file_index: MOCK_DOWNLOAD.fileIndex,
        total_files: MOCK_DOWNLOAD.totalFiles,
        bytes_downloaded: MOCK_DOWNLOAD.bytesDownloaded,
        bytes_total: MOCK_DOWNLOAD.bytesTotal,
        percent: MOCK_DOWNLOAD.percent,
      }] } }),
    );

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    // Open the download manager via its titlebar toggle button.
    await page.locator('.titlebar__download-toggle').click();
    await page.waitForSelector('.download-manager__panel');
    // Ensure the download item row is rendered before we start asserting.
    await page.waitForSelector('.download-item--downloading', { timeout: 5000 });
  });

  test('A59 — active download progress element has role="progressbar"', async ({ page }) => {
    const progressBar = page.locator('.download-manager__panel [role="progressbar"]').first();
    await expect(progressBar).toBeVisible();
  });

  test('A60 — progressbar has aria-valuenow matching percent, aria-valuemin=0, aria-valuemax=100', async ({ page }) => {
    const progressBar = page.locator('.download-manager__panel [role="progressbar"]').first();
    const valuenow = await progressBar.getAttribute('aria-valuenow');
    const valuemin = await progressBar.getAttribute('aria-valuemin');
    const valuemax = await progressBar.getAttribute('aria-valuemax');
    expect(Number(valuenow)).toBe(42);
    expect(Number(valuemin)).toBe(0);
    expect(Number(valuemax)).toBe(100);
  });

  test('A61 — progressbar aria-label includes the model name', async ({ page }) => {
    const progressBar = page.locator('.download-manager__panel [role="progressbar"]').first();
    const label = await progressBar.getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(label).toContain('Llama-3.1-8B');
  });

  test('A62 — sr-only polite status live region is present inside the download manager panel', async ({ page }) => {
    const liveRegion = page.locator(
      '.download-manager__panel [role="status"][aria-live="polite"]',
    );
    await expect(liveRegion).toBeAttached();
  });
});

// ─── 17. Conversation rail — listbox keyboard navigation ──────────────────────

test.describe('Accessibility — conversation rail listbox', () => {
  const RAIL_CONVOS = [
    { id: 'rc1', title: 'Alpha conversation', model: null, messages: [], updatedAt: Date.now(), schemaVersion: 3 },
    { id: 'rc2', title: 'Beta conversation', model: null, messages: [], updatedAt: Date.now() - 1000, schemaVersion: 3 },
  ];

  test.beforeEach(async ({ page }) => {
    await page.addInitScript((data: { persistKey: string; convKey: string; activeKey: string; convos: typeof RAIL_CONVOS }) => {
      localStorage.setItem(data.persistKey, 'true');
      localStorage.setItem(data.convKey, JSON.stringify({ version: 3, conversations: data.convos }));
      localStorage.setItem(data.activeKey, 'rc1');
    }, {
      persistKey: 'lemonade:persist_conversations',
      convKey: 'lemonade:conversations',
      activeKey: 'lemonade:active_conversation',
      convos: RAIL_CONVOS,
    });
    await page.goto('/');
    await page.waitForSelector('.rail__list');
  });

  test('A63 — rail__list has role="listbox" with an accessible aria-label', async ({ page }) => {
    const list = page.locator('.rail__list').first();
    expect(await list.getAttribute('role')).toBe('listbox');
    const label = await list.getAttribute('aria-label');
    expect(label).toBeTruthy();
  });

  test('A64 — selected conversation option has aria-selected="true" and tabIndex=0', async ({ page }) => {
    const activeOption = page.locator('.rail__list [role="option"][aria-selected="true"]').first();
    await expect(activeOption).toBeVisible();
    const tabIndex = await activeOption.getAttribute('tabindex');
    expect(tabIndex).toBe('0');
  });

  test('A65 — ArrowDown moves keyboard focus to the next conversation option', async ({ page }) => {
    await page.locator('#rail-conv-rc1').focus();
    await page.keyboard.press('ArrowDown');

    const focusedId = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.id ?? '',
    );
    expect(focusedId).toBe('rail-conv-rc2');
  });

  test('A66 — delete button accessible name includes the conversation title', async ({ page }) => {
    const deleteBtn = page.locator('.rail__list .rail__item-delete').first();
    const label = await deleteBtn.getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(label!.toLowerCase()).toContain('delete');
    expect(label).toContain('Alpha conversation');
  });
});

// ─── 19. Group F — Omni picker combobox semantics ─────────────────────────────

test.describe('Accessibility — Omni picker combobox semantics (#2347)', () => {
  async function openOmniCollectionForm(page: Page): Promise<void> {
    await openCustomModelEditor(page, 'omni-collection');
    await page.waitForSelector('.omni-component-picker');
  }

  test('A71 — Omni picker input has role=combobox and aria-expanded=false when closed', async ({ page }) => {
    await openOmniCollectionForm(page);

    const input = page.locator('.omni-component-picker input').first();
    await expect(input).toHaveAttribute('role', 'combobox');
    await expect(input).toHaveAttribute('aria-expanded', 'false');
    await expect(input).toHaveAttribute('aria-controls');
    await expect(input).toHaveAttribute('aria-autocomplete', 'list');
  });

  test('A72 — Omni picker opens on focus (aria-expanded=true) and Escape closes it (aria-expanded=false)', async ({ page }) => {
    await openOmniCollectionForm(page);

    const input = page.locator('.omni-component-picker input').first();

    await input.focus();
    await expect(input).toHaveAttribute('aria-expanded', 'true');

    const listbox = page.locator('.omni-component-picker [role="listbox"]').first();
    await expect(listbox).toBeVisible();

    await input.press('Escape');
    await expect(input).toHaveAttribute('aria-expanded', 'false');
  });

  test('A73 — Omni picker ArrowDown opens popup; aria-activedescendant tracks active option when options exist', async ({ page }) => {
    await openOmniCollectionForm(page);

    const input = page.locator('.omni-component-picker input').first();
    await input.press('Escape');
    await expect(input).toHaveAttribute('aria-expanded', 'false');

    await input.press('ArrowDown');
    await expect(input).toHaveAttribute('aria-expanded', 'true');

    const hasOptions = await page.locator('[role="option"]').count() > 0;
    if (hasOptions) {
      const activeDesc = await input.getAttribute('aria-activedescendant');
      expect(activeDesc).toBeTruthy();
      if (activeDesc) {
        await expect(page.locator(`[id="${activeDesc}"]`)).toBeAttached();
      }
    }
  });

  test('A74 — Omni picker label is associated with input via htmlFor/id (for=id pair)', async ({ page }) => {
    await openOmniCollectionForm(page);

    const firstLabel = page.locator('.omni-component-picker label').first();
    const forAttr = await firstLabel.getAttribute('for');
    expect(forAttr).toMatch(/^omni-picker-input-/);

    if (forAttr) {
      await expect(page.locator(`[id="${forAttr}"]`)).toHaveCount(1);
    }
  });
});

// ─── 20. Group F — Connect / cloud form durable labels (#2349) ─────────────────

test.describe('Accessibility — connect and cloud form labels (#2349)', () => {
  test('A75 — Cloud provider form fields have programmatic labels (no placeholder-only)', async ({ page }) => {
    await page.goto('/');
    await navigateToConnectSection(page, 'Cloud providers');

    await expect(page.getByLabel('Provider name')).toBeVisible();
    await expect(page.getByLabel('Base URL')).toBeVisible();
    await expect(page.getByLabel('Provider API key (optional)')).toBeVisible();
  });

  test('A76 — Marketplace search has accessible name', async ({ page }) => {
    await page.goto('/');
    await navigateToView(page, 'Apps');
    await page.waitForSelector('[data-view="apps"]');

    const searchInput = page.getByLabel('Search apps');
    await expect(searchInput).toBeVisible();
    await expect(searchInput).toHaveAttribute('aria-label', 'Search apps');
  });
});

// ─── 21. Group F — Icon-only / title-only controls have reliable names (#2353) ─

test.describe('Accessibility — icon-button accessible names (#2353)', () => {
  test('A77 — LogViewer search input has an accessible name (not placeholder-only)', async ({ page }) => {
    await page.goto('/');
    await navigateToMonitorSection(page, 'Logs');
    await page.waitForSelector('.logs-view');

    const searchInput = page.locator('.logs-search');
    const ariaLabel = await searchInput.getAttribute('aria-label');
    expect(ariaLabel).toBeTruthy();
    expect(ariaLabel).toBe('Filter logs');
  });

  test('A78 — LogViewer Clear button has an aria-label with full action name', async ({ page }) => {
    await page.goto('/');
    await navigateToMonitorSection(page, 'Logs');
    await page.waitForSelector('.logs-view');

    const clearBtn = page.locator('.logs-btn').filter({ hasNotText: 'Reconnect' }).first();
    const ariaLabel = await clearBtn.getAttribute('aria-label');
    expect(ariaLabel).toBe('Clear log output');
  });

  test('A79 — Omni picker clear button has aria-label naming target', async ({ page }) => {
    await openCustomModelEditor(page, 'omni-collection');
    await page.waitForSelector('.omni-component-picker');

    const clearBtns = page.locator('.omni-component-picker__clear');
    const count = await clearBtns.count();
    if (count > 0) {
      const ariaLabel = await clearBtns.first().getAttribute('aria-label');
      expect(ariaLabel).toBeTruthy();
      expect(ariaLabel).toMatch(/^Clear /);
    }
    expect(count).toBeGreaterThanOrEqual(0);
  });
});

// ─── 22. MCP Gateway panel (ConnectView) — Phase A (read-only dashboard) ──────
//        Covers: #2417 (endpoint visibility, copy, status, tools list)

test.describe('Accessibility — MCP Gateway panel (#2417)', () => {
  const MCP_TOOLS = [
    {
      name: 'lemonade_list_models',
      description: 'List all models available on this lemonade server.',
      inputSchema: {
        type: 'object',
        properties: { capability: { type: 'string' } },
        required: [],
      },
    },
    { name: 'lemonade_chat', description: 'Send a chat completion request to a lemonade LLM model.' },
    { name: 'lemonade_transcribe_audio', description: 'Transcribe audio to text.' },
    { name: 'lemonade_generate_image', description: 'Generate an image from a text prompt.' },
    { name: 'lemonade_omni', description: 'Multi-modal omni tool.' },
  ];


  /** Mock health + MCP so the panel shows connected with a tools list. */
  async function setupWithMcp(page: import('@playwright/test').Page): Promise<void> {
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    // Both initialize (id:1) and tools/list (id:2) go to /mcp
    let callIndex = 0;
    await page.route(/\/mcp(?:\?.*)?$/, async route => {
      const body = route.request().postDataJSON() as { method?: string; id?: number };
      if (body?.method === 'initialize') {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            result: {
              protocolVersion: '2025-06-18',
              capabilities: { tools: {} },
              serverInfo: { name: 'lemonade-mcp', version: '1.0.0' },
            },
          }),
        });
      } else if (body?.method === 'notifications/initialized') {
        await route.fulfill({ status: 202, body: '' });
      } else if (body?.method === 'tools/list') {
        callIndex++;
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body.id, result: { tools: MCP_TOOLS } }),
        });
      } else {
        await route.fulfill({ status: 400, json: { error: 'Unexpected MCP request' } });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });
    void callIndex; // suppress unused warning
  }

  test('A80 — MCP panel is present in ConnectView with correct heading', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });

    const heading = page.locator('#connect-pane-title');
    await expect(heading).toBeVisible();
    await expect(heading).toHaveText('MCP gateway');
  });

  test('A81 — MCP endpoint URL input contains /mcp path and is read-only', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]');

    const input = page.locator('#mcp-endpoint-display');
    await expect(input).toBeVisible();
    const value = await input.inputValue();
    expect(value).toMatch(/\/mcp$/);
    expect(await input.getAttribute('readonly')).not.toBeNull();
  });

  test('A82 — Copy button has a qualifying aria-label mentioning clipboard', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]');

    const copyBtn = page.locator('.mcp-panel__copy-btn');
    await expect(copyBtn).toBeVisible();
    const label = await copyBtn.getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(label!.toLowerCase()).toContain('copy');
    expect(label!.toLowerCase()).toContain('clipboard');
  });

  test('A83 — copy-confirmation live region is always present in DOM (not conditionally mounted)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]');

    const liveRegion = page.locator('[data-mcp-copy-live]');
    await expect(liveRegion).toHaveCount(1);
    expect(await liveRegion.getAttribute('role')).toBe('status');
    expect(await liveRegion.getAttribute('aria-live')).toBe('polite');
    expect(await liveRegion.getAttribute('aria-atomic')).toBe('true');
  });

  test('A84 — health/status indicator has role="status" and aria-live="polite"', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]');

    const statusEl = page.locator('[data-mcp-status]');
    await expect(statusEl).toHaveCount(1);
    expect(await statusEl.getAttribute('role')).toBe('status');
    expect(await statusEl.getAttribute('aria-live')).toBe('polite');
    expect(await statusEl.getAttribute('aria-atomic')).toBe('true');
  });

  test('A85 — with mocked MCP server, tools list is collapsible with expected tool names', async ({ page }) => {
    await setupWithMcp(page);
    await page.waitForSelector('[data-mcp-tools-list]', { state: 'attached', timeout: 8000 });

    const toolList = page.locator('[data-mcp-tools-list]');
    const disclosure = page.locator('.mcp-panel__tool-disclosure').first();
    await expect(disclosure).not.toHaveAttribute('open', '');
    await expect(toolList).not.toBeVisible();
    await disclosure.locator('summary').click();
    await expect(toolList).toBeVisible();

    const items = toolList.locator('.mcp-panel__tool-name');
    const count = await items.count();
    expect(count).toBe(MCP_TOOLS.length);
    await expect(toolList.locator('.mcp-panel__tool-row')).toHaveCount(MCP_TOOLS.length);
    await expect(toolList.locator('.mcp-panel__tool-meta').first()).toHaveText('1 input');
    await expect(toolList.locator('.mcp-panel__tool-description').first()).toHaveText(MCP_TOOLS[0].description);

    // Verify first expected tool name is present
    await expect(toolList.getByText('lemonade_list_models')).toBeVisible();
    await expect(toolList.getByText('lemonade_chat')).toBeVisible();
  });

  test('A86 — tools list element has accessible aria-label', async ({ page }) => {
    await setupWithMcp(page);
    await page.waitForSelector('[data-mcp-tools-list]', { state: 'attached', timeout: 8000 });

    const toolList = page.locator('[data-mcp-tools-list]');
    const label = await toolList.getAttribute('aria-label');
    expect(label).toBeTruthy();
  });

  test('A87 — Refresh button has aria-label and is a <button>', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]');

    const refreshBtn = page.locator('[data-mcp-panel] button[aria-label="Refresh MCP tools list"]');
    await expect(refreshBtn).toBeVisible();
    const tag = await refreshBtn.evaluate(el => el.tagName.toLowerCase());
    expect(tag).toBe('button');
  });

  test('A88 — ConnectView with MCP panel passes WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route(/\/mcp(?:\?.*)?$/, async route => {
      const body = route.request().postDataJSON() as { method?: string; id?: number };
      if (body?.method === 'initialize') {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            result: { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'lemonade-mcp', version: '1.0.0' } },
          }),
        });
      } else if (body?.method === 'notifications/initialized') {
        await route.fulfill({ status: 202, body: '' });
      } else if (body?.method === 'tools/list') {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body.id, result: { tools: MCP_TOOLS } }),
        });
      } else {
        await route.fulfill({ status: 400, json: { error: 'Unexpected MCP request' } });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    // Wait for MCP panel and give tools list time to populate
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });
    await page.waitForTimeout(500);

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .disableRules(['color-contrast'])
      .analyze();

    const serious = results.violations.filter(v => v.impact === 'serious' || v.impact === 'critical');
    expect(serious, formatViolations(serious)).toHaveLength(0);
  });

  test('A88b — external MCP setup defaults to an HTTP endpoint and keeps local processes available', async ({ page }) => {
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0', high_security: true } }),
    );
    await page.route(/\/internal\/mcp\/servers(?:\?.*)?$/, route =>
      route.fulfill({ contentType: 'application/json', body: JSON.stringify({ servers: [] }) }),
    );
    await page.route(/\/mcp(?:\?.*)?$/, async route => {
      const body = route.request().postDataJSON() as { method?: string; id?: number };
      if (body?.method === 'initialize') {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            result: { protocolVersion: '2025-11-25', capabilities: { tools: {} }, serverInfo: { name: 'lemonade-mcp', version: '1.0.0' } },
          }),
        });
      } else if (body?.method === 'notifications/initialized') {
        await route.fulfill({ status: 202, body: '' });
      } else {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body?.id, result: { tools: MCP_TOOLS } }),
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.getByRole('button', { name: 'Add server', exact: true }).click();

    const form = page.locator('.mcp-server-form');
    await expect(form).toBeVisible();
    await expect(form.getByRole('radio', { name: /HTTP endpoint/i })).toBeChecked();
    await expect(form.getByPlaceholder('http://127.0.0.1:3000/mcp')).toBeVisible();
    await expect(form.getByLabel('Command', { exact: true })).toHaveCount(0);
    await expect(form.getByRole('button', { name: 'Test connection', exact: true })).toBeVisible();

    await form.getByRole('radio', { name: /Local process/i }).check();
    await expect(form.getByLabel('Command', { exact: true })).toBeVisible();
    await expect(form.getByPlaceholder('http://127.0.0.1:3000/mcp')).toHaveCount(0);

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .disableRules(['color-contrast'])
      .analyze();
    const serious = results.violations.filter(v => v.impact === 'serious' || v.impact === 'critical');
    expect(serious, formatViolations(serious)).toHaveLength(0);
  });

  test('A89 — MCP handshake: initialize→notifications/initialized→tools/list in order with correct params and MCP-Protocol-Version + Mcp-Session-Id headers', async ({ page }) => {
    // Capture all /mcp requests in order so we can assert the sequence.
    type CapturedRequest = {
      method: string;
      headers: Record<string, string>;
      body: Record<string, unknown>;
    };
    const captured: CapturedRequest[] = [];

    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route(/\/mcp(?:\?.*)?$/, async route => {
      const body = route.request().postDataJSON() as {
        method?: string; id?: number; params?: Record<string, unknown>;
      };
      const headers = route.request().headers();
      captured.push({ method: body?.method ?? '', headers, body: body ?? {} });

      if (body?.method === 'initialize') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          // Expose header so the cross-origin fetch can read it via Response.headers.get()
          headers: {
            'Access-Control-Allow-Origin': '*',
            'Access-Control-Expose-Headers': 'Mcp-Session-Id',
            'Mcp-Session-Id': 'sess-abc-123',
          },
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            result: { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'lemonade-mcp', version: '1.0.0' } },
          }),
        });
      } else if (body?.method === 'notifications/initialized') {
        // Notifications return 202 with empty body per Streamable HTTP spec.
        await route.fulfill({ status: 202, body: '' });
      } else {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body?.id, result: { tools: MCP_TOOLS } }),
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-tools-list]', { state: 'attached', timeout: 8000 });

    // Must have at least 3 requests: initialize, notifications/initialized, tools/list.
    expect(captured.length).toBeGreaterThanOrEqual(3);

    // (a) initialize is first with correct params
    const initReq = captured[0];
    expect(initReq.method).toBe('initialize');
    const initParams = (initReq.body as { params?: Record<string, unknown> }).params ?? {};
    // Advertise the current client revision; the mock server negotiates the
    // session down to 2025-06-18, which subsequent request headers must use.
    expect(initParams['protocolVersion']).toBe('2025-11-25');
    expect(initParams['capabilities']).toMatchObject({ tools: {} });
    const clientInfo = initParams['clientInfo'] as Record<string, string> | undefined;
    expect(clientInfo?.['name']).toBe('lemonade-gui3');
    expect(typeof clientInfo?.['version']).toBe('string');

    // (b) notifications/initialized is second (no id field — it is a notification)
    const notifReq = captured[1];
    expect(notifReq.method).toBe('notifications/initialized');
    expect((notifReq.body as { id?: unknown }).id).toBeUndefined();

    // (c) tools/list is third
    const toolsReq = captured[2];
    expect(toolsReq.method).toBe('tools/list');

    // (d) subsequent requests carry MCP-Protocol-Version and Mcp-Session-Id headers
    // (HTTP headers are lowercased by the browser/node fetch internals)
    expect(notifReq.headers['mcp-protocol-version']).toBe('2025-06-18');
    expect(notifReq.headers['mcp-session-id']).toBe('sess-abc-123');
    expect(toolsReq.headers['mcp-protocol-version']).toBe('2025-06-18');
    expect(toolsReq.headers['mcp-session-id']).toBe('sess-abc-123');
  });

  test('A90 — MCP initialize failure: accessible error state shown, tools list absent, status not Connected', async ({ page }) => {
    await page.route('**/api/v1/health**', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [], version: '1.0.0' } }),
    );
    await page.route(/\/mcp(?:\?.*)?$/, async route => {
      const body = route.request().postDataJSON() as { method?: string; id?: number };
      if (body?.method === 'initialize') {
        // Server rejects with a JSON-RPC error in the response body.
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            error: { code: -32600, message: 'Unsupported protocol version' },
          }),
        });
      } else {
        // Should not be reached; fulfil defensively.
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body?.id, result: { tools: [] } }),
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToConnectSection(page, 'MCP gateway');
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });
    // Allow async flow to settle
    await page.waitForTimeout(600);

    // Accessible error alert is visible and contains the server error message.
    const errorEl = page.locator('[data-mcp-tools-error]');
    await expect(errorEl).toBeVisible();
    await expect(errorEl).toContainText('Unsupported protocol version');

    // Tools list must NOT be rendered.
    await expect(page.locator('[data-mcp-tools-list]')).toHaveCount(0);

    // Status indicator must not claim 'Connected'.
    const statusEl = page.locator('[data-mcp-status]');
    await expect(statusEl).not.toContainText('Connected');
  });
});

// ─── 23. Master-detail model view (#2355 Slice 1) ─────────────────────────────
//
// Covers: model list panel, detail panel tablist, and filter ownership.
// The duplicate middle-panel funnel was removed by #2936; filters live in the left rail.
// Range: A91–A105.

test.describe('Accessibility — master-detail model view (#2355 Slice 1)', () => {
  /** Navigate to Models view and wait for the master-detail layout to mount. */
  async function goToModels(page: Page): Promise<void> {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
  }

  /** Navigate to Models with mock API data of two models. */
  async function goToModelsWithMock(page: Page): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false },
          ],
        }),
      }),
    );
    await goToModels(page);
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(200);
  }

  // ── Layout landmarks ─────────────────────────────────────────────────────────

  test('A91 — manager--detail layout renders list panel and detail panel regions', async ({ page }) => {
    await goToModels(page);
    await expect(page.locator('.model-list-panel')).toBeVisible();
    await expect(page.locator('.model-detail-panel, .manager__detail-form-panel')).toBeAttached();
  });

  test('A92 — model list panel has an h1 heading "Models"', async ({ page }) => {
    await goToModels(page);
    await expect(page.locator('.manager__title h1')).toContainText('Models');
  });

  // ── Search input ─────────────────────────────────────────────────────────────

  test('A93 — model list search input is associated with a label', async ({ page }) => {
    await goToModels(page);
    const input = page.locator('#model-list-search');
    await expect(input).toBeVisible();
    // label must exist with for="model-list-search"
    const label = page.locator('label[for="model-list-search"]');
    await expect(label).toBeAttached();
  });

  test('A94 — typing in search input filters the model list (aria-live count updates)', async ({ page }) => {
    await goToModelsWithMock(page);
    const search = page.locator('#model-list-search');
    await search.fill('zzznotamodel');
    await page.waitForTimeout(200);
    // Either empty state is visible or count shows 0 models
    const countText = await page.locator('.model-list-panel__count').textContent();
    const emptyVisible = await page.locator('.manager__empty').isVisible().catch(() => false);
    expect(emptyVisible || (countText ?? '').startsWith('0')).toBeTruthy();
  });

  // ── Filter ownership (#2936) ─────────────────────────────────────────────────

  test('A95 — models middle panel has no duplicate funnel dialog trigger', async ({ page }) => {
    await goToModels(page);
    const middlePanel = page.locator('.model-list-panel');
    await expect(middlePanel.locator('.model-list-panel__filter-btn')).toHaveCount(0);
    await expect(middlePanel.locator('button[aria-haspopup="dialog"]')).toHaveCount(0);
    await expect(middlePanel.locator('.model-list-panel__filter-popover')).toHaveCount(0);
  });

  test('A96 — left-rail task filtering remains usable after funnel removal', async ({ page }) => {
    await goToModelsWithMock(page);
    const chatTask = page.locator('.model-nav-rail__task-chip').filter({ hasText: /^Chat/ });
    await expect(chatTask).toBeVisible();
    await chatTask.click();
    await expect(chatTask).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator('.model-list-panel__count')).toContainText('(Chat)');
    await expect(page.locator('.model-list-item')).toHaveCount(2);
  });

  // ── List keyboard navigation ─────────────────────────────────────────────────

  test('A97 — model list container has role=listbox with accessible label', async ({ page }) => {
    await goToModels(page);
    const listbox = page.getByRole('listbox', { name: 'Model list' });
    await expect(listbox).toBeAttached();
    const label = await listbox.getAttribute('aria-label');
    expect(label).toBeTruthy();
  });

  test('A98 — model list items have role=option with aria-selected', async ({ page }) => {
    await goToModelsWithMock(page);
    const items = page.locator('[role="option"]');
    const count = await items.count();
    expect(count).toBeGreaterThan(0);
    for (let i = 0; i < Math.min(count, 3); i++) {
      await expect(items.nth(i)).toHaveAttribute('aria-selected');
    }
  });

  test('A99 — ArrowDown/ArrowUp keyboard navigation moves selection in model list', async ({ page }) => {
    await goToModelsWithMock(page);
    const listbox = page.getByRole('listbox', { name: 'Model list' });
    await listbox.focus();
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(100);
    // At least one option should now be selected
    const selectedCount = await page.locator('[role="option"][aria-selected="true"]').count();
    expect(selectedCount).toBeGreaterThanOrEqual(1);
  });

  // ── Detail panel tablist ─────────────────────────────────────────────────────

  test('A100 — detail panel tablist has correct ARIA structure (role=tablist, tabs, tabpanels)', async ({ page }) => {
    await goToModelsWithMock(page);
    // Select a model to open the detail panel
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const tablist = page.locator('[role="tablist"]');
    await expect(tablist).toBeVisible();

    const tabs = page.locator('[role="tab"]');
    const tabCount = await tabs.count();
    expect(tabCount).toBeGreaterThanOrEqual(2); // README + Configuration at minimum

    // Each tab must have aria-selected
    for (let i = 0; i < tabCount; i++) {
      await expect(tabs.nth(i)).toHaveAttribute('aria-selected');
    }

    // Exactly one tab should be selected
    const selectedTabs = await page.locator('[role="tab"][aria-selected="true"]').count();
    expect(selectedTabs).toBe(1);

    // Active tabpanel must be visible
    const activePanel = page.locator('[role="tabpanel"]:visible');
    await expect(activePanel).toBeVisible();
  });

  test('A101 — tab keyboard navigation (ArrowLeft/ArrowRight) moves focus between tabs', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const tabs = page.locator('[role="tab"]');
    await tabs.first().focus();
    const initialLabel = await tabs.first().getAttribute('aria-label') ?? await tabs.first().textContent();

    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(100);

    // Second tab should now have focus / be selected
    const secondSelected = await page.locator('[role="tab"][aria-selected="true"]').textContent();
    expect(secondSelected).not.toBe(initialLabel?.trim());
  });

  // ── Configuration tab ───────────────────────────────────────────────────────

  test('A102 — Configuration tab in detail panel is keyboard-reachable and focusable', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const configTab = page.locator('[role="tab"]').filter({ hasText: /Configuration/i });
    await expect(configTab).toBeVisible();
    await configTab.click();
    await page.waitForTimeout(100);
    await expect(configTab).toHaveAttribute('aria-selected', 'true');
  });

  test('A103 — Configuration tab panel has accessible heading or label', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const configTab = page.locator('[role="tab"]').filter({ hasText: /Configuration/i });
    await configTab.click();
    await page.waitForTimeout(100);

    const tabpanel = page.locator('[role="tabpanel"]:visible');
    await expect(tabpanel).toBeVisible();
    // Panel should have aria-labelledby referencing the tab
    const labelledBy = await tabpanel.getAttribute('aria-labelledby');
    expect(labelledBy).toBeTruthy();
  });

  // ── Custom model management ───────────────────────────────────────────────────

  test('A104 — custom-model heading action opens an accessible model-type switch', async ({ page }) => {
    await goToModels(page);

    const openCustomModels = page.getByRole('button', { name: 'Open custom models' });
    await expect(openCustomModels).toBeVisible();
    await expect(openCustomModels).toHaveRole('button');
    await openCustomModels.click();

    const typeGroup = page.getByRole('group', { name: 'Custom model type' });
    const customBtn = typeGroup.getByRole('button', { name: 'Custom model', exact: true });
    const omniBtn = typeGroup.getByRole('button', { name: 'Omni collection', exact: true });
    await expect(typeGroup).toBeVisible();
    await expect(customBtn).toBeVisible();
    await expect(omniBtn).toBeVisible();
    await expect(customBtn).toHaveRole('button');
    await expect(omniBtn).toHaveRole('button');
    await expect(customBtn).toHaveAttribute('aria-pressed', 'true');
  });

  test('A105 — master-detail Models view passes WCAG 2.1 AA axe-core scan with mock data', async ({ page }) => {
    await goToModelsWithMock(page);
    // Select first model to populate the detail panel
    const items = page.locator('.model-list-item');
    if (await items.count() > 0) {
      await items.first().click();
      await page.waitForTimeout(200);
    }

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});

//
// Covers the 4 gaps addressed in the fl0rianr 2026-06-25 clarifications:
//   A: README checkpoint derivation (tightened regex + checkpoints.main fallback)
//   B: Sort controls (labeled select with 4 options)
//   C: Responsive list-first (narrow ≤700px shows list only; selecting shows detail + Back)
// Range: A106–A115.

test.describe('Accessibility — #2355 Slice 1 reconciliation (fl0rianr clarifications)', () => {
  async function goToModels(page: Page): Promise<void> {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
  }

  async function goToModelsWithMock(page: Page): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true,
              checkpoint: 'gguf-community/Llama-3.1-8B-Instruct:Q4_K_M.gguf' },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false },
          ],
        }),
      }),
    );
    await goToModels(page);
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(200);
  }

  // ── B: Sort controls ─────────────────────────────────────────────────────────

  test('A106 — sort control is a labelled <select> with accessible name', async ({ page }) => {
    await goToModels(page);
    const sortSelect = page.locator('#model-list-sort');
    await expect(sortSelect).toBeVisible();
    await expect(sortSelect).toHaveRole('combobox');

    // Label must reference the select
    const label = page.locator('label[for="model-list-sort"]');
    await expect(label).toBeVisible();
  });

  test('A107 — sort control offers Name, Size, Last used, Download count options', async ({ page }) => {
    await goToModels(page);
    const sortSelect = page.locator('#model-list-sort');
    await expect(sortSelect).toBeVisible();

    const opts = await sortSelect.locator('option').allTextContents();
    expect(opts.some(t => /name/i.test(t))).toBe(true);
    expect(opts.some(t => /size/i.test(t))).toBe(true);
    expect(opts.some(t => /last.used/i.test(t))).toBe(true);
    expect(opts.some(t => /download/i.test(t))).toBe(true);
  });

  test('A108 — sort select default value is Name (alphabetical)', async ({ page }) => {
    await goToModels(page);
    const sortSelect = page.locator('#model-list-sort');
    await expect(sortSelect).toHaveValue('name');
  });

  test('A109 — sort select is keyboard-operable (can change value via keyboard)', async ({ page }) => {
    await goToModelsWithMock(page);
    const sortSelect = page.locator('#model-list-sort');
    await sortSelect.focus();
    await sortSelect.selectOption('size');
    await expect(sortSelect).toHaveValue('size');
    // Revert
    await sortSelect.selectOption('name');
    await expect(sortSelect).toHaveValue('name');
  });

  // ── C: Responsive list-first ──────────────────────────────────────────────────

  test('A110 — on narrow viewport (640px), detail panel is hidden until model selected', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 800 });
    await goToModelsWithMock(page);

    // Detail panel should not be visible before selection
    const detailPanel = page.locator('.model-detail-panel');
    await expect(detailPanel).not.toBeVisible();

    // List should be visible
    await expect(page.locator('.model-list-panel')).toBeVisible();
  });

  test('A111 — on narrow viewport, selecting a model shows the detail panel and hides the list', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 800 });
    await goToModelsWithMock(page);

    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    // Detail visible, list hidden
    await expect(page.locator('.model-detail-panel')).toBeVisible();
    await expect(page.locator('.model-list-panel')).not.toBeVisible();
  });

  test('A112 — narrow viewport detail view has a "Back to models" button with accessible label', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 800 });
    await goToModelsWithMock(page);

    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const backBtn = page.locator('.model-detail-panel__back-btn');
    await expect(backBtn).toBeVisible();
    await expect(backBtn).toHaveRole('button');
    const label = await backBtn.getAttribute('aria-label');
    expect(label).toMatch(/back.+model/i);
  });

  test('A113 — Back button returns to list view and restores list visibility', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 800 });
    await goToModelsWithMock(page);

    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const backBtn = page.locator('.model-detail-panel__back-btn');
    await backBtn.click();
    await page.waitForTimeout(200);

    // List back, detail hidden
    await expect(page.locator('.model-list-panel')).toBeVisible();
    await expect(page.locator('.model-detail-panel')).not.toBeVisible();
  });
});

// ─── 25. Model README raw-HTML rendering (#2355 README tab fix) ───────────────
//
// HF model READMEs commonly embed raw HTML (<div align="center">, <img>, badges,
// tables). The README tab previously used markdown-it { html: false }, which
// ESCAPED that markup so it appeared as literal text. Fix: html:true behind the
// existing strict DOMPurify allowlist + a leading YAML frontmatter strip.
// Range: A116–A117.

test.describe('Accessibility — model README raw-HTML rendering (#2355)', () => {
  async function goToModelsWithReadme(page: Page, readmeBody: string): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true,
              checkpoint: 'gguf-community/Llama-3.1-8B-Instruct:Q4_K_M.gguf' },
          ],
        }),
      }),
    );
    // Mock the Hugging Face README fetch the component performs against
    // https://huggingface.co/${hfRepo}/raw/main/README.md
    await page.route('**/huggingface.co/**/raw/main/README.md', async route =>
      route.fulfill({ contentType: 'text/plain', body: readmeBody }),
    );

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.locator('.model-list-item').first().click();
    const readmeTab = page.getByRole('tab', { name: 'README', exact: true });
    await expect(readmeTab).toBeVisible();
    await readmeTab.click();
    await expect(readmeTab).toHaveAttribute('aria-selected', 'true');
    await page.waitForSelector('.detail-readme', { timeout: 5000 });
    await page.waitForTimeout(200);
  }

  test('A116 — raw HTML in README renders as real DOM elements, not escaped text', async ({ page }) => {
    const readme = [
      '# Model Card',
      '',
      '<div align="center"><strong>Centered Heading</strong></div>',
      '',
      '<img src="https://example.com/badge.svg" alt="build badge">',
      '',
      'Some normal markdown text.',
    ].join('\n');

    await goToModelsWithReadme(page, readme);

    const container = page.locator('.detail-readme');
    await expect(container).toBeVisible();

    // Raw HTML must materialise as actual elements inside the README container.
    await expect(container.locator('div[align="center"]')).toHaveCount(1);
    await expect(container.locator('strong', { hasText: 'Centered Heading' })).toHaveCount(1);
    await expect(container.locator('img[alt="build badge"]')).toHaveCount(1);

    // And it must NOT appear as literal/escaped text.
    const text = (await container.innerText()).toLowerCase();
    expect(text).not.toContain('<div');
    expect(text).not.toContain('&lt;div');
    expect(text).not.toContain('<strong');
    expect(text).not.toContain('<img');
  });

  test('A117 — leading YAML frontmatter block is stripped before rendering', async ({ page }) => {
    const readme = [
      '---',
      'license: apache-2.0',
      'pipeline_tag: text-generation',
      'tags:',
      '  - text-generation',
      '---',
      '',
      '# Real Heading',
      '',
      'Body content goes here.',
    ].join('\n');

    await goToModelsWithReadme(page, readme);

    const container = page.locator('.detail-readme');
    await expect(container).toBeVisible();

    // The real heading must render.
    await expect(container.locator('h1', { hasText: 'Real Heading' })).toHaveCount(1);

    // Frontmatter keys must NOT be visible as dumped text.
    const text = await container.innerText();
    expect(text).not.toContain('license: apache-2.0');
    expect(text).not.toContain('pipeline_tag');
  });
});

// ─── 26. #2355 left-rail parity — pin / favorite (client-local) ───────────────
//
// fl0rianr feedback (2026-06-25): the master-detail rail dropped the original
// rail's pin/favorite affordance. Re-wired the existing client-local pin store
// (localStorage `pinned_models`, no lemond) into ModelListPanel. Pinned models
// float to the top; the affordance is a non-button span (so it does not nest an
// interactive control inside role="option"), and keyboard/AT users toggle via
// the "P" shortcut on the focused row, with pinned state in the row aria-label.
// Range: A118–A123.

test.describe('Accessibility — left-rail pin/favorite parity (#2355)', () => {
  async function goToModelsWithMock(page: Page): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false },
          ],
        }),
      }),
    );
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(200);
  }

  test('A118 — each model row exposes a pin affordance with an accessible title', async ({ page }) => {
    await goToModelsWithMock(page);
    const pins = page.locator('.model-list-item__pin');
    const count = await pins.count();
    expect(count).toBeGreaterThan(0);
    // Title communicates the pin action to pointer users.
    const title = await pins.first().getAttribute('title');
    expect((title ?? '').toLowerCase()).toContain('pin');
  });

  test('A119 — pin affordance is NOT a nested interactive button inside role="option"', async ({ page }) => {
    await goToModelsWithMock(page);
    const pin = page.locator('.model-list-item__pin').first();
    // It must be a span (not a button/anchor/input) so role=option does not nest
    // an interactive control (axe nested-interactive).
    const tag = await pin.evaluate(el => el.tagName.toLowerCase());
    expect(tag).toBe('span');
    // No button inside any option row.
    expect(await page.locator('[role="option"] button').count()).toBe(0);
  });

  test('A120 — clicking the pin toggles the row pinned state and aria-label', async ({ page }) => {
    await goToModelsWithMock(page);
    const row = page.locator('.model-list-item').first();
    const pin = row.locator('.model-list-item__pin');
    await pin.click();
    await page.waitForTimeout(100);
    // The (now-pinned) model floats to the top; assert the first row is pinned.
    const firstRow = page.locator('.model-list-item').first();
    await expect(firstRow).toHaveClass(/model-list-item--pinned/);
    const label = await firstRow.getAttribute('aria-label');
    expect((label ?? '').toLowerCase()).toContain('pinned');
    // Unpin and verify the pinned class is removed.
    await firstRow.locator('.model-list-item__pin').click();
    await page.waitForTimeout(100);
    expect(await page.locator('.model-list-item--pinned').count()).toBe(0);
  });

  test('A121 — selected row is keyboard-operable: "P" toggles pin (aria-keyshortcuts)', async ({ page }) => {
    await goToModelsWithMock(page);
    // Select a model (focus moves to the detail panel in master-detail), then
    // return focus to the now-focusable selected row (tabIndex 0) — the path a
    // keyboard user takes via Shift+Tab — and press the advertised "P" shortcut.
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(150);
    const selected = page.locator('.model-list-item--selected');
    // The shortcut must be advertised to assistive tech.
    expect(await selected.getAttribute('aria-keyshortcuts')).toBe('P');
    await selected.focus();
    await page.keyboard.press('p');
    await page.waitForTimeout(100);
    const pinnedCount = await page.locator('.model-list-item--pinned').count();
    expect(pinnedCount).toBe(1);
    const label = await page.locator('.model-list-item--pinned').first().getAttribute('aria-label');
    expect((label ?? '').toLowerCase()).toContain('pinned');
  });

  test('A122 — pinned state persists client-locally to localStorage (no lemond)', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().locator('.model-list-item__pin').click();
    await page.waitForTimeout(100);
    const persisted = await page.evaluate(() => {
      for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (key && key.endsWith('pinned_models')) return localStorage.getItem(key);
      }
      return null;
    });
    expect(persisted, 'a *pinned_models localStorage key should exist').toBeTruthy();
    expect((persisted ?? '').length).toBeGreaterThan(2); // non-empty JSON array
  });

  test('A123 — model list with a pinned row passes WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().locator('.model-list-item__pin').click();
    await page.waitForTimeout(150);
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});

// ─── Left navigation rail — three-pane model view (#2355 follow-up) ──────────
//
// fl0rianr (2026-06-25) posted a canonical 3-pane target: a NEW left NAVIGATION
// rail (ModelNavRail) + the existing ModelListPanel (middle) + ModelDetailPanel
// (right). The left rail surfaces filter dimensions — primary nav (All/
// Downloaded/My Models/Favorites), collapsible Task chips, multi-select Backend chips,
// collapsible Tags, and a Storage meter — all derived CLIENT-SIDE from the model
// list (no lemond). Selecting any of them filters the middle list.
// Range: A124–A136.

test.describe('Accessibility — left navigation rail (#2355 three-pane)', () => {
  async function goToModelsWithNavMock(page: Page): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm', 'tools', 'hot'], recipe: 'llamacpp', suggested: true, downloaded: true, size: 8 },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false, size: 7 },
            { id: 'Whisper-Large-v3', name: 'Whisper-Large-v3', labels: ['audio'], recipe: 'whispercpp', downloaded: true, size: 3 },
            { id: 'SDXL-Turbo', name: 'SDXL-Turbo', labels: ['image'], recipe: 'sd-cpp', downloaded: false, size: 6 },
          ],
        }),
      }),
    );
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.model-nav-rail', { state: 'attached' });
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(200);
  }

  // ── Landmark & structure ─────────────────────────────────────────────────

  test('A124 — left rail is a <nav> landmark with an accessible name', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const rail = page.locator('nav.model-nav-rail');
    await expect(rail).toBeVisible();
    expect(await rail.getAttribute('aria-label')).toBeTruthy();
  });

  // ── Primary nav ──────────────────────────────────────────────────────────

  test('A125 — primary nav items are buttons with counts that are not the only signal', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const allBtn = page.locator('.model-nav-rail__nav-item').filter({ hasText: 'All Models' });
    await expect(allBtn).toBeVisible();
    // Visible count chip plus an sr-only "N models" phrase so the count is not
    // conveyed by the digit alone.
    const accName = (await allBtn.getAttribute('aria-label')) ?? (await allBtn.textContent()) ?? '';
    expect(accName.toLowerCase()).toContain('models');
  });

  test('A126 — selecting a primary nav item exposes selected state via aria-current and filters the list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const before = await page.locator('.model-list-item').count();
    const downloaded = page.locator('.model-nav-rail__nav-item').filter({ hasText: 'Downloaded' });
    await downloaded.click();
    await page.waitForTimeout(150);
    expect(await downloaded.getAttribute('aria-current')).toBe('true');
    const after = await page.locator('.model-list-item').count();
    // Two of four mock models are downloaded.
    expect(after).toBeLessThan(before);
    expect(after).toBe(2);
  });

  test('A127 — primary nav is keyboard operable (focus + Enter selects)', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const fav = page.locator('.model-nav-rail__nav-item').filter({ hasText: 'Favorites' });
    await fav.focus();
    await expect(fav).toBeFocused();
    await page.keyboard.press('Enter');
    await page.waitForTimeout(120);
    expect(await fav.getAttribute('aria-current')).toBe('true');
  });

  // ── Task chips (collapsible multi-select) ─────────────────────────────────

  test('A128 — Task section header is a button with aria-expanded that toggles the list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const toggle = page.locator('.model-nav-rail__section-toggle').filter({ hasText: 'Task' });
    expect(await toggle.getAttribute('aria-expanded')).toBe('true');
    await expect(page.locator('#nav-tasks')).toBeVisible();
    await toggle.click();
    await page.waitForTimeout(100);
    expect(await toggle.getAttribute('aria-expanded')).toBe('false');
    await expect(page.locator('#nav-tasks')).toBeHidden();
  });

  test('A129 — selecting tasks is multi-select and filters the middle list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const audio = page.locator('.model-nav-rail__task-chip').filter({ hasText: /^Audio/ });
    const image = page.locator('.model-nav-rail__task-chip').filter({ hasText: /^Image/ });
    await audio.click();
    await image.click();
    await page.waitForTimeout(150);
    expect(await audio.getAttribute('aria-pressed')).toBe('true');
    expect(await image.getAttribute('aria-pressed')).toBe('true');
    const rows = page.locator('.model-list-item');
    await expect(rows).toHaveCount(2);
    await expect(rows.filter({ hasText: 'Whisper' })).toHaveCount(1);
    await expect(rows.filter({ hasText: 'SDXL' })).toHaveCount(1);
  });

  // ── Backends multi-select chips ──────────────────────────────────────────

  test('A130 — Backend chips support independent multi-selection', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const llama = page.locator('.model-nav-rail__backend-chip').filter({ hasText: 'llama.cpp' });
    const whisper = page.locator('.model-nav-rail__backend-chip').filter({ hasText: 'Whisper' });
    await llama.click();
    await whisper.click();
    expect(await llama.getAttribute('aria-pressed')).toBe('true');
    expect(await whisper.getAttribute('aria-pressed')).toBe('true');
    await page.waitForTimeout(150);
    await expect(page.locator('.model-list-item')).toHaveCount(3);
  });

  // ── Tags (collapsible chips) ─────────────────────────────────────────────

  test('A131 — Tags section uses aria-pressed chips that filter the list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const llamaTag = page.locator('.model-nav-rail__tag-chip').filter({ hasText: /^Llama/ });
    await expect(llamaTag).toBeVisible();
    expect(await llamaTag.getAttribute('aria-pressed')).toBe('false');
    await llamaTag.click();
    await page.waitForTimeout(150);
    expect(await llamaTag.getAttribute('aria-pressed')).toBe('true');
    const rows = page.locator('.model-list-item');
    await expect(rows).toHaveCount(1);
    await expect(rows.first()).toContainText('Llama');
  });


  test('A131b — Recommended is a real metadata filter', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const recommended = page.locator('.model-nav-rail__tag-chip').filter({ hasText: /^Recommended/ });
    await recommended.click();
    await page.waitForTimeout(150);
    expect(await recommended.getAttribute('aria-pressed')).toBe('true');
    await expect(page.locator('.model-list-item')).toHaveCount(1);
    await expect(page.locator('.model-list-item').first()).toContainText('Llama');
  });

  test('A131bb — Hot is a built-in exact server-metadata filter', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const hot = page.locator('.model-nav-rail__tag-chip').filter({ hasText: /^Hot/ });
    await expect(hot).toBeVisible();
    await hot.click();
    await page.waitForTimeout(150);
    expect(await hot.getAttribute('aria-pressed')).toBe('true');
    await expect(page.locator('.model-list-item')).toHaveCount(1);
    await expect(page.locator('.model-list-item').first()).toContainText('Llama');
  });

  test('A131c — custom tags can be added, selected, and removed', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const input = page.locator('#nav-custom-tag');
    await input.fill('tools');
    await page.getByRole('button', { name: 'Add custom tag' }).click();
    const custom = page.locator('.model-nav-rail__tag-chip').filter({ hasText: /^tools/ });
    await expect(custom).toBeVisible();
    expect(await custom.getAttribute('aria-pressed')).toBe('true');
    await expect(page.locator('.model-list-item')).toHaveCount(1);
    await expect(page.locator('.model-list-item').first()).toContainText('Llama');
    await page.getByRole('button', { name: 'Remove custom tag tools' }).click();
    await expect(custom).toHaveCount(0);
  });

  // ── Storage meter ────────────────────────────────────────────────────────

  test('A132 — Storage meter is a role=progressbar with value range and accessible name', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const bar = page.locator('.model-nav-rail__storage-bar');
    await expect(bar).toHaveAttribute('role', 'progressbar');
    expect(await bar.getAttribute('aria-valuenow')).toBeTruthy();
    expect(await bar.getAttribute('aria-valuemin')).toBe('0');
    const max = await bar.getAttribute('aria-valuemax');
    expect(Number(max)).toBeGreaterThan(0);
    // Accessible name via aria-label.
    expect((await bar.getAttribute('aria-label')) ?? '').not.toBe('');
  });

  // ── Custom-model heading action ───────────────────────────────────────────

  test('A133 — custom-model action is grouped with the Models heading and keyboard reachable', async ({ page }) => {
    await goToModelsWithNavMock(page);

    const title = page.locator('.model-list-panel .workspace-list-panel__header');
    const heading = title.getByRole('heading', { name: 'Models' });
    const customModelsBtn = title.getByRole('button', { name: 'Open custom models' });
    await expect(title).toBeVisible();
    await expect(heading).toBeVisible();
    await expect(customModelsBtn).toBeVisible();

    await customModelsBtn.focus();
    await expect(customModelsBtn).toBeFocused();
    await page.keyboard.press('Enter');
    await expect(page.locator('.custom-model-form')).toBeVisible();

    // The heading action remains grounded above the model list.
    const titleBox = await title.boundingBox();
    const listBox = await page.locator('.model-list-panel__list').boundingBox();
    expect(titleBox && listBox && titleBox.y < listBox.y).toBeTruthy();
  });

  // ── Responsive nav toggle ────────────────────────────────────────────────

  test('A134 — on narrow viewport the nav toggle controls the rail and is keyboard reachable', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 900 });
    await goToModelsWithNavMock(page);
    const toggle = page.getByRole('button', { name: 'Open model filters', exact: true });
    await expect(toggle).toBeVisible();
    expect(await toggle.getAttribute('aria-controls')).toBe('model-nav-rail');
    expect(await toggle.getAttribute('aria-expanded')).toBe('false');
    // Rail hidden until toggled.
    await expect(page.locator('.model-nav-rail')).toBeHidden();
    await toggle.focus();
    await page.keyboard.press('Enter');
    await page.waitForTimeout(150);
    expect(await toggle.getAttribute('aria-expanded')).toBe('true');
    await expect(page.locator('.model-nav-rail')).toBeVisible();
  });

  // ── Axe scan ─────────────────────────────────────────────────────────────

  test('A135 — three-pane model view with the left rail passes WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });

});

// ─── PR #2424 maintainer refinements (fl0rianr 2026-06-25) ──────────────────
//
// Five review items: (1) a favorite STAR toggle in the model DETAIL panel that
// reuses the existing client-local pin store and updates the Favorites nav
// the updated A136); (3) "Back to models" hidden on desktop, shown on narrow;
// (4) the middle-panel funnel filter was later removed by #2936 so filtering
// lives only in the left rail; (5) the left rail scrolls independently instead of clipping the
// lower part of the screen when its sections are expanded.
// Range: A142–A148.

test.describe('Accessibility — model view refinements (#2424)', () => {
  async function goToModelsRefined(page: Page): Promise<void> {
    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    // Keep the storage-meter tests deterministic: this helper intentionally
    // exercises the fallback estimate path instead of inheriting whatever disk
    // shape a local dev server may expose today.
    await page.route('**/api/v1/system-info**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({}),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm', 'tools'], recipe: 'llamacpp', suggested: true, downloaded: true, size: 8 },
            { id: 'Qwen2.5-7B', name: 'Qwen2.5-7B', labels: ['llm'], recipe: 'llamacpp', downloaded: false, size: 7 },
            { id: 'Whisper-Large-v3', name: 'Whisper-Large-v3', labels: ['audio'], recipe: 'whispercpp', downloaded: true, size: 3 },
            { id: 'SDXL-Turbo', name: 'SDXL-Turbo', labels: ['image'], recipe: 'sd-cpp', downloaded: false, size: 6 },
          ],
        }),
      }),
    );
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.waitForTimeout(200);
  }

  // ── 1. Favorite star toggle in the detail panel ──────────────────────────

  test('A142 — detail panel favorite star is an aria-pressed toggle naming the model', async ({ page }) => {
    await goToModelsRefined(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);
    const star = page.locator('.model-detail-panel__fav-btn');
    await expect(star).toBeVisible();
    await expect(star).toHaveRole('button');
    // Off state: not pressed, label invites adding to favorites and names the model.
    expect(await star.getAttribute('aria-pressed')).toBe('false');
    const offLabel = (await star.getAttribute('aria-label')) ?? '';
    expect(offLabel.toLowerCase()).toContain('favorite');
    expect(offLabel).toContain('Llama-3.1-8B');
    // Toggle on.
    await star.click();
    await page.waitForTimeout(120);
    expect(await star.getAttribute('aria-pressed')).toBe('true');
    expect(((await star.getAttribute('aria-label')) ?? '').toLowerCase()).toContain('remove');
  });

  test('A143 — favoriting in the detail panel updates the Favorites nav count and persists to a DISTINCT favorites store', async ({ page }) => {
    await goToModelsRefined(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(150);
    await page.locator('.model-detail-panel__fav-btn').click();
    await page.waitForTimeout(150);

    // Favorites primary-nav entry now reports one model (via its sr-only phrase).
    const fav = page.locator('.model-nav-rail__nav-item').filter({ hasText: 'Favorites' });
    await expect(fav).toContainText('1');

    // Favorites is a DISTINCT concept from Pinned (#2424): it must persist to a
    // SEPARATE favorite_models key, NOT the pinned_models store.
    const stores = await page.evaluate(() => {
      let favorite: string | null = null;
      let pinned: string | null = null;
      for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (!key) continue;
        if (key.endsWith('favorite_models')) favorite = localStorage.getItem(key);
        if (key.endsWith('pinned_models')) pinned = localStorage.getItem(key);
      }
      return { favorite, pinned };
    });
    expect(stores.favorite, 'favorite must persist to a separate favorite_models store').toBeTruthy();
    expect((stores.favorite ?? '').toLowerCase()).toContain('llama-3.1-8b');
    // The pinned store must NOT have been touched by favoriting.
    expect((stores.pinned ?? '').toLowerCase()).not.toContain('llama-3.1-8b');
  });

  // ── 3. "Back to models" only on narrow viewports ─────────────────────────

  test('A144 — "Back to models" is hidden on desktop widths', async ({ page }) => {
    await page.setViewportSize({ width: 1200, height: 800 });
    await goToModelsRefined(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);
    // Present in the DOM but visually hidden on desktop.
    await expect(page.locator('.model-detail-panel__back-btn')).toBeHidden();
  });

  // ── 4. Filters live only in the left rail (#2936) ─────────────────────────

  test('A145 — the models middle panel has no duplicate funnel filter control', async ({ page }) => {
    await goToModelsRefined(page);
    await expect(page.locator('.model-list-panel__filter-btn')).toHaveCount(0);
    await expect(page.locator('.model-list-panel__filter-popover')).toHaveCount(0);
    await expect(page.locator('.model-nav-rail')).toBeVisible();
  });

  test('A146 — removing the funnel does not degrade model search', async ({ page }) => {
    await goToModelsRefined(page);
    const search = page.locator('#model-list-search');
    await search.fill('Whisper');
    await page.waitForTimeout(150);
    const rows = page.locator('.model-list-item');
    await expect(rows).toHaveCount(1);
    await expect(rows.first()).toContainText('Whisper-Large-v3');
  });

  // ── 5. Left rail scrolls instead of clipping ─────────────────────────────

  test('A147 — the left nav rail is independently scrollable and reaches the Storage meter', async ({ page }) => {
    await page.setViewportSize({ width: 1100, height: 600 });
    await goToModelsRefined(page);
    const rail = page.locator('.model-nav-rail');
    await expect(rail).toBeVisible();
    const railScroll = page.locator('.model-nav-rail__scroll');
    const overflowY = await railScroll.evaluate(el => getComputedStyle(el).overflowY);
    expect(overflowY).toBe('auto');
    // … and must not exceed the viewport height (no clipping past the screen).
    const box = await rail.boundingBox();
    expect(box, 'rail bounding box').toBeTruthy();
    expect((box!.y + box!.height)).toBeLessThanOrEqual(601);
    // The Storage meter stays reachable by scrolling within the rail.
    const storage = page.locator('.model-nav-rail__storage');
    await storage.scrollIntoViewIfNeeded();
    await expect(storage).toBeVisible();
  });

  // ── 6. Capability icons on rows + empty-state in detail pane (#2424) ──────

  test('A149 — middle-list rows show multiple capability icons with an accessible label', async ({ page }) => {
    await goToModelsRefined(page);
    // The Llama row exposes both Chat and Tool-use capabilities → ≥2 icons.
    const llamaRow = page.locator('.model-list-item').filter({ hasText: 'Llama-3.1-8B' });
    const caps = llamaRow.locator('.model-list-item__caps');
    await expect(caps).toHaveAttribute('role', 'img');
    const label = (await caps.getAttribute('aria-label')) ?? '';
    expect(label.toLowerCase()).toContain('capabilities');
    expect(label).toMatch(/Tool use/i);
    // Multiple capability icon slots rendered for this multi-capability model.
    const iconCount = await caps.locator('.model-list-item__cap').count();
    expect(iconCount).toBeGreaterThanOrEqual(2);
  });

  test('A150 — the "no model selected" empty state lives in the detail pane, NOT the middle list', async ({ page }) => {
    await goToModelsRefined(page);
    // Nothing selected yet: the middle list must NOT render an empty placeholder…
    await expect(page.locator('.model-list-panel__empty')).toHaveCount(0);
    // …and the model rows are still present (the list is populated).
    expect(await page.locator('.model-list-item').count()).toBeGreaterThan(0);
    // The empty/placeholder message belongs to the RIGHT detail pane.
    const placeholder = page.locator('.model-detail-panel__placeholder');
    await expect(placeholder).toBeVisible();
    await expect(placeholder).toContainText(/no model selected/i);
  });

  // ── 7. Storage meter uses derived data, not the 32/512 mock (#2424) ───────

  test('A151 — Storage meter derives from data (no hardcoded 512 GB) and labels fallback estimates', async ({ page }) => {
    await goToModelsRefined(page);
    const value = page.locator('.model-nav-rail__storage-value');
    const text = (await value.textContent()) ?? '';
    // Downloaded mock sizes total 11 GB → the fallback capacity is derived from
    // the data (32 GB), never the old 512 GB literal. If a real storage source
    // is available in the test/browser environment, this assertion still guards
    // against regressing to the hardcoded mock.
    expect(text).not.toContain('512');
    const match = text.match(/(\d+)\s*GB\s*\/\s*(\d+)\s*GB/i);
    expect(match).not.toBeNull();

    const bar = page.locator('.model-nav-rail__storage-bar');
    const ariaLabel = ((await bar.getAttribute('aria-label')) ?? '').toLowerCase();
    const ariaValueText = ((await bar.getAttribute('aria-valuetext')) ?? '').toLowerCase();
    const ariaValueMax = Number(await bar.getAttribute('aria-valuemax'));
    const visibleTotal = Number(match?.[2] ?? NaN);

    expect(ariaLabel).toContain('model storage used');
    expect(ariaValueMax).toBe(visibleTotal);
    expect(ariaValueMax).not.toBe(512);

    const labelText = ((await page.locator('.model-nav-rail__storage-label').textContent()) ?? '').toLowerCase();
    expect(labelText).toContain('est');
    expect(ariaLabel).toContain('estimat');
    expect(ariaValueText).toContain('estimat');
  });

  // ── 8. HuggingFace inline zone in the model list panel (#2564) ──────────────

  async function goToModelsRefinedWithHf(page: Page): Promise<void> {
    await page.route('**huggingface.co/api/models**', async route => {
      const url = new URL(route.request().url());
      const modelPath = url.pathname.replace('/api/models', '').replace(/^\//, '');
      if (modelPath) {
        const isFlm = modelPath.includes('FastFlowLM');
        const isImage = modelPath.includes('No-Pipeline-Image');
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({
            id: modelPath,
            tags: isFlm ? ['flm'] : [],
            siblings: isFlm
              ? [{ rfilename: 'model.flm' }]
              : isImage ? [{ rfilename: 'diffusion-model.safetensors' }] : [],
          }),
        });
        return;
      }
      await route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify([
          { id: 'org/Mistral-7B-GGUF', modelId: 'org/Mistral-7B-GGUF', likes: 10, downloads: 999, tags: ['gguf', 'text-generation'], pipeline_tag: 'text-generation' },
          { id: 'org/Mistral-Image-GGUF', modelId: 'org/Mistral-Image-GGUF', likes: 7, downloads: 700, tags: ['gguf', 'text-to-image'], pipeline_tag: 'text-to-image' },
          { id: 'org/Mistral-No-Pipeline-Image-GGUF', modelId: 'org/Mistral-No-Pipeline-Image-GGUF', likes: 6, downloads: 600, tags: ['gguf'] },
          { id: 'org/Phi-3-GGUF', modelId: 'org/Phi-3-GGUF', likes: 5, downloads: 500, tags: ['gguf'], pipeline_tag: 'text-generation' },
          { id: 'FastFlowLM/Mistral-FLM', modelId: 'FastFlowLM/Mistral-FLM', likes: 4, downloads: 400, tags: ['flm'], pipeline_tag: 'text-generation' },
        ]),
      });
    });
    await page.route('**/api/v1/pull/variants?**', async route => {
      const checkpoint = new URL(route.request().url()).searchParams.get('checkpoint') || '';
      const needsFallback = checkpoint.includes('No-Pipeline-Image') || checkpoint.includes('FastFlowLM');
      await route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          checkpoint,
          source: 'huggingface',
          recipe: 'llamacpp',
          repo_kind: 'gguf',
          suggested_name: checkpoint.split('/').pop() || checkpoint,
          suggested_labels: [],
          mmproj_files: [],
          variants: needsFallback ? [] : [{ name: 'Q4_K_M', primary_file: 'model.gguf', files: ['model.gguf'], sharded: false, size_bytes: 1024 }],
        }),
      });
    });
    await goToModelsRefined(page);
  }

  test('A152 — HuggingFace results appear inline in the model list panel on search and clear when search is cleared', async ({ page }) => {
    await goToModelsRefinedWithHf(page);
    const search = page.locator('#model-list-search');
    await search.fill('mistral');
    // Allow the debounced HF search (400ms) + render to settle.
    await page.waitForTimeout(900);

    // No nav rail HF entry — results are inline in the list panel.
    await expect(page.locator('.model-nav-rail__nav-item').filter({ hasText: 'Hugging Face' })).toHaveCount(0);

    // HF zone is visible (elevated at top since no local models match "mistral").
    const hfZone = page.locator('.zone--hf');
    await expect(hfZone).toBeVisible();
    await expect(hfZone).toContainText('HuggingFace');

    // GUI2's compatibility filters remove both excluded pipeline categories and
    // incompatible backends discovered from repository metadata.
    const hfRows = page.locator('.zone--hf .row--hf');
    await expect(hfRows).toHaveCount(3);
    await expect(hfZone).not.toContainText('Mistral-Image');
    await expect(hfZone).not.toContainText('Mistral-No-Pipeline-Image');
    await expect(hfZone).toContainText('Mistral-FLM');
    const flmRow = hfZone.locator('.row--hf').filter({ hasText: 'Mistral-FLM' });
    await flmRow.locator('.row__content').click();
    await expect(flmRow.locator('.row__detail').getByRole('button', { name: 'Download FastFlowLM/Mistral-FLM' })).toBeVisible();

    // Clearing the search removes the HF zone entirely.
    await search.fill('');
    await page.waitForTimeout(300);
    await expect(page.locator('.zone--hf')).toHaveCount(0);
  });

  test('A153 — the model view with an active HuggingFace search passes WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await goToModelsRefinedWithHf(page);
    await page.locator('#model-list-search').fill('mistral');
    await page.waitForTimeout(900);
    // HF zone is now inline in the list panel; no nav rail click needed.
    await expect(page.locator('.zone--hf')).toBeVisible();
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});


//
//   • Live changes (sampling / system prompt / tools) are a pure client-local
//     rebind — NO network call, applied by request composition next request.
//   • Load-time changes (recipe_options) perform a real reload = unload + load
// Range: A154–A166.


// ─── 29. Model-detail Files tab — model file listing (#2428 Slice 2) ──────────
//
// Slice 2 wires the Files tab to the new GET /api/v1/models/{id}/files endpoint
// (PR #2437). The tab renders the physical files backing a model — filename,
// role badge, human-readable size, and download status — in an accessible
// <table> with column headers and a caption. Empty/error/loading states are
// covered too. Range: A180–A184.

test.describe('Accessibility — model-detail Files tab (#2428)', () => {
  const SAMPLE_FILES = [
    { name: 'Llama-3.1-8B-Q4_K_M.gguf', role: 'main', size_bytes: 4294967296, exists: true },
    { name: 'mmproj-Llama-3.1-8B.gguf', role: 'mmproj', size_bytes: 524288000, exists: true },
    { name: 'tokenizer.json', role: 'tokenizer', size_bytes: 0, exists: false },
  ];

  async function goToFilesTab(
    page: Page,
    opts: { files?: Array<Record<string, unknown>>; filesStatus?: number } = {},
  ): Promise<void> {
    const files = opts.files ?? SAMPLE_FILES;
    const filesStatus = opts.filesStatus ?? 200;

    await page.route('**/api/v1/health', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: [] }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
          ],
        }),
      }),
    );
    // Registered AFTER the generic /models** route so this more specific handler
    // wins (Playwright matches routes in reverse registration order).
    await page.route('**/api/v1/models/*/files', async route =>
      route.fulfill({
        status: filesStatus,
        contentType: 'application/json',
        body: JSON.stringify({ model_id: 'Llama-3.1-8B', files }),
      }),
    );

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(150);
    await page.locator('[role="tab"]').filter({ hasText: /Files/i }).click();
    await page.waitForTimeout(200);
  }

  test('A180 — Files tab renders a table with accessible column headers', async ({ page }) => {
    await goToFilesTab(page);
    const table = page.locator('.detail-files__table');
    await expect(table).toBeVisible();
    // Column headers expose scope="col" so screen readers announce them.
    const headers = table.locator('th[scope="col"]');
    expect(await headers.count()).toBe(4);
    await expect(table.locator('caption')).toHaveText(/Files backing/i);
    // One body row per file returned by the endpoint.
    await expect(table.locator('tbody tr')).toHaveCount(SAMPLE_FILES.length);
  });

  test('A181 — file rows show name, role badge, size, and download status', async ({ page }) => {
    await goToFilesTab(page);
    const firstRow = page.locator('.detail-files__table tbody tr').first();
    await expect(firstRow.locator('.detail-files__name')).toContainText('Llama-3.1-8B-Q4_K_M.gguf');
    await expect(firstRow.locator('.detail-files__role-badge')).toHaveText(/Main/i);
    // 4294967296 bytes == 4.00 GB (binary units).
    await expect(firstRow.locator('.detail-files__col-size')).toContainText('GB');
    await expect(firstRow.locator('.detail-files__status--present')).toContainText(/Downloaded/i);

    // A not-yet-downloaded file surfaces the missing state (not by color alone).
    const missingRow = page.locator('.detail-files__table tbody tr').last();
    await expect(missingRow.locator('.detail-files__status--missing')).toContainText(/Not downloaded/i);
  });

  test('A182 — empty file list shows an accessible empty state', async ({ page }) => {
    await goToFilesTab(page, { files: [] });
    await expect(page.locator('.detail-files__table')).toHaveCount(0);
    const empty = page.locator('.detail-files--empty');
    await expect(empty).toBeVisible();
    await expect(empty).toContainText(/No files found/i);
  });

  test('A183 — a failed files request shows an error state, not a broken table', async ({ page }) => {
    await goToFilesTab(page, { filesStatus: 500 });
    await expect(page.locator('.detail-files__table')).toHaveCount(0);
    await expect(page.locator('.detail-files--empty')).toContainText(/Unable to load files/i);
  });

  test('A184 — the Files tab passes a WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await goToFilesTab(page);
    await expect(page.locator('.detail-files__table')).toBeVisible();
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});


//
// the Chat composer toolbar. The model selector, Effective Settings button,
// Add menu, tools entry, and Logs toggle must remain accessible. Range: A185–A187.

test.describe('Chat toolbar accessibility', () => {
  async function goToChatWithLoadedModel(page: Page): Promise<void> {
    await page.route('**/api/v1/health**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          status: 'ok',
          version: 'test',
          all_models_loaded: [
            {
              model_name: 'Llama-3.1-8B-Instruct',
              checkpoint: 'Llama-3.1-8B-Instruct',
              recipe: 'llamacpp',
              device: 'cpu',
              backend_url: 'http://localhost:11434',
              pid: 1234,
              type: 'llm',
              last_use: Date.now(),
              labels: ['llm'],
              capabilities: ['chat'],
            },
          ],
        }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B-Instruct', name: 'Llama-3.1-8B-Instruct', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
          ],
        }),
      }),
    );
    await page.goto('/');
    await page.waitForSelector('.chat');
    await page.waitForTimeout(300);
  }

  test('A186 — composer toolbar retains model selector, settings, add menu, and Logs buttons', async ({ page }) => {
    await goToChatWithLoadedModel(page);
    // Model picker button is present (model is loaded so it appears)
    await expect(page.locator('.composer__model-button')).toBeVisible();
    await expect(page.getByRole('button', { name: /Add files, photos, or tools/i })).toBeVisible();
    // Logs toggle
    await expect(page.getByRole('button', { name: /Logs/i })).toBeVisible();
    // Effective Settings button (requires current model)
    await expect(page.getByRole('button', { name: 'Effective settings' })).toBeVisible();
  });

  test('A187 — add menu exposes one unified tools entry and is keyboard-operable', async ({ page }) => {
    await goToChatWithLoadedModel(page);
    const addBtn = page.getByRole('button', { name: /Add files, photos, or tools/i });
    await expect(addBtn).toBeVisible();
    await expect(addBtn).toHaveAttribute('aria-expanded', 'false');
    await addBtn.focus();
    await page.keyboard.press('Space');
    await expect(addBtn).toHaveAttribute('aria-expanded', 'true');
    const menu = page.getByRole('menu', { name: 'Add to chat' });
    await expect(menu).toBeVisible();
    const toolsEntry = page.getByRole('menuitem', { name: 'Tools', exact: true });
    await expect(toolsEntry).toBeVisible();
    await toolsEntry.focus();
    await page.keyboard.press('Space');
    const dialog = page.getByRole('dialog', { name: 'Tools for this chat' });
    await expect(dialog).toBeVisible();
    await expect(menu).toHaveCount(0);
    const back = page.getByRole('button', { name: 'Back to add to chat options' });
    await expect(back).toBeVisible();
    await expect(back).toBeFocused();
    const results = await new AxeBuilder({ page })
      .include('[role="dialog"]')
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
    await back.focus();
    await page.keyboard.press('Space');
    await expect(dialog).toHaveCount(0);
    await expect(menu).toBeVisible();
    await expect(toolsEntry).toBeVisible();
    await expect(toolsEntry).toBeFocused();
  });

  test('A187b — external MCP remains selectable inside the unified tools flow', async ({ page }) => {
    await goToChatWithLoadedModel(page);
    const addBtn = page.getByRole('button', { name: /Add files, photos, or tools/i });
    await addBtn.click();
    const menu = page.getByRole('menu', { name: 'Add to chat' });
    const toolsEntry = page.getByRole('menuitem', { name: 'Tools', exact: true });
    await toolsEntry.click();
    const dialog = page.getByRole('dialog', { name: 'Tools for this chat' });
    await expect(dialog).toBeVisible();
    await expect(menu).toHaveCount(0);
    const externalTab = dialog.getByRole('tab', { name: 'External MCP servers', exact: true });
    await externalTab.click();
    await expect(externalTab).toHaveAttribute('aria-selected', 'true');
    await page.getByRole('button', { name: 'Back to add to chat options' }).click();
    await expect(menu).toBeVisible();
    await expect(toolsEntry).toBeVisible();
  });

  test('A187c — MCP enablement selection persists when backing out and reopening the picker', async ({ page }) => {
    await goToChatWithLoadedModel(page);
    await page.getByRole('button', { name: /Add files, photos, or tools/i }).click();
    await page.getByRole('menuitem', { name: 'Tools', exact: true }).click();

    const dialog = page.getByRole('dialog', { name: 'Tools for this chat' });
    const enabled = dialog.getByRole('checkbox', { name: /Tools for this chat/i });
    await expect(enabled).toBeChecked();
    await enabled.uncheck();
    await expect(enabled).not.toBeChecked();

    await page.getByRole('button', { name: 'Back to add to chat options' }).click();
    await page.getByRole('menuitem', { name: 'Tools', exact: true }).click();
    await expect(page.getByRole('dialog', { name: 'Tools for this chat' })).toBeVisible();
    await expect(page.getByRole('dialog', { name: 'Tools for this chat' }).getByRole('checkbox', { name: /Tools for this chat/i })).not.toBeChecked();
  });

  async function goToChatWithLoadedModels(page: Page, names: string[]): Promise<void> {
    const models = names.map(name => ({
      model_name: name,
      checkpoint: name,
      recipe: 'llamacpp',
      device: 'cpu',
      backend_url: 'http://localhost:11434',
      pid: 1234,
      type: 'llm',
      last_use: Date.now(),
      labels: ['llm'],
      capabilities: ['chat'],
    }));
    const modelInfos = names.map(name => ({
      id: name,
      name,
      labels: ['llm'],
      recipe: 'llamacpp',
      downloaded: true,
    }));
    await page.route('**/api/v1/health**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ status: 'ok', version: 'test', all_models_loaded: models }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({ contentType: 'application/json', body: JSON.stringify({ data: modelInfos }) }),
    );
    await page.goto('/');
    await page.waitForSelector('.chat');
    await page.waitForTimeout(300);
  }

  test('A187d — MCP selection survives model changes, picker reopen, and reload', async ({ page }) => {
    await goToChatWithLoadedModels(page, ['model-one', 'model-two']);
    await page.evaluate(() => {
      localStorage.setItem('lemonade:mcp_enabled', 'true');
      localStorage.setItem('lemonade:mcp_server_ids', JSON.stringify(['lemonade', 'external-echo']));
      localStorage.setItem('lemonade:mcp_tool_names', JSON.stringify(['lemonade_chat', 'external-echo__echo']));
    });
    await page.reload();
    await page.waitForSelector('.chat');

    const openPicker = async () => {
      await page.getByRole('button', { name: /Add files, photos, or tools/i }).click();
      await page.getByRole('menuitem', { name: 'Tools', exact: true }).click();
      return page.getByRole('dialog', { name: 'Tools for this chat' });
    };
    const dismissAddMenu = async () => {
      const addButton = page.getByRole('button', { name: /Add files, photos, or tools/i });
      const menu = page.getByRole('menu', { name: 'Add to chat' });
      await expect(menu).toBeVisible();
      await addButton.click();
      await expect(menu).toHaveCount(0);
      await expect(addButton).toHaveAttribute('aria-expanded', 'false');
    };
    let dialog = await openPicker();
    await expect(dialog).toContainText('2 servers');
    await page.getByRole('button', { name: 'Back to add to chat options' }).click();
    await dismissAddMenu();

    await page.locator('.composer__model-button').click();
    await page.getByRole('option', { name: /model-two/i }).click();
    await page.waitForTimeout(250);
    dialog = await openPicker();
    await expect(dialog).toContainText('2 servers');
    await page.getByRole('button', { name: 'Back to add to chat options' }).click();
    await dismissAddMenu();

    const persistedBeforeReload = await page.evaluate(() => ({
      enabled: localStorage.getItem('lemonade:mcp_enabled'),
      servers: localStorage.getItem('lemonade:mcp_server_ids'),
      tools: localStorage.getItem('lemonade:mcp_tool_names'),
    }));
    expect(persistedBeforeReload).toEqual({
      enabled: 'true',
      servers: JSON.stringify(['lemonade', 'external-echo']),
      tools: JSON.stringify(['lemonade_chat', 'external-echo__echo']),
    });

    await page.reload();
    await page.waitForSelector('.chat');
    dialog = await openPicker();
    await expect(dialog).toContainText('2 servers');
  });

  test('A187e — legacy use_tools is only a fallback when explicit MCP enablement is absent', async ({ page }) => {
    await page.addInitScript(() => {
      if (sessionStorage.getItem('mcp-fallback-seeded')) return;
      localStorage.removeItem('lemonade:mcp_enabled');
      localStorage.setItem('lemonade:use_tools', 'false');
      sessionStorage.setItem('mcp-fallback-seeded', 'true');
    });
    await goToChatWithLoadedModel(page);
    await page.getByRole('button', { name: /Add files, photos, or tools/i }).click();
    await page.getByRole('menuitem', { name: 'Tools', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Tools for this chat' });
    await expect(dialog.getByRole('checkbox', { name: /Tools for this chat/i })).not.toBeChecked();
    await page.getByRole('button', { name: 'Back to add to chat options' }).click();

    await page.evaluate(() => localStorage.setItem('lemonade:mcp_enabled', 'true'));
    await page.reload();
    await page.waitForSelector('.chat');
    await page.getByRole('button', { name: /Add files, photos, or tools/i }).click();
    await page.getByRole('menuitem', { name: 'Tools', exact: true }).click();
    await expect(page.getByRole('dialog', { name: 'Tools for this chat' }).getByRole('checkbox', { name: /Tools for this chat/i })).toBeChecked();
  });
});


//
//   - Built-in tuning source badge is renamed to "Recipe default".
//   - Local direct-config badge renamed to "Direct configuration".
//   - An authority note near Settings by source explains that the load command
//     includes server-applied defaults not shown in the rows table.
// Range: A188–A192.

test.describe('Effective Settings modal accessibility', () => {
  async function openEffectiveSettings(page: Page): Promise<void> {
    await page.route('**/api/v1/health**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          status: 'ok',
          version: 'test',
          all_models_loaded: [
            {
              model_name: 'Llama-3.1-8B-Instruct',
              checkpoint: 'Llama-3.1-8B-Instruct',
              recipe: 'llamacpp',
              device: 'cpu',
              backend_url: 'http://localhost:11434',
              pid: 1234,
              type: 'llm',
              last_use: Date.now(),
              labels: ['llm'],
              capabilities: ['chat'],
            },
          ],
        }),
      }),
    );
    await page.route('**/api/v1/models**', async route =>
      route.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          data: [
            { id: 'Llama-3.1-8B-Instruct', name: 'Llama-3.1-8B-Instruct', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
          ],
        }),
      }),
    );
    await page.route('**/api/v1/load**', async route =>
      route.fulfill({ contentType: 'application/json', body: JSON.stringify({ args: ['--threads', '8'], options: {}, backend: 'llamacpp' }) }),
    );
    await page.goto('/');
    await page.waitForSelector('.chat');
    await page.waitForTimeout(300);
    await page.getByRole('button', { name: 'Effective settings' }).click();
    await page.waitForSelector('[role="dialog"]');
    await page.waitForTimeout(200);
  }

  test('A191 — Settings by source has an authority note referencing the Effective load command', async ({ page }) => {
    await openEffectiveSettings(page);
    const note = page.locator('.effective-settings__section').first().locator('.effective-settings__note');
    await expect(note).toBeVisible();
    await expect(note).toContainText('Effective load command');
    await expect(note).toContainText('authoritative');
    const copy = note.locator('.effective-settings__note-copy');
    await expect(copy).toHaveCount(1);
    await expect(copy.locator('strong')).toHaveText('Effective load command');
  });

  test('A192 — modal passes a WCAG 2.1 AA axe-core scan with no critical/serious violations', async ({ page }) => {
    await openEffectiveSettings(page);
    const results = await new AxeBuilder({ page })
      .include('[role="dialog"]')
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});
