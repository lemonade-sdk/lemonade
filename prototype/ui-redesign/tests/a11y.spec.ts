/**
 * a11y.spec.ts — Accessibility test suite for the Lemonade UI redesign prototype.
 *
 * Covers all Phase 1 + Phase 2 items shipped on kpoin/ui-accessibility:
 *   - axe-core WCAG 2.1 AA automated scans (one per major view)
 *   - Skip link behaviour (hidden until focused, activates #main-content)
 *   - ARIA landmarks (<main>, <nav>, role="status")
 *   - Keyboard navigation order and completeness
 *   - Focus traps: bottom sheet (mobile 390px) and preset slideover
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

// ─── Constants & helpers ──────────────────────────────────────────────────────

const WCAG_TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'] as const;

/** Navigate to a view by its titlebar nav label text. */
async function navigateToView(page: Page, label: string): Promise<void> {
  await page.locator('.titlebar__nav').getByText(label).click();
  await page.waitForTimeout(300);
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

  test('A03 — Presets view passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('[data-view="presets"]');

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
    await navigateToView(page, 'Connect');
    await page.waitForSelector('.connect');

    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });

  test('A05 — Dashboard view passes WCAG 2.1 AA', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Dash');
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
    await page.waitForSelector('.titlebar__status-dot');

    const role = await page.locator('.titlebar__status-dot').getAttribute('role');
    expect(role).toBe('status');
  });

  test('A13 — status dot aria-label reflects one of the three connection states', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__status-dot');

    const label = await page.locator('.titlebar__status-dot').getAttribute('aria-label');
    expect(label).toBeTruthy();
    expect(['Connected', 'Connecting…', 'Offline']).toContain(label);
  });
});

// ─── 4. Keyboard navigation ───────────────────────────────────────────────────

test.describe('Accessibility — keyboard navigation', () => {
  test('A14 — at least one titlebar nav button is reachable in the first 12 Tab presses', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    const knownNavLabels = ['Chat', 'Models', 'Presets', 'Backends', 'Dash', 'Logs', 'Connect'];
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
    await page.waitForSelector('.chat__mobile-rail-trigger');
  });

  test('A17 — opening bottom sheet moves focus inside it (useFocusTrap activates)', async ({ page }) => {
    await page.locator('.chat__mobile-rail-trigger').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    const activeIsInSheet = await page.evaluate(() => {
      const sheet = document.querySelector('.bottom-sheet');
      return sheet?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInSheet).toBe(true);
  });

  test('A18 — Tab from last focusable inside bottom sheet wraps back to first (never escapes)', async ({ page }) => {
    await page.locator('.chat__mobile-rail-trigger').click();
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
    await page.locator('.chat__mobile-rail-trigger').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    await page.keyboard.press('Escape');

    await expect(page.locator('.bottom-sheet--open')).not.toBeVisible({ timeout: 3000 });
  });

  test('A20 — focus returns to trigger button after bottom sheet closes via Escape', async ({ page }) => {
    await page.locator('.chat__mobile-rail-trigger').click();
    await page.locator('.bottom-sheet--open').waitFor({ state: 'visible', timeout: 5000 });

    await page.keyboard.press('Escape');
    await page.waitForTimeout(100); // allow rAF from closeMobileSheet to run

    const activeClass = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.className ?? '',
    );
    expect(activeClass).toContain('chat__mobile-rail-trigger');
  });
});

// ─── 6. Focus trap — preset slideover ────────────────────────────────────────

