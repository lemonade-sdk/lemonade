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