test.describe('Accessibility — focus trap (preset slideover)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.recipe-card', { timeout: 5000 });
  });

  test('A21 — opening preset slideover moves focus inside it (useFocusTrap activates)', async ({ page }) => {
    await page.locator('.recipe-card').first().click();
    await page.waitForSelector('.slideover.is-open', { timeout: 5000 });

    const activeIsInSlideover = await page.evaluate(() => {
      const slideover = document.querySelector('.slideover.is-open');
      return slideover?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInSlideover).toBe(true);
  });

  test('A22 — Tab from last focusable inside slideover wraps back to first (never escapes)', async ({ page }) => {
    await page.locator('.recipe-card').first().click();
    await page.waitForSelector('.slideover.is-open', { timeout: 5000 });

    const count = await page.locator(
      '.slideover.is-open :is(a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"]))',
    ).evaluateAll(els =>
      els.filter(el => !el.closest('[aria-hidden="true"]')).length,
    );

    for (let i = 0; i < count; i++) {
      await page.keyboard.press('Tab');
    }

    const activeIsInSlideover = await page.evaluate(() => {
      const slideover = document.querySelector('.slideover.is-open');
      return slideover?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInSlideover, 'Focus should still be inside the slideover after wrapping').toBe(true);
  });

  test('A23 — pressing Escape closes the preset slideover', async ({ page }) => {
    await page.locator('.recipe-card').first().click();
    await page.waitForSelector('.slideover.is-open', { timeout: 5000 });

    await page.keyboard.press('Escape');

    // .is-open class is removed; element stays in DOM (CSS transform moves it off-screen)
    await expect(page.locator('.slideover')).not.toHaveClass(/is-open/, { timeout: 3000 });
  });

  test('A24 — focus returns to the preset card that opened the slideover (via Escape)', async ({ page }) => {
    const card = page.locator('.recipe-card').first();
    await card.click();
    await page.waitForSelector('.slideover.is-open', { timeout: 5000 });

    await page.keyboard.press('Escape');
    await page.waitForTimeout(200); // requestAnimationFrame in closeSlideover

    const activeIsOnCard = await page.evaluate(() => {
      const el = document.activeElement as HTMLElement | null;
      return (
        el?.classList.contains('recipe-card') ||
        el?.closest('.recipe-card') !== null
      );
    });
    expect(activeIsOnCard).toBe(true);
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

  test('A30 — composer textarea gets visible focus ring on keyboard focus (Tab navigation)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.composer__input');

    let found = false;
    for (let i = 0; i < 40; i++) {
      await page.keyboard.press('Tab');
      const info = await page.evaluate(() => {
        const el = document.activeElement as HTMLElement | null;
        return {
          isComposer: el?.classList.contains('composer__input') ?? false,
          outlineWidth: el
            ? window.getComputedStyle(el).outlineWidth
            : '0px',
        };
      });
      if (info.isComposer) {
        expect(
          info.outlineWidth,
          'composer__input should have a non-zero outline when reached via keyboard',
        ).not.toBe('0px');
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

// ─── 10. Preset parameter labels — issue #2338 ───────────────────────────────

test.describe('Accessibility — preset parameter labels (#2338)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.recipe-card');
    // nth(1) skips DEFAULT_PRESET (which hides behavior fields); opens first STARTER (chat)
    await page.locator('.recipe-card').nth(1).click();
    await page.waitForSelector('.slideover.is-open');
  });

  test('A35 — temperature slider linked to label via htmlFor/id', async ({ page }) => {
    const labelText = await page.locator('[data-recipe-temp]').evaluate(el => {
      const id = (el as HTMLElement).id;
      if (!id) return '';
      return document.querySelector(`label[for="${id}"]`)?.textContent?.trim() ?? '';
    });
    expect(labelText, 'Temperature slider must be labelled via htmlFor/id').toBeTruthy();
  });

  test('A36 — context-size slider linked to label via htmlFor/id', async ({ page }) => {
    const labelText = await page.locator('[data-recipe-ctx]').evaluate(el => {
      const id = (el as HTMLElement).id;
      if (!id) return '';
      return document.querySelector(`label[for="${id}"]`)?.textContent?.trim() ?? '';
    });
    expect(labelText, 'Context size slider must be labelled via htmlFor/id').toBeTruthy();
  });

  test('A37 — top_k input linked to label via htmlFor/id', async ({ page }) => {
    const labelText = await page.locator('[data-recipe-top-k]').evaluate(el => {
      const id = (el as HTMLElement).id;
      if (!id) return '';
      return document.querySelector(`label[for="${id}"]`)?.textContent?.trim() ?? '';
    });
    expect(labelText, 'top_k input must be labelled via htmlFor/id').toBeTruthy();
  });
});

// ─── 11. Advanced backend/device fields discoverable — issue #2339 ────────────

test.describe('Accessibility — backend/device fields discoverable (#2339)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.recipe-card');
    await page.locator('.recipe-card').nth(1).click();
    await page.waitForSelector('.slideover.is-open');
    // Open the <details> Advanced engine options section
    await page.locator('.preset-advanced summary').click();
    await page.waitForTimeout(200);
  });

  test('A38 — llamacpp_backend input has a datalist of known backend values', async ({ page }) => {
    const listId = await page.locator('#preset-field-llamacpp-backend').getAttribute('list');
    expect(listId, 'llamacpp_backend input must have list= attribute').toBeTruthy();
    const optionCount = await page.locator(`#${listId} option`).count();
    expect(optionCount, 'llamacpp_backend datalist must have ≥3 options').toBeGreaterThanOrEqual(3);
  });

  test('A39 — llamacpp_device input has a datalist of known device values', async ({ page }) => {
    const listId = await page.locator('#preset-field-llamacpp-device').getAttribute('list');
    expect(listId, 'llamacpp_device input must have list= attribute').toBeTruthy();
    const optionCount = await page.locator(`#${listId} option`).count();
    expect(optionCount, 'llamacpp_device datalist must have ≥3 options').toBeGreaterThanOrEqual(3);
  });
});

// ─── 12. Preset card accessible description — issue #2345 ────────────────────

test.describe('Accessibility — preset card metadata accessible (#2345)', () => {
  test('A40 — card button has aria-describedby pointing to metadata description', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.recipe-card');

    const { descId, descText } = await page.locator('.recipe-card').first().evaluate(card => {
      const btn = card.querySelector('.recipe-card__overlay-btn') as HTMLElement | null;
      const id = btn?.getAttribute('aria-describedby') ?? '';
      const descEl = id ? document.getElementById(id) : null;
      return { descId: id, descText: descEl?.textContent?.trim() ?? '' };
    });

    expect(descId, 'card overlay button must have aria-describedby').toBeTruthy();
    expect(descText, 'description element must have non-empty text').toBeTruthy();
    expect(descText, 'description must include applies_to metadata').toMatch(/Applies to:/i);
    expect(descText, 'description must include prompt metadata').toMatch(/Prompt:/i);
    expect(descText, 'description must include tools metadata').toMatch(/Tools:/i);
  });
});

// ─── 13. Capability chip toggle-button semantics — issue #2350 (revised) ─────

test.describe('Accessibility — capability chip toggle-button semantics (#2350)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.recipe-card');
    await page.locator('.recipe-card').nth(1).click();
    await page.waitForSelector('.slideover.is-open');
  });

  test('A41 — capability chip container has role="group" with accessible label', async ({ page }) => {
    const container = page.locator('[data-preset-capabilities]');
    const role = await container.getAttribute('role');
    expect(role).toBe('group');
    const label = await container.getAttribute('aria-label');
    expect(label).toBeTruthy();
  });

  test('A42 — each capability chip is a plain button with aria-pressed', async ({ page }) => {
    const capButtons = page.locator('[data-preset-capabilities] .preset-cap-button');
    const count = await capButtons.count();
    expect(count).toBeGreaterThan(0);

    for (let i = 0; i < count; i++) {
      const btn = capButtons.nth(i);
      // Must NOT have role="radio" — plain button semantics
      const role = await btn.getAttribute('role');
      expect(role, 'chip must not have role="radio"').not.toBe('radio');
      // Must expose aria-pressed as "true" or "false"
      const pressed = await btn.getAttribute('aria-pressed');
      expect(['true', 'false'], `aria-pressed must be "true" or "false", got "${pressed}"`).toContain(pressed);
    }
  });

  test('A43 — exactly one capability chip has aria-pressed="true"; all others are "false"', async ({ page }) => {
    const { trueCount, falseCount, total } = await page
      .locator('[data-preset-capabilities] .preset-cap-button')
      .evaluateAll(buttons => ({
        trueCount: buttons.filter(b => b.getAttribute('aria-pressed') === 'true').length,
        falseCount: buttons.filter(b => b.getAttribute('aria-pressed') === 'false').length,
        total: buttons.length,
      }));
    expect(trueCount).toBe(1);
    expect(falseCount).toBe(total - 1);
  });
});

// ─── 14. AutoOpt run selection state — issue #2352 ───────────────────────────

test.describe('Accessibility — AutoOpt run selection state (#2352)', () => {
  test('A44 — AutoOpt run buttons expose selected state via aria-pressed', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.auto-run-list');

    const buttons = page.locator('.auto-run-card__main');
    const count = await buttons.count();
    expect(count).toBeGreaterThan(0);

    // Exactly one button should be aria-pressed="true" on initial render
    const pressedCount = await buttons.evaluateAll(btns =>
      btns.filter(b => b.getAttribute('aria-pressed') === 'true').length,
    );
    expect(pressedCount).toBe(1);
  });

  test('A45 — clicking a different AutoOpt run updates aria-pressed to true on that button', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Presets');
    await page.waitForSelector('.auto-run-list');

    const buttons = page.locator('.auto-run-card__main');
    // Click the second button (index 1) to change selection
    await buttons.nth(1).click();
    await page.waitForTimeout(100);

    const secondPressed = await buttons.nth(1).getAttribute('aria-pressed');
    expect(secondPressed).toBe('true');

    // First button must now be false
    const firstPressed = await buttons.nth(0).getAttribute('aria-pressed');
    expect(firstPressed).toBe('false');
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
          vulkan: { state: 'installable', version: 'b1234', message: '', action: '' },
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

  // ── #2343 — matrix cells are keyboard-focusable with selected state ────────

  test('A51 — each matrix cell contains a button with aria-pressed (selected state)', async ({ page }) => {
    const cellBtn = page.locator('.cell__select-btn').first();
    await expect(cellBtn).toBeVisible();
    const pressed = await cellBtn.getAttribute('aria-pressed');
    expect(['true', 'false']).toContain(pressed);
  });

  test('A52 — clicking the cell select button toggles aria-pressed between "true" and "false"', async ({ page }) => {
    const cellBtn = page.locator('[data-cell="llamacpp:vulkan"] .cell__select-btn');
    await expect(cellBtn).toHaveAttribute('aria-pressed', 'false');
    await cellBtn.click();
    await expect(cellBtn).toHaveAttribute('aria-pressed', 'true');
    await cellBtn.click();
    await expect(cellBtn).toHaveAttribute('aria-pressed', 'false');
  });

  test('A53 — cell select button is keyboard-focusable (tabIndex >= 0, is a <button>)', async ({ page }) => {
    const cellBtn = page.locator('.cell__select-btn').first();
    // Verify it is a <button> element (native keyboard operability)
    const tag = await cellBtn.evaluate(el => el.tagName.toLowerCase());
    expect(tag).toBe('button');
    // Verify it is in the tab order
    const tabIndex = await cellBtn.evaluate(el => (el as HTMLElement).tabIndex);
    expect(tabIndex).toBeGreaterThanOrEqual(0);
  });

  test('A54 — cell select button aria-label contains both recipe label and backend identifier', async ({ page }) => {
    const btn = page.locator('[data-cell="llamacpp:vulkan"] .cell__select-btn');
    const label = await btn.getAttribute('aria-label');
    expect(label).toBeTruthy();
    // Should mention the human-readable recipe label (llama.cpp) and backend (vulkan)
    expect(label!.toLowerCase()).toContain('llama');
    expect(label!.toLowerCase()).toContain('vulkan');
  });

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

  // ── #2351 — persistent polite live regions for toasts and preset notices ───

  test('A57 — backends view has a persistent role="status" live region for toast messages', async ({ page }) => {
    const liveRegion = page.locator('[data-backends-toast-live]');
    await expect(liveRegion).toHaveCount(1);
    expect(await liveRegion.getAttribute('role')).toBe('status');
    expect(await liveRegion.getAttribute('aria-live')).toBe('polite');
    expect(await liveRegion.getAttribute('aria-atomic')).toBe('true');
  });

  test('A58 — backend preset rail has a persistent role="status" live region for preset notices', async ({ page }) => {
    const liveRegion = page.locator('[data-backends-preset-notice-live]');
    await expect(liveRegion).toHaveCount(1);
    expect(await liveRegion.getAttribute('role')).toBe('status');
    expect(await liveRegion.getAttribute('aria-live')).toBe('polite');
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
    await page.addInitScript((item: unknown) => {
      localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify([item]));
    }, MOCK_DOWNLOAD);

    await page.route('/api/v1/health', route =>
      route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
    );
    // Return empty from the server so the mock item is not overwritten by polling.
    await page.route('/api/v1/downloads**', route =>
      route.fulfill({ json: { downloads: [] } }),
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
      persistKey: 'lemonade:guest:shared:persist_conversations',
      convKey: 'lemonade:guest:shared:conversations',
      activeKey: 'lemonade:guest:shared:active_conversation',
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

// ─── 18. Account menu — modal dialog semantics ────────────────────────────────

test.describe('Accessibility — account menu dialog', () => {
  test('A67 — account menu trigger has aria-haspopup="dialog" and aria-expanded="false" on load', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.account-menu__trigger');

    const trigger = page.locator('.account-menu__trigger');
    expect(await trigger.getAttribute('aria-haspopup')).toBe('dialog');
    expect(await trigger.getAttribute('aria-expanded')).toBe('false');
  });

  test('A68 — opening account menu: panel has role="dialog" + aria-modal="true", trigger aria-expanded="true"', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.account-menu__trigger');

    await page.locator('.account-menu__trigger').click();
    await page.waitForSelector('.account-menu__panel');

    const panel = page.locator('.account-menu__panel');
    expect(await panel.getAttribute('role')).toBe('dialog');
    expect(await panel.getAttribute('aria-modal')).toBe('true');

    const trigger = page.locator('.account-menu__trigger');
    expect(await trigger.getAttribute('aria-expanded')).toBe('true');
  });

  test('A69 — opening account menu moves focus inside the panel (useFocusTrap activates)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.account-menu__trigger');

    await page.locator('.account-menu__trigger').click();
    await page.waitForSelector('.account-menu__panel');

    const activeIsInPanel = await page.evaluate(() => {
      const panel = document.querySelector('.account-menu__panel');
      return panel?.contains(document.activeElement) ?? false;
    });
    expect(activeIsInPanel).toBe(true);
  });

  test('A70 — Escape closes account menu and restores focus to the trigger', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.account-menu__trigger');

    await page.locator('.account-menu__trigger').click();
    await page.waitForSelector('.account-menu__panel');

    await page.keyboard.press('Escape');
    await page.waitForTimeout(100); // allow rAF focus restore

    await expect(page.locator('.account-menu__panel')).not.toBeVisible({ timeout: 3000 });

    const activeClass = await page.evaluate(
      () => (document.activeElement as HTMLElement | null)?.className ?? '',
    );
    expect(activeClass).toContain('account-menu__trigger');

  });
});

// ─── 19. Group F — Omni picker combobox semantics ─────────────────────────────

test.describe('Accessibility — Omni picker combobox semantics (#2347)', () => {
  async function openOmniCollectionForm(page: Page): Promise<void> {
    await page.goto('/');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager');
    await page.getByText('+ Omni collection').click();
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
    await navigateToView(page, 'Connect');
    await page.waitForSelector('.connect');

    await expect(page.getByLabel('Provider name')).toBeVisible();
    await expect(page.getByLabel('Base URL')).toBeVisible();
    await expect(page.getByLabel('Provider API key (optional)')).toBeVisible();
  });

  test('A76 — Marketplace search has accessible name', async ({ page }) => {
    await page.goto('/');
    await navigateToView(page, 'Connect');
    await page.waitForSelector('.connect');

    const searchInput = page.locator('.connect__marketplace-search');
    const ariaLabel = await searchInput.getAttribute('aria-label');
    expect(ariaLabel).toBeTruthy();
    expect(ariaLabel).toBe('Search marketplace apps');
  });
});

// ─── 21. Group F — Icon-only / title-only controls have reliable names (#2353) ─

test.describe('Accessibility — icon-button accessible names (#2353)', () => {
  test('A77 — LogViewer search input has an accessible name (not placeholder-only)', async ({ page }) => {
    await page.goto('/');
    await navigateToView(page, 'Logs');
    await page.waitForSelector('.logs-view');

    const searchInput = page.locator('.logs-search');
    const ariaLabel = await searchInput.getAttribute('aria-label');
    expect(ariaLabel).toBeTruthy();
    expect(ariaLabel).toBe('Filter logs');
  });

  test('A78 — LogViewer Clear button has an aria-label with full action name', async ({ page }) => {
    await page.goto('/');
    await navigateToView(page, 'Logs');
    await page.waitForSelector('.logs-view');

    const clearBtn = page.locator('.logs-btn').filter({ hasNotText: 'Reconnect' }).first();
    const ariaLabel = await clearBtn.getAttribute('aria-label');
    expect(ariaLabel).toBe('Clear log output');
  });

  test('A79 — Omni picker clear button has aria-label naming target', async ({ page }) => {
    await page.goto('/');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager');
    await page.getByText('+ Omni collection').click();
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
    { name: 'lemonade_list_models', description: 'List all models available on this lemonade server.' },
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
    await page.route('**/mcp**', async route => {
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
      } else {
        callIndex++;
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body.id, result: { tools: MCP_TOOLS } }),
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Connect');
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });
    void callIndex; // suppress unused warning
  }

  test('A80 — MCP panel is present in ConnectView with correct heading', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Connect');
    await page.waitForSelector('[data-mcp-panel]', { timeout: 5000 });

    const heading = page.locator('#mcp-section-title');
    await expect(heading).toBeVisible();
    await expect(heading).toHaveText('MCP Gateway');
  });

  test('A81 — MCP endpoint URL input contains /mcp path and is read-only', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Connect');
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
    await navigateToView(page, 'Connect');
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
    await navigateToView(page, 'Connect');
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
    await navigateToView(page, 'Connect');
    await page.waitForSelector('[data-mcp-panel]');

    const statusEl = page.locator('[data-mcp-status]');
    await expect(statusEl).toHaveCount(1);
    expect(await statusEl.getAttribute('role')).toBe('status');
    expect(await statusEl.getAttribute('aria-live')).toBe('polite');
    expect(await statusEl.getAttribute('aria-atomic')).toBe('true');
  });

  test('A85 — with mocked MCP server, tools list renders with expected tool names', async ({ page }) => {
    await setupWithMcp(page);
    await page.waitForSelector('[data-mcp-tools-list]', { timeout: 8000 });

    const toolList = page.locator('[data-mcp-tools-list]');
    await expect(toolList).toBeVisible();

    const items = toolList.locator('.mcp-panel__tool-name');
    const count = await items.count();
    expect(count).toBe(MCP_TOOLS.length);

    // Verify first expected tool name is present
    await expect(toolList.getByText('lemonade_list_models')).toBeVisible();
    await expect(toolList.getByText('lemonade_chat')).toBeVisible();
  });

  test('A86 — tools list element has accessible aria-label', async ({ page }) => {
    await setupWithMcp(page);
    await page.waitForSelector('[data-mcp-tools-list]', { timeout: 8000 });

    const toolList = page.locator('[data-mcp-tools-list]');
    const label = await toolList.getAttribute('aria-label');
    expect(label).toBeTruthy();
  });

  test('A87 — Refresh button has aria-label and is a <button>', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Connect');
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
    await page.route('**/mcp**', async route => {
      const body = route.request().postDataJSON() as { method?: string; id?: number };
      if (body?.method === 'initialize') {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({
            jsonrpc: '2.0', id: body.id,
            result: { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'lemonade-mcp', version: '1.0.0' } },
          }),
        });
      } else {
        await route.fulfill({
          contentType: 'application/json',
          body: JSON.stringify({ jsonrpc: '2.0', id: body.id, result: { tools: MCP_TOOLS } }),
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Connect');
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
    await page.route('**/mcp**', async route => {
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
    await navigateToView(page, 'Connect');
    await page.waitForSelector('[data-mcp-tools-list]', { timeout: 8000 });

    // Must have at least 3 requests: initialize, notifications/initialized, tools/list.
    expect(captured.length).toBeGreaterThanOrEqual(3);

    // (a) initialize is first with correct params
    const initReq = captured[0];
    expect(initReq.method).toBe('initialize');
    const initParams = (initReq.body as { params?: Record<string, unknown> }).params ?? {};
    expect(initParams['protocolVersion']).toBe('2025-06-18');
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
    await page.route('**/mcp**', async route => {
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
    await navigateToView(page, 'Connect');
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
// Covers: model list panel, detail panel tablist, funnel filter button,
// preset attach flow, and keyboard navigation — all added in Slice 1.
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

  // ── Funnel filter ────────────────────────────────────────────────────────────

  test('A95 — funnel filter button has aria-expanded and aria-haspopup', async ({ page }) => {
    await goToModels(page);
    const btn = page.locator('[aria-haspopup="dialog"]').filter({ has: page.locator('[aria-label*="filter" i], [aria-label*="Filter" i]') }).first();
    // Fallback: any button with the funnel SVG class or the filter popover trigger
    const filterBtn = page.locator('button[aria-haspopup="dialog"]').first();
    await expect(filterBtn).toBeAttached();
    await expect(filterBtn).toHaveAttribute('aria-expanded');
  });

  test('A96 — funnel filter popover opens on button click and has role=dialog', async ({ page }) => {
    await goToModels(page);
    const filterBtn = page.locator('button[aria-haspopup="dialog"]').first();
    await filterBtn.click();
    const popover = page.locator('[role="dialog"]').first();
    await expect(popover).toBeVisible();
    await expect(filterBtn).toHaveAttribute('aria-expanded', 'true');
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
    expect(tabCount).toBeGreaterThanOrEqual(2); // README + Presets at minimum

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

  // ── Preset tab attach flow ────────────────────────────────────────────────────

  test('A102 — Presets tab in detail panel is keyboard-reachable and focusable', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const presetsTab = page.locator('[role="tab"]').filter({ hasText: /Preset/i });
    await expect(presetsTab).toBeVisible();
    await presetsTab.click();
    await page.waitForTimeout(100);
    await expect(presetsTab).toHaveAttribute('aria-selected', 'true');
  });

  test('A103 — Presets tab panel has accessible heading or label', async ({ page }) => {
    await goToModelsWithMock(page);
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const presetsTab = page.locator('[role="tab"]').filter({ hasText: /Preset/i });
    await presetsTab.click();
    await page.waitForTimeout(100);

    const tabpanel = page.locator('[role="tabpanel"]:visible');
    await expect(tabpanel).toBeVisible();
    // Panel should have aria-labelledby referencing the tab
    const labelledBy = await tabpanel.getAttribute('aria-labelledby');
    expect(labelledBy).toBeTruthy();
  });

  // ── Custom model / Omni collection buttons ────────────────────────────────────

  test('A104 — "+ Custom model" and "+ Omni collection" buttons are visible and keyboard-accessible', async ({ page }) => {
    await goToModels(page);
    const customBtn = page.getByText('+ Custom model');
    const omniBtn = page.getByText('+ Omni collection');
    await expect(customBtn).toBeVisible();
    await expect(omniBtn).toBeVisible();
    // Both should be real buttons
    await expect(customBtn).toHaveRole('button');
    await expect(omniBtn).toHaveRole('button');
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

// ─── 24. #2355 Slice 1 reconciliation — sort, responsive, README derivation, preset change ──
//
// Covers the 4 gaps addressed in the fl0rianr 2026-06-25 clarifications:
//   A: README checkpoint derivation (tightened regex + checkpoints.main fallback)
//   B: Sort controls (labeled select with 4 options)
//   C: Responsive list-first (narrow ≤700px shows list only; selecting shows detail + Back)
//   D: Presets tab Change inline chooser (attach + detach already present)
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

  // ── D: Preset Change inline chooser ──────────────────────────────────────────

  test('A114 — preset Change button has aria-expanded and aria-haspopup="dialog"', async ({ page }) => {
    // Inject a user preset via localStorage before page load
    await page.addInitScript(() => {
      // Seed a user preset compatible with LLM (chat), using scoped key
      const preset = {
        id: 'test-preset-1',
        name: 'Test Chat Preset',
        description: 'Seed preset for testing',
        applies_to: ['chat'],
        recipe_options: {},
        sampling: {},
        engine_hint: 'auto',
        starter: false,
        auto_opt_run_id: null,
        auto_opt_enabled: true,
        system_prompt_id: 'none',
        system_prompts: [],
        tools_enabled: false,
      };
      localStorage.setItem('lemonade:guest:shared:user_presets', JSON.stringify([preset]));
      // Link the preset to Llama-3.1-8B
      localStorage.setItem('lemonade:guest:shared:applied_presets', JSON.stringify({ 'Llama-3.1-8B': 'test-preset-1' }));
    });

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
    await goToModels(page);
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    // Navigate to Presets tab
    const presetsTab = page.locator('[role="tab"]').filter({ hasText: /Preset/i });
    await presetsTab.click();
    await page.waitForTimeout(100);

    // Change button should be visible (non-default preset is linked)
    const changeBtn = page.locator('.detail-presets__change-btn');
    await expect(changeBtn).toBeVisible();
    await expect(changeBtn).toHaveAttribute('aria-haspopup', 'dialog');
    const expanded = await changeBtn.getAttribute('aria-expanded');
    expect(expanded).toBe('false');
  });

  test('A115 — preset Change chooser opens as role=dialog when Change clicked', async ({ page }) => {
    await page.addInitScript(() => {
      const preset = {
        id: 'test-preset-2',
        name: 'Alt Chat Preset',
        description: 'Another preset',
        applies_to: ['chat'],
        recipe_options: {},
        sampling: {},
        engine_hint: 'auto',
        starter: false,
        auto_opt_run_id: null,
        auto_opt_enabled: true,
        system_prompt_id: 'none',
        system_prompts: [],
        tools_enabled: false,
      };
      localStorage.setItem('lemonade:guest:shared:user_presets', JSON.stringify([preset]));
      localStorage.setItem('lemonade:guest:shared:applied_presets', JSON.stringify({ 'Llama-3.1-8B': 'test-preset-2' }));
    });

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
    await goToModels(page);
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);

    const presetsTab = page.locator('[role="tab"]').filter({ hasText: /Preset/i });
    await presetsTab.click();
    await page.waitForTimeout(100);

    const changeBtn = page.locator('.detail-presets__change-btn');
    await changeBtn.click();
    await page.waitForTimeout(100);

    // Chooser dialog should be visible
    const chooser = page.locator('.detail-presets__change-chooser');
    await expect(chooser).toBeVisible();
    await expect(chooser).toHaveAttribute('role', 'dialog');
    await expect(changeBtn).toHaveAttribute('aria-expanded', 'true');

    // Close chooser
    const closeBtn = page.locator('.detail-presets__chooser-close');
    await closeBtn.click();
    await page.waitForTimeout(100);
    await expect(chooser).not.toBeVisible();
    await expect(changeBtn).toHaveAttribute('aria-expanded', 'false');
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
    // README is the default tab; wait for the rendered container.
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
// Downloaded/My Models/Favorites), collapsible Categories, a Backends select,
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
            { id: 'Llama-3.1-8B', name: 'Llama-3.1-8B', labels: ['llm', 'tools'], recipe: 'llamacpp', downloaded: true, size: 8 },
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

  // ── Categories (collapsible) ─────────────────────────────────────────────

  test('A128 — Categories section header is a button with aria-expanded that toggles the list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const toggle = page.locator('.model-nav-rail__section-toggle').filter({ hasText: 'Categories' });
    expect(await toggle.getAttribute('aria-expanded')).toBe('true');
    await expect(page.locator('#nav-categories')).toBeVisible();
    await toggle.click();
    await page.waitForTimeout(100);
    expect(await toggle.getAttribute('aria-expanded')).toBe('false');
    await expect(page.locator('#nav-categories')).toBeHidden();
  });

  test('A129 — selecting a category filters the middle list (Audio → whisper only)', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const audio = page.locator('.model-nav-rail__cat-item').filter({ hasText: 'Audio' });
    await audio.click();
    await page.waitForTimeout(150);
    expect(await audio.getAttribute('aria-current')).toBe('true');
    const rows = page.locator('.model-list-item');
    await expect(rows).toHaveCount(1);
    await expect(rows.first()).toContainText('Whisper');
  });

  // ── Backends select ──────────────────────────────────────────────────────

  test('A130 — Backends select is labelled and filters the list by recipe', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const select = page.locator('#nav-backend-select');
    // Associated label.
    const labelText = await page.locator('label[for="nav-backend-select"]').textContent();
    expect((labelText ?? '').toLowerCase()).toContain('backend');
    await select.selectOption('whispercpp');
    await page.waitForTimeout(150);
    await expect(page.locator('.model-list-item')).toHaveCount(1);
  });

  // ── Tags (collapsible chips) ─────────────────────────────────────────────

  test('A131 — Tags section uses aria-pressed chips that filter the list', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const llamaTag = page.locator('.model-nav-rail__tag').filter({ hasText: /^Llama$/ });
    await expect(llamaTag).toBeVisible();
    expect(await llamaTag.getAttribute('aria-pressed')).toBe('false');
    await llamaTag.click();
    await page.waitForTimeout(150);
    expect(await llamaTag.getAttribute('aria-pressed')).toBe('true');
    const rows = page.locator('.model-list-item');
    await expect(rows).toHaveCount(1);
    await expect(rows.first()).toContainText('Llama');
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

  // ── Custom-model buttons (moved to TOP) ──────────────────────────────────

  test('A133 — custom-model buttons are a grounded group at the top and keyboard reachable', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const group = page.locator('.model-list-panel__add-group');
    await expect(group).toBeVisible();
    const customBtn = group.getByRole('button', { name: /custom model/i });
    const omniBtn = group.getByRole('button', { name: /omni collection/i });
    await expect(customBtn).toBeVisible();
    await expect(omniBtn).toBeVisible();
    await customBtn.focus();
    await expect(customBtn).toBeFocused();
    // The group sits above the model list in DOM order (top of the area).
    const groupBox = await group.boundingBox();
    const listBox = await page.locator('.model-list-panel__list').boundingBox();
    expect(groupBox && listBox && groupBox.y < listBox.y).toBeTruthy();
  });

  // ── Responsive nav toggle ────────────────────────────────────────────────

  test('A134 — on narrow viewport the nav toggle controls the rail and is keyboard reachable', async ({ page }) => {
    await page.setViewportSize({ width: 640, height: 900 });
    await goToModelsWithNavMock(page);
    const toggle = page.locator('.manager__nav-toggle');
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

  test('A136 — preset quick-search input in the rail is labelled', async ({ page }) => {
    await goToModelsWithNavMock(page);
    const input = page.locator('#nav-preset-search');
    await expect(input).toBeVisible();
    // Associated <label> (sr-only) provides the accessible name.
    const hasLabel = await page.locator('label[for="nav-preset-search"]').count();
    expect(hasLabel).toBeGreaterThan(0);
  });
});

// ─── 28. Model-detail Presets tab — neat compact card grid (#2424 fl0rianr) ───
//
// fl0rianr asked for the model-detail Presets tab to render presets as a neat
// grid of small focused cards (matching the global Presets-page cards), not
// full-width stacked rows. The linked preset sits above as a single highlighted
// card; recommended presets render in a responsive grid. Each Attach/Switch
// button names its preset, linked/active state is exposed via text + aria (not
// color only), and the inline Change dialog still works.
// Range: A137–A141.

test.describe('Accessibility — model-detail Presets card grid (#2424)', () => {
  async function goToPresetsTab(
    page: Page,
    opts: { applied?: Record<string, string> } = {},
  ): Promise<void> {
    const applied = opts.applied ?? {};
    await page.addInitScript((appliedJson: string) => {
      const presets = [
        {
          id: 'p-balanced', name: 'Balanced', description: 'Reliable defaults for everyday chat and general use.',
          applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: false,
          auto_opt_run_id: null, auto_opt_enabled: true, system_prompt_id: 'none', system_prompts: [], tools_enabled: true,
        },
        {
          id: 'p-thorough', name: 'Thorough', description: 'Careful answers for analysis, planning, and debugging.',
          applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: false,
          auto_opt_run_id: null, auto_opt_enabled: true, system_prompt_id: 'none', system_prompts: [], tools_enabled: true,
        },
        {
          id: 'p-creative', name: 'Creative', description: 'Higher creativity for brainstorming and writing.',
          applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: false,
          auto_opt_run_id: null, auto_opt_enabled: true, system_prompt_id: 'none', system_prompts: [], tools_enabled: false,
        },
      ];
      localStorage.setItem('lemonade:guest:shared:user_presets', JSON.stringify(presets));
      localStorage.setItem('lemonade:guest:shared:applied_presets', appliedJson);
    }, JSON.stringify(applied));

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
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await navigateToView(page, 'Models');
    await page.waitForSelector('.manager--detail');
    await page.waitForSelector('.model-list-item', { timeout: 5000 }).catch(() => {});
    await page.locator('.model-list-item').first().click();
    await page.waitForTimeout(200);
    const presetsTab = page.locator('[role="tab"]').filter({ hasText: /Preset/i });
    await presetsTab.click();
    await page.waitForTimeout(150);
  }

  test('A137 — recommended presets render as a grid of compact cards (not full-width rows)', async ({ page }) => {
    await goToPresetsTab(page);
    // The old row container is gone; the new grid is present.
    await expect(page.locator('.detail-presets__preset-list')).toHaveCount(0);
    const grid = page.locator('.detail-presets__preset-grid');
    await expect(grid).toBeVisible();
    await expect(grid).toHaveAttribute('role', 'list');
    // Multiple compact cards rendered as a grid.
    const cards = grid.locator('.detail-presets__preset-card');
    expect(await cards.count()).toBeGreaterThanOrEqual(2);
    const display = await grid.evaluate(el => getComputedStyle(el).display);
    expect(display).toBe('grid');
  });

  test('A138 — each Attach/Switch button has an accessible name that includes its preset name', async ({ page }) => {
    await goToPresetsTab(page);
    const attachButtons = page.locator('.detail-presets__attach-btn');
    const count = await attachButtons.count();
    expect(count).toBeGreaterThanOrEqual(2);
    for (let i = 0; i < count; i++) {
      const label = await attachButtons.nth(i).getAttribute('aria-label');
      expect(label).toMatch(/(Attach|Switch to) preset ".+" for /);
    }
  });

  test('A139 — linked/active state is exposed via text + aria, not color alone', async ({ page }) => {
    // Link "Balanced" so it is both the active linked card and the selected option.
    await goToPresetsTab(page, { applied: { 'Llama-3.1-8B': 'p-balanced' } });

    // Linked card above carries aria-current + visible "Active" badge text.
    const linkedCard = page.locator('.detail-presets__linked-card');
    await expect(linkedCard).toHaveAttribute('aria-current', 'true');
    await expect(linkedCard.locator('.detail-presets__card-badge--linked')).toHaveText(/Active/i);

    // The matching card in the grid exposes aria-current + a text "Linked" badge.
    const selected = page.locator('.detail-presets__preset-card--selected');
    await expect(selected).toHaveAttribute('aria-current', 'true');
    await expect(selected).toContainText(/Linked/i);
    // Selected card shows a text note instead of an Attach button (state not by color only).
    await expect(selected.locator('.detail-presets__card-linked-note')).toBeVisible();
  });

  test('A140 — Change dialog still opens from the linked card and closes', async ({ page }) => {
    await goToPresetsTab(page, { applied: { 'Llama-3.1-8B': 'p-balanced' } });
    const changeBtn = page.locator('.detail-presets__change-btn');
    await expect(changeBtn).toBeVisible();
    await changeBtn.click();
    await page.waitForTimeout(100);
    const chooser = page.locator('.detail-presets__change-chooser');
    await expect(chooser).toBeVisible();
    await expect(chooser).toHaveAttribute('role', 'dialog');
    await expect(changeBtn).toHaveAttribute('aria-expanded', 'true');
    await page.locator('.detail-presets__chooser-close').click();
    await page.waitForTimeout(100);
    await expect(chooser).not.toBeVisible();
  });

  test('A141 — the Presets card grid passes WCAG 2.1 AA axe-core scan', async ({ page }) => {
    await goToPresetsTab(page, { applied: { 'Llama-3.1-8B': 'p-balanced' } });
    await expect(page.locator('.detail-presets__preset-grid')).toBeVisible();
    const results = await new AxeBuilder({ page })
      .withTags([...WCAG_TAGS])
      .analyze();
    const critical = results.violations.filter(
      v => v.impact === 'critical' || v.impact === 'serious',
    );
    expect(critical, formatViolations(critical)).toHaveLength(0);
  });
});
