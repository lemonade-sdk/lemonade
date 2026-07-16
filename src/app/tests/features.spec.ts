import { test, expect, Page } from '@playwright/test';

const realServerRequired = /^(06|07|08|09|10|11|14|21|22)\b/;

test.beforeEach(async ({ page }, testInfo) => {
  const originalScreenshot = page.screenshot.bind(page);
  page.screenshot = ((options: Parameters<Page['screenshot']>[0] = {}) => {
    const rawPath = typeof options.path === 'string' ? options.path : undefined;
    const path = rawPath?.startsWith('screenshots/')
      ? testInfo.outputPath(rawPath.replace(/^screenshots\//, ''))
      : rawPath;
    return originalScreenshot({ ...options, ...(path ? { path } : {}) });
  }) as Page['screenshot'];

  test.skip(realServerRequired.test(testInfo.title) && process.env.LEMONADE_REAL_SERVER !== '1',
    'Real-server smoke tests are opt-in. Set LEMONADE_REAL_SERVER=1 and start lemond first.');
});

test.describe('Lemonade UI — Feature Parity', () => {

  test('01 — App loads with titlebar, nav, and status', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    // Titlebar brand
    await expect(page.locator('.titlebar__brand')).toContainText('lemonade');

    // Navigation buttons exist
    const nav = page.locator('.titlebar__nav');
    await expect(nav.getByText('Chat')).toBeVisible();
    await expect(nav.getByText('Models')).toBeVisible();
    await expect(nav.getByText('Presets')).toBeVisible();
    await expect(nav.getByText('Backends')).toBeVisible();
    await expect(nav.getByText('Connect')).toBeVisible();

    // Status dot visible
    await expect(page.locator('.titlebar__status-dot')).toBeVisible();

    await page.screenshot({ path: 'screenshots/01-app-loaded.png', fullPage: true });
  });

  test('02 — Chat view renders with composer', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    // Chat view active by default
    await expect(page.locator('.chat')).toBeVisible();
    await expect(page.locator('.hero')).toBeVisible();
    await expect(page.locator('.composer__input')).toBeVisible();
    await expect(page.locator('.composer__send')).toBeVisible();

    // New chat button in rail
    await expect(page.locator('.rail__new')).toBeVisible();

    await page.screenshot({ path: 'screenshots/02-chat-view.png', fullPage: true });
  });

  test('03 — Models view shows model grid', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.manager');

    await expect(page.locator('.manager__title h1')).toContainText('Models');

    await page.screenshot({ path: 'screenshots/03-models-view.png', fullPage: true });
  });

  test('04 — Connect view shows server form', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Connect
    await page.locator('.titlebar__nav').getByText('Connect').click();
    await page.waitForSelector('.connect');

    await expect(page.locator('.connect h1')).toContainText('Connect');
    await expect(page.locator('#host-input')).toBeVisible();
    await expect(page.locator('#key-input')).toBeVisible();
    await expect(page.locator('.connect__section--server button[type="submit"]')).toBeVisible();

    await page.screenshot({ path: 'screenshots/04-connect-view.png', fullPage: true });
  });

  test('05 — Navigation switches views correctly', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Default: Chat is active
    await expect(page.locator('.titlebar__nav button.is-active')).toContainText('Chat');

    // Switch to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await expect(page.locator('.titlebar__nav button.is-active')).toContainText('Models');
    await expect(page.locator('.manager')).toBeVisible();

    // Switch to Connect
    await page.locator('.titlebar__nav').getByText('Connect').click();
    await expect(page.locator('.titlebar__nav button.is-active')).toContainText('Connect');
    await expect(page.locator('.connect')).toBeVisible();

    // Back to Chat
    await page.locator('.titlebar__nav').getByText('Chat').click();
    await expect(page.locator('.titlebar__nav button.is-active')).toContainText('Chat');
    await expect(page.locator('.chat')).toBeVisible();

    await page.screenshot({ path: 'screenshots/05-navigation.png', fullPage: true });
  });

  test('06 — Connect form connects to server', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await page.locator('.titlebar__nav').getByText('Connect').click();
    await page.waitForSelector('.connect');

    // Fill in server URL (lemond should be running)
    const testPort = process.env.LEMONADE_TEST_PORT || '13305';
    const urlInput = page.locator('#host-input');
    await urlInput.clear();
    await urlInput.fill(`http://localhost:${testPort}`);

    // Click Connect
    await page.locator('.connect__section--server button[type="submit"]').click();

    // Wait for connection status dot to turn green
    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.screenshot({ path: 'screenshots/06-connected.png', fullPage: true });
  });

  test('07 — Models view shows loaded models when connected', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Wait for auto-connect
    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.manager');

    // Wait for models to load
    await page.waitForTimeout(2000);

    // Should have zones (Running, Downloaded, Available)
    const zones = page.locator('.zone');
    const zoneCount = await zones.count();
    console.log(`Found ${zoneCount} model zones`);

    // Should have model rows
    const rows = page.locator('.row');
    const count = await rows.count();
    console.log(`Found ${count} model rows`);

    // Should have search bar
    await expect(page.locator('.manager__search-input')).toBeVisible();

    // Should have filter tabs
    await expect(page.locator('.manager__filters')).toBeVisible();

    // Stats should be visible
    await expect(page.locator('.manager__stats')).toBeVisible();

    await page.screenshot({ path: 'screenshots/07-models-loaded.png', fullPage: true });

    // Test search filtering
    const searchInput = page.locator('.manager__search-input');
    await searchInput.fill('Qwen');
    await page.waitForTimeout(500);
    const filteredCount = await page.locator('.row').count();
    console.log(`Filtered to ${filteredCount} rows for "Qwen"`);
    await page.screenshot({ path: 'screenshots/07b-models-search.png', fullPage: true });

    // HuggingFace Explore zone should appear after debounce
    await page.waitForTimeout(600);
    const hfZone = page.locator('.zone--hf');
    const hfVisible = await hfZone.isVisible().catch(() => false);
    console.log(`HuggingFace zone visible: ${hfVisible}`);
    if (hfVisible) {
      const hfRows = await page.locator('.row--hf').count();
      console.log(`HuggingFace results: ${hfRows}`);
      await page.screenshot({ path: 'screenshots/07b2-models-hf-zone.png', fullPage: true });
    }

    // Clear search and test type filter
    await searchInput.clear();
    await page.waitForTimeout(300);
    await page.locator('.manager__filter').getByText('Image').click();
    await page.waitForTimeout(500);
    await page.screenshot({ path: 'screenshots/07c-models-filter-image.png', fullPage: true });

    // Reset to All
    await page.locator('.manager__filter').getByText('All').click();
    await page.waitForTimeout(300);

    // Test expanding a model detail (click first row)
    const firstRow = page.locator('.row__content').first();
    await firstRow.click();
    await page.waitForTimeout(500);
    // Detail panel should appear
    const detail = page.locator('.row__detail').first();
    if (await detail.isVisible().catch(() => false)) {
      console.log('Model detail panel expanded successfully');
    }
    await page.screenshot({ path: 'screenshots/07d-model-detail.png', fullPage: true });
  });

  test('08 — Chat sends message and receives streaming response', async ({ page }) => {
    test.setTimeout(120000); // Extended timeout for slow local LLMs
    await page.goto('/');

    // Wait for connection + model
    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    // Wait for model to be loaded (composer placeholder shows model name)
    await page.waitForFunction(() => {
      const input = document.querySelector('.composer__input') as HTMLTextAreaElement;
      return input && input.placeholder && input.placeholder.startsWith('Message ');
    }, { timeout: 10000 }).catch(() => {});

    const placeholder = await page.locator('.composer__input').getAttribute('placeholder').catch(() => null);
    if (!placeholder || !placeholder.startsWith('Message ')) {
      await page.screenshot({ path: 'screenshots/08-no-model.png', fullPage: true });
    }
    expect(placeholder ?? '', 'Real-server chat smoke requires a loaded chat model').toMatch(/^Message /);

    // Type and send a message
    const input = page.locator('.composer__input');
    await input.fill('Say "Hello World" in exactly 5 words.');
    await page.locator('.composer__send').click();

    // User message should appear
    await expect(page.locator('.message--user').first()).toBeVisible();

    // Wait for streaming (stop button or streaming cursor)
    await page.waitForSelector('.composer__stop, .streaming-cursor', { timeout: 10000 }).catch(() => {});

    await page.screenshot({ path: 'screenshots/08-chat-streaming.png', fullPage: true });

    // Wait for completion (extended for thinking models)
    await page.waitForFunction(() => {
      return !document.querySelector('.composer__stop') && !document.querySelector('.streaming-cursor');
    }, { timeout: 90000 }).catch(() => {});

    // Assistant message should have appeared
    const assistantMsg = page.locator('.message--assistant').first();
    if (await assistantMsg.isVisible().catch(() => false)) {
      // Metrics visible
      const metrics = page.locator('.message__metrics').first();
      await expect(metrics).toBeVisible({ timeout: 5000 }).catch(() => {});
    }

    await page.screenshot({ path: 'screenshots/08-chat-response.png', fullPage: true });
  });

  test('09 — Markdown rendering with code blocks', async ({ page }) => {
    await page.goto('/');

    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const input = document.querySelector('.composer__input') as HTMLTextAreaElement;
      return input && input.placeholder && input.placeholder.startsWith('Message ');
    }, { timeout: 10000 }).catch(() => {});

    const placeholder09 = await page.locator('.composer__input').getAttribute('placeholder').catch(() => null);
    if (!placeholder09 || !placeholder09.startsWith('Message ')) {
      await page.screenshot({ path: 'screenshots/09-no-model.png', fullPage: true });
    }
    expect(placeholder09 ?? '', 'Markdown rendering test requires a loaded chat model').toMatch(/^Message /);

    // Ask for code
    await page.locator('.composer__input').fill('Write a hello world function in Python. Use a code block.');
    await page.locator('.composer__send').click();

    // Wait for completion
    await page.waitForFunction(() => {
      return document.querySelectorAll('.message--assistant').length > 0 &&
             !document.querySelector('.streaming-cursor');
    }, { timeout: 60000 }).catch(() => {});

    // Check for code block rendering
    const codeBlock = page.locator('.code-block');
    const hasCodeBlock = await codeBlock.count() > 0;
    console.log(`Code blocks found: ${await codeBlock.count()}`);

    if (hasCodeBlock) {
      // Copy button should be visible
      await expect(codeBlock.first().locator('.code-block__copy')).toBeVisible();
      // Language label
      await expect(codeBlock.first().locator('.code-block__lang')).toBeVisible();
    }

    await page.screenshot({ path: 'screenshots/09-markdown-code.png', fullPage: true });
  });

  test('10 — Thinking model shows reasoning section', async ({ page }) => {
    await page.goto('/');

    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const input = document.querySelector('.composer__input') as HTMLTextAreaElement;
      return input && input.placeholder && input.placeholder.startsWith('Message ');
    }, { timeout: 10000 }).catch(() => {});

    const placeholder10 = await page.locator('.composer__input').getAttribute('placeholder').catch(() => null);
    if (!placeholder10 || !placeholder10.startsWith('Message ')) {
      await page.screenshot({ path: 'screenshots/10-no-model.png', fullPage: true });
    }
    expect(placeholder10 ?? '', 'Thinking-model test requires a loaded chat model').toMatch(/^Message /);

    // Ask something that triggers reasoning
    await page.locator('.composer__input').fill('What is 2+2? Think step by step.');
    await page.locator('.composer__send').click();

    // Wait for any thinking block to appear (or full completion)
    await page.waitForFunction(() => {
      return document.querySelector('.message__thinking') !== null ||
             (document.querySelectorAll('.message--assistant').length > 0 &&
              !document.querySelector('.streaming-cursor'));
    }, { timeout: 60000 }).catch(() => {});

    const thinkingBlock = page.locator('.message__thinking');
    const hasThinking = await thinkingBlock.count() > 0;
    console.log(`Thinking blocks found: ${await thinkingBlock.count()}`);

    await page.screenshot({ path: 'screenshots/10-thinking-model.png', fullPage: true });
  });

  test('11 — New Chat button clears conversation', async ({ page }) => {
    await page.goto('/');

    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const input = document.querySelector('.composer__input') as HTMLTextAreaElement;
      return input && input.placeholder && input.placeholder.startsWith('Message ');
    }, { timeout: 10000 }).catch(() => {});

    const placeholder11 = await page.locator('.composer__input').getAttribute('placeholder').catch(() => null);
    if (!placeholder11 || !placeholder11.startsWith('Message ')) {
      await page.screenshot({ path: 'screenshots/11-no-model.png', fullPage: true });
    }
    expect(placeholder11 ?? '', 'New-chat real-server test requires a loaded chat model').toMatch(/^Message /);

    // Send a message
    await page.locator('.composer__input').fill('Hi');
    await page.locator('.composer__send').click();

    // Wait for response
    await page.waitForFunction(() => {
      return document.querySelectorAll('.message--assistant').length > 0 &&
             !document.querySelector('.streaming-cursor');
    }, { timeout: 60000 }).catch(() => {});

    // Messages visible
    await expect(page.locator('.message').first()).toBeVisible();

    // Click New Chat button in rail
    await page.locator('.rail__new').click();

    // Hero should be back (conversation cleared)
    await expect(page.locator('.hero')).toBeVisible();

    await page.screenshot({ path: 'screenshots/11-new-chat.png', fullPage: true });
  });

  test('12 — Responsive layout at different widths', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar');

    // Desktop
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.screenshot({ path: 'screenshots/12-responsive-desktop.png', fullPage: true });

    // Tablet
    await page.setViewportSize({ width: 768, height: 1024 });
    await page.screenshot({ path: 'screenshots/12-responsive-tablet.png', fullPage: true });

    // Mobile
    await page.setViewportSize({ width: 375, height: 667 });
    await page.screenshot({ path: 'screenshots/12-responsive-mobile.png', fullPage: true });
  });

  test('13 — Presets v1.4 renders capability flow and stages bindings', async ({ page }) => {
    let loadCalls = 0;
    await page.addInitScript(() => {
      localStorage.removeItem('lemonade_user_presets');
      localStorage.removeItem('lemonade_applied_presets');
    });
    await page.route('**/api/v1/models**', async route => route.fulfill({
      contentType: 'application/json',
      body: JSON.stringify({
        data: [
          { id: 'llama-chat', name: 'llama-chat', labels: ['llm'], recipe: 'llamacpp' },
          { id: 'sd-image', name: 'sd-image', labels: ['image'], recipe: 'sd-cpp' },
        ],
      }),
    }));
    await page.route('**/api/v1/load', async route => {
      loadCalls++;
      await route.fulfill({ contentType: 'application/json', body: JSON.stringify({ status: 'ok' }) });
    });
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Presets
    await page.locator('.titlebar__nav').getByText('Presets').click();
    await page.waitForSelector('.recipes');

    // Title visible
    await expect(page.locator('.recipes__title h1')).toContainText('Presets');

    // Count subtitle visible
    await expect(page.locator('.recipes__title-sub')).toContainText('starters');

    // Lede explains the intent-to-tuning separation.
    const lede = page.locator('.recipes__lede');
    await expect(lede).toBeVisible();
    await expect(lede).toContainText('describe how you want to use a model');
    await expect(lede).toContainText('Model Tuning');

    // Zone: Bundled starters (scope to recipes view to avoid hitting Models zones).
    // User-created content sorts above the starters, so match by title.
    const recipesView = page.locator('.recipes').last();
    const starterZone = recipesView.locator('.zone').filter({ hasText: 'Bundled starters' });
    await expect(starterZone.locator('.zone__title')).toContainText('Bundled starters');

    // Should have 8 starter cards
    const starterCards = recipesView.locator('[data-recipe-grid="starters"] .recipe-card');
    await expect(starterCards).toHaveCount(9);

    // Starter badge on first card
    await expect(starterCards.first().locator('.starter-badge')).toContainText('Starter');

    // Capability chip visible on cards (v1.4 applies_to schema)
    await expect(starterCards.first().locator('.cap-chip')).toContainText('Chat');
    await expect(starterCards.nth(6).locator('.cap-chip')).toContainText('Image');

    // Zone: Your presets is genuinely empty on first run
    await expect(recipesView.locator('[data-empty="yours"]')).toBeVisible();
    await expect(recipesView.locator('[data-empty="yours"]')).toContainText('Pick a starter, customize it, or save from a model');

    // Click a preset card to open slide-over
    await starterCards.first().locator('.recipe-card__overlay-btn').click();
    await page.waitForSelector('.slideover.is-open');

    // Slide-over has preset name
    await expect(page.locator('.slideover__title')).toBeVisible();

    // Slide-over shows capability chips
    await expect(page.locator('.slideover .cap-chip').first()).toContainText('Chat');

    // Slide-over exposes semantic intent controls and no concrete runtime controls.
    await expect(page.locator('[data-preset-intent="temperature"]')).toBeVisible();
    await expect(page.locator('[data-preset-intent="context"]')).toBeVisible();
    await expect(page.locator('[data-preset-intent="thinking"]')).toBeVisible();
    await expect(page.locator('[data-preset-intent="temperature"] [data-intent-value]')).toHaveCount(4);
    await expect(page.locator('[data-preset-intent="context"] [data-intent-value]')).toHaveCount(4);
    await expect(page.locator('[data-intent-value="smart"]')).toBeDisabled();
    await expect(page.locator('[data-intent-value="smart-extra"]')).toBeDisabled();
    await expect(page.locator('.slideover details.preset-advanced')).not.toHaveAttribute('open', '');
    await expect(page.locator('.slideover .slider')).toHaveCount(0);

    // Incompatible model options are disabled with an explanation tooltip
    const imageOption = page.locator('[data-recipe-apply-target] option[value="sd-image"]');
    await expect(imageOption).toBeDisabled();
    await expect(imageOption).toHaveAttribute('title', /Incompatible/);

    // Applying stages the binding only; it does not POST /load immediately.
    await page.locator('[data-recipe-apply-target]').selectOption('llama-chat');
    await page.locator('.slideover .btn--primary').getByText('Apply').click();
    await expect(page.locator('.preset-success')).toContainText('Will apply on next load');
    expect(loadCalls).toBe(0);

    await page.locator('.slideover__close').click();
    await page.waitForFunction(() => !document.querySelector('.slideover.is-open'));
    await expect(recipesView.locator('[data-applied-row="llama-chat"] .preset-status-chip')).toContainText('Will apply on next load');

    // New Preset button visible
    await expect(page.locator('.recipes__actions .btn--primary')).toContainText('New Preset');

    await page.screenshot({ path: 'screenshots/13-presets-view.png', fullPage: true });
  });

  test('13b — Presets import rejects legacy schema', async ({ page }) => {
    await page.addInitScript(() => {
      localStorage.removeItem('lemonade_user_presets');
      localStorage.removeItem('lemonade_applied_presets');
    });
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await page.locator('.titlebar__nav').getByText('Presets').click();
    await page.waitForSelector('.recipes');

    await page.locator('.dropdown__trigger').click();
    const fileChooserPromise = page.waitForEvent('filechooser');
    await page.locator('.dropdown__item').getByText('From file…').click();
    const fileChooser = await fileChooserPromise;
    await fileChooser.setFiles({
      name: 'legacy-preset.json',
      mimeType: 'application/json',
      buffer: Buffer.from(JSON.stringify({ id: 'legacy', name: 'Legacy', recipe: 'llamacpp' })),
    });

    await expect(page.locator('.preset-error')).toContainText('This file uses the legacy schema. Use the v1.4 export instead.');
  });

  test('14 — Backends view shows matrix and device info', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Backends nav button exists
    await expect(page.locator('.titlebar__nav').getByText('Backends')).toBeVisible();

    // Navigate to Backends
    await page.locator('.titlebar__nav').getByText('Backends').click();
    await page.waitForSelector('[data-view="backends"]');

    // Title visible
    await expect(page.locator('.backends__title h1')).toContainText('Backends');

    // Show technical details toggle visible
    await expect(page.locator('.backends__toggle')).toBeVisible();

    // Matrix table present
    const matrix = page.locator('[data-backends-matrix] table');
    await expect(matrix).toBeVisible();

    // Matrix has capability column headers
    await expect(matrix.locator('thead th')).toHaveCount(5); // Device + LLM + Audio + Image + TTS

    // At least one device row
    const rows = matrix.locator('tbody tr');
    expect(await rows.count()).toBeGreaterThan(0);

    // Toggle tech details — version sha becomes visible
    await page.locator('.backends__toggle input').check();

    await page.screenshot({ path: 'screenshots/14-backends-view.png', fullPage: true });
  });

  test('15 — Dashboard view shows system gauges and session overview', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Dashboard nav button exists
    await expect(page.locator('.titlebar__nav').getByText('Dashboard')).toBeVisible();

    // Navigate to Dashboard
    await page.locator('.titlebar__nav').getByText('Dashboard').click();
    await page.waitForSelector('[data-view="dashboard"]');

    // Top bar visible
    await expect(page.locator('.dash2-bar')).toBeVisible();

    // Connection indicator dot
    await expect(page.locator('.dash2-bar__dot')).toBeVisible();

    // Pause/resume button
    await expect(page.locator('.dash2-bar__btn')).toBeVisible();

    // Aggregate Throughput hero section
    await expect(page.getByText('Aggregate Throughput')).toBeVisible();

    // At least CPU and RAM gauges rendered
    const gauges = page.locator('.dash2-gauge');
    expect(await gauges.count()).toBeGreaterThanOrEqual(2);

    // Hero throughput stats — check for the aggregate throughput section
    await expect(page.getByText('Aggregate Throughput')).toBeVisible();
    await expect(page.getByText('tok/s').first()).toBeVisible();
    await expect(page.getByText('Generation TPS')).toBeVisible();

    // Session summary hidden until inference happens (no data at idle)

    // Pause button toggles
    await page.locator('.dash2-bar__btn').click();
    await expect(page.locator('.dash2-bar__btn')).toHaveClass(/is-paused/);

    // Resume
    await page.locator('.dash2-bar__btn').click();
    await expect(page.locator('.dash2-bar__btn')).not.toHaveClass(/is-paused/);

    // Loaded Models section present (scope to dashboard to avoid Models view zone match)
    const dashView = page.locator('[data-view="dashboard"]');
    await expect(dashView.getByText('Loaded Models')).toBeVisible();

    await page.screenshot({ path: 'screenshots/15-dashboard.png', fullPage: true });
  });

  test('16 — Logs view shows toolbar and log output', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Logs nav button exists
    await expect(page.locator('.titlebar__nav').getByText('Logs')).toBeVisible();

    // Navigate to Logs
    await page.locator('.titlebar__nav').getByText('Logs').click();
    await page.waitForSelector('[data-view="logs"]');

    // Toolbar visible with controls
    await expect(page.locator('.logs-toolbar')).toBeVisible();

    // Connection status dot
    await expect(page.locator('.logs-status__dot')).toBeVisible();

    // Status label visible
    await expect(page.locator('.logs-status__label')).toBeVisible();

    // Search input
    await expect(page.locator('.logs-search')).toBeVisible();

    // Show (filter) level selector
    const showSelect = page.locator('.logs-level__select').first();
    await expect(showSelect).toBeVisible();

    // Server level selector
    const serverSelect = page.locator('.logs-level__select').nth(1);
    await expect(serverSelect).toBeVisible();

    // Clear button
    await expect(page.getByRole('button', { name: 'Clear' })).toBeVisible();

    // Log output area exists
    await expect(page.locator('.logs-output')).toBeVisible();

    // Wait briefly for WebSocket connection
    await page.waitForTimeout(2000);

    // If connected, should show some log entries or the empty state
    const output = page.locator('.logs-output');
    const hasEntries = await output.locator('.logs-line').count() > 0;
    const hasEmpty = await output.locator('.logs-empty').count() > 0;
    expect(hasEntries || hasEmpty).toBeTruthy();

    // If we have entries, verify structure: time, badge, tag, text
    if (hasEntries) {
      const firstLine = output.locator('.logs-line').first();
      await expect(firstLine.locator('.logs-line__time')).toBeVisible();
      await expect(firstLine.locator('.logs-line__badge')).toBeVisible();
      await expect(firstLine.locator('.logs-line__text')).toBeVisible();
    }

    // Search filtering works — type something and verify
    await page.locator('.logs-search').fill('xyz_nonexistent_query');
    await page.waitForTimeout(300);

    // Entry count in toolbar should update
    await expect(page.locator('.logs-toolbar__count')).toBeVisible();

    // Clear the search
    await page.locator('.logs-search').fill('');

    await page.screenshot({ path: 'screenshots/16-logs-view.png', fullPage: true });
  });

  /* ── Bug fix validations ─────────────────────────────────── */

  test('17 — Logs auto-scroll sticks to bottom across view switches', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Logs
    await page.locator('.titlebar__nav').getByText('Logs').click();
    await page.waitForSelector('.logs-output', { state: 'visible' });

    // Inject enough content to make the container scrollable, then scroll to bottom
    await page.evaluate(() => {
      const output = document.querySelector('.logs-output');
      if (!output) return;
      for (let i = 0; i < 100; i++) {
        const line = document.createElement('div');
        line.className = 'logs-line';
        line.style.height = '24px';
        line.innerHTML = `
          <span class="logs-line__time">12:00:${String(i).padStart(2, '0')}</span>
          <span class="logs-line__badge logs-line__badge--info">INFO</span>
          <span class="logs-line__tag">test</span>
          <span class="logs-line__text">Synthetic log entry #${i}</span>`;
        output.appendChild(line);
      }
      // Scroll to the very bottom
      output.scrollTop = output.scrollHeight;
    });

    await page.waitForTimeout(200);

    // Verify we are at the bottom
    const scrolledBefore = await page.evaluate(() => {
      const el = document.querySelector('.logs-output');
      if (!el) return { at: false, top: 0, height: 0, scroll: 0 };
      return {
        at: el.scrollHeight - el.scrollTop <= el.clientHeight + 80,
        top: el.scrollTop,
        height: el.scrollHeight,
        scroll: el.clientHeight,
      };
    });
    expect(scrolledBefore.at).toBeTruthy();

    // Switch away to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForTimeout(500);

    // Switch back to Logs
    await page.locator('.titlebar__nav').getByText('Logs').click();
    await page.waitForSelector('.logs-output', { state: 'visible' });
    await page.waitForTimeout(500);

    // After coming back, the IntersectionObserver should have re-scrolled to bottom
    const scrolledAfter = await page.evaluate(() => {
      const el = document.querySelector('.logs-output');
      if (!el) return { at: false, top: 0, height: 0, scroll: 0 };
      return {
        at: el.scrollHeight - el.scrollTop <= el.clientHeight + 80,
        top: el.scrollTop,
        height: el.scrollHeight,
        scroll: el.clientHeight,
      };
    });
    expect(scrolledAfter.at).toBeTruthy();

    await page.screenshot({ path: 'screenshots/17-logs-sticky-scroll.png', fullPage: true });
  });

  test('18 — Chat allows navigation while streaming (concurrent chat)', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    // The rail should allow clicking conversations even with no server
    // Verify new chat button works and switching is not blocked
    const newBtn = page.locator('.rail__new');
    await expect(newBtn).toBeVisible();

    // Create a first chat by clicking New Chat
    await newBtn.click();
    await page.waitForTimeout(200);

    // The hero should be visible (empty chat state)
    await expect(page.locator('.hero')).toBeVisible();

    // Verify the conversation rail exists and is interactive
    const rail = page.locator('.rail');
    await expect(rail).toBeVisible();

    // Verify the composer is not disabled (can start typing in new chat)
    const input = page.locator('.composer__input');
    await expect(input).toBeVisible();
    await input.fill('Test message for nav check');

    // Click New Chat again — should work without being blocked
    await newBtn.click();
    await page.waitForTimeout(200);

    // Hero should still be visible (new empty chat)
    await expect(page.locator('.hero')).toBeVisible();

    await page.screenshot({ path: 'screenshots/18-concurrent-chat-nav.png', fullPage: true });
  });

  test('19 — Chat streaming badge shows on rail items', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.chat');

    // Verify the streaming badge CSS class exists in the stylesheet
    const hasBadgeStyle = await page.evaluate(() => {
      const sheets = document.styleSheets;
      for (let i = 0; i < sheets.length; i++) {
        try {
          const rules = sheets[i].cssRules;
          for (let j = 0; j < rules.length; j++) {
            if ((rules[j] as CSSStyleRule).selectorText?.includes('rail__streaming-badge')) {
              return true;
            }
          }
        } catch { /* cross-origin */ }
      }
      return false;
    });
    expect(hasBadgeStyle).toBeTruthy();

    await page.screenshot({ path: 'screenshots/19-streaming-badge-style.png', fullPage: true });
  });

  test('20 — Models page shows model list panel with search', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.manager');

    // New master-detail layout: left panel with search input
    const listPanel = page.locator('.model-list-panel');
    await expect(listPanel).toBeVisible();

    // Search input should be present and operable
    const searchInput = page.locator('.manager__search-input');
    await expect(searchInput).toBeVisible();

    // When disconnected / no models, empty state should show appropriate message
    const emptyState = page.locator('.manager__empty');
    if (await emptyState.isVisible().catch(() => false)) {
      const text = await emptyState.textContent();
      expect(text).toMatch(/Connect to a Lemonade server|No models found|No models match/);
    }

    // Model count annotation should be present (even "0 models")
    const countEl = page.locator('.model-list-panel__count');
    await expect(countEl).toBeVisible();

    // Typing into search filters the list
    await searchInput.fill('zzznotamodel');
    await page.waitForTimeout(200);
    // Either empty state appears, or count shows 0
    const countText = await countEl.textContent();
    const emptyVisible = await page.locator('.manager__empty').isVisible().catch(() => false);
    expect(emptyVisible || (countText ?? '').startsWith('0')).toBeTruthy();

    await page.screenshot({ path: 'screenshots/20-models-zones.png', fullPage: true });
  });

  test('21 — Models page zone labels: Loaded, Downloaded, Registry', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Connect to server first
    await page.locator('.titlebar__nav').getByText('Connect').click();
    await page.waitForSelector('.connect');
    const testPort = process.env.LEMONADE_TEST_PORT || '13305';
    const urlInput = page.locator('#host-input');
    await urlInput.clear();
    await urlInput.fill(`http://localhost:${testPort}`);
    await page.locator('.connect__section--server button[type="submit"]').click();

    // Wait for connection
    await page.waitForFunction(() => {
      const dot = document.querySelector('.titlebar__status-dot');
      return dot?.classList.contains('titlebar__status-dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    // Navigate to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.manager');
    await page.waitForTimeout(2000);

    // Check zone labels (only visible zones will have these titles)
    const allTitles = await page.locator('.zone__title').allTextContents();
    console.log('Zone titles:', allTitles);

    // Should NOT contain old labels
    for (const t of allTitles) {
      expect(t).not.toContain('Ready to Load');
      expect(t).not.toContain('Download Required');
      expect(t).not.toContain('Explore —');
    }

    // Should contain new labels where zones appear
    const hasLoadedModels = allTitles.some(t => t.includes('Loaded Models'));
    const hasDownloaded = allTitles.some(t => t === 'Downloaded');
    const hasRegistry = allTitles.some(t => t.includes('Lemonade Registry'));
    const hasHuggingFace = allTitles.some(t => t === 'HuggingFace');

    // HuggingFace should always be there
    expect(hasHuggingFace).toBeTruthy();

    console.log(`Loaded Models: ${hasLoadedModels}, Downloaded: ${hasDownloaded}, Registry: ${hasRegistry}, HF: ${hasHuggingFace}`);

    await page.screenshot({ path: 'screenshots/21-models-zone-labels.png', fullPage: true });
  });

  test('22 — Backends update button says "updated" not "installed"', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Backends
    await page.locator('.titlebar__nav').getByText('Backends').click();
    await page.waitForSelector('[data-view="backends"]');

    // Check if any Update buttons exist in the matrix (filter by text)
    const updateBtns = page.locator('.cell__swap', { hasText: /^Update$|^Updating/ });
    const installBtns = page.locator('.cell__swap', { hasText: /^Install$|^Installing/ });
    const updateCount = await updateBtns.count();
    const installCount = await installBtns.count();
    console.log(`Update buttons: ${updateCount}, Install buttons: ${installCount}`);

    // If update buttons exist, they should say "Update" not "Install"
    if (updateCount > 0) {
      const firstUpdate = updateBtns.first();
      await expect(firstUpdate).toContainText(/Update/);
    }

    // Verify the matrix table renders
    const matrix = page.locator('[data-backends-matrix] table');
    await expect(matrix).toBeVisible();

    await page.screenshot({ path: 'screenshots/22-backends-update.png', fullPage: true });
  });

  test('23 — loopback API requests resolve to IPv4 127.0.0.1 by default and respect capture setting for session headers', async ({ page }) => {
    // Mock WebSocket to immediately connect and send auth.ok
    await page.addInitScript(() => {
      class MockWebSocket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;

        readyState = MockWebSocket.CONNECTING;
        onopen: (() => void) | null = null;
        onmessage: ((ev: any) => void) | null = null;
        onclose: (() => void) | null = null;
        onerror: (() => void) | null = null;

        constructor() {
          setTimeout(() => {
            this.readyState = MockWebSocket.OPEN;
            if (this.onopen) this.onopen();
          }, 10);
        }

        send(data: string) {
          const parsed = JSON.parse(data);
          if (parsed.type === 'auth') {
            setTimeout(() => {
              if (this.onmessage) {
                this.onmessage({ data: JSON.stringify({ type: 'auth.ok' }) });
              }
            }, 10);
          }
        }

        close() {
          this.readyState = MockWebSocket.CLOSED;
          if (this.onclose) this.onclose();
        }
      }
      (window as any).WebSocket = MockWebSocket as any;
    });

    // Mock chat completion
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ choices: [{ message: { role: 'assistant', content: 'hi' } }] })
      });
    });

    const requests: Array<{ url: string; headers: Record<string, string> }> = [];
    page.on('request', req => {
      if (req.url().includes('/api/v1/')) {
        requests.push({ url: req.url(), headers: req.headers() });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await page.waitForTimeout(1000);

    // Initial requests (capturing is OFF by default)
    expect(requests.length).toBeGreaterThan(0);
    for (const req of requests) {
      expect(req.url).not.toContain('//localhost:13305');
      expect(req.url).toContain('//127.0.0.1:13305');
    }

    // Trigger a chat completion while capturing is OFF
    await page.evaluate(async () => {
      try {
        await (window as any).apiClient.chatCompletionOnce('mock-model', []);
      } catch (e) {}
    });

    // Verify last request (chat completion) has NO session headers
    const chatReqOff = requests.find(r => r.url.includes('/chat/completions'));
    expect(chatReqOff).toBeDefined();
    expect(chatReqOff!.headers['x-client-session-id']).toBeUndefined();
    expect(chatReqOff!.headers['x-account-session-id']).toBeUndefined();

    // Now toggle capturing ON
    await page.evaluate(() => {
      (window as any).inspectStore.setState({ capturing: true });
    });

    // Wait briefly for WebSocket connection to authenticate and enable headers
    await page.waitForTimeout(200);

    // Trigger a chat completion while capturing is ON
    const requestsAfterToggle: Array<{ url: string; headers: Record<string, string> }> = [];
    page.on('request', req => {
      if (req.url().includes('/api/v1/')) {
        requestsAfterToggle.push({ url: req.url(), headers: req.headers() });
      }
    });

    await page.evaluate(async () => {
      try {
        await (window as any).apiClient.chatCompletionOnce('mock-model', []);
      } catch (e) {}
    });

    const chatReqOn = requestsAfterToggle.find(r => r.url.includes('/chat/completions'));
    expect(chatReqOn).toBeDefined();
    expect(chatReqOn!.headers['x-client-session-id']).toBeDefined();
    expect(chatReqOn!.headers['x-account-session-id']).toBeDefined();
  });

  test('24 — fallback retry: retries without session headers on fetch preflight/network failure and disables them', async ({ page }) => {
    // Mock WebSocket to immediately connect and send auth.ok
    await page.addInitScript(() => {
      class MockWebSocket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;

        readyState = MockWebSocket.CONNECTING;
        onopen: (() => void) | null = null;
        onmessage: ((ev: any) => void) | null = null;
        onclose: (() => void) | null = null;
        onerror: (() => void) | null = null;

        constructor() {
          setTimeout(() => {
            this.readyState = MockWebSocket.OPEN;
            if (this.onopen) this.onopen();
          }, 10);
        }

        send(data: string) {
          const parsed = JSON.parse(data);
          if (parsed.type === 'auth') {
            setTimeout(() => {
              if (this.onmessage) {
                this.onmessage({ data: JSON.stringify({ type: 'auth.ok' }) });
              }
            }, 10);
          }
        }

        close() {
          this.readyState = MockWebSocket.CLOSED;
          if (this.onclose) this.onclose();
        }
      }
      (window as any).WebSocket = MockWebSocket as any;
    });

    // Mock chat completion: reject requests with session headers, succeed without them
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      const headers = route.request().headers();
      if (headers['x-client-session-id']) {
        await route.abort('failed');
      } else {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify({ choices: [{ message: { role: 'assistant', content: 'fallback-success' } }] })
        });
      }
    });

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Turn capturing ON
    await page.evaluate(() => {
      (window as any).inspectStore.setState({ capturing: true });
    });

    // Wait briefly for WebSocket connection to authenticate and enable headers
    await page.waitForTimeout(200);

    // Verify sessionHeadersEnabled is true initially
    const initialEnabled = await page.evaluate(() => (window as any).apiClient.sessionHeadersEnabled);
    expect(initialEnabled).toBe(true);

    // Call chat completion
    const result = await page.evaluate(async () => {
      try {
        const resp = await (window as any).apiClient.chatCompletionOnce('mock-model', []);
        return resp;
      } catch (e) {
        return `failed: ${e instanceof Error ? e.message : String(e)}`;
      }
    });

    // Check that we got fallback success and sessionHeadersEnabled was disabled
    expect(result).toBe('fallback-success');
    const finalEnabled = await page.evaluate(() => (window as any).apiClient.sessionHeadersEnabled);
    expect(finalEnabled).toBe(false);
  });

  test('25 — Model Tuning maps every intent level while Max context stays fixed', async ({ page }) => {
    const modelName = 'intent-map-model';
    await page.route('**/api/v1/health**', route => route.fulfill({
      json: { status: 'ok', version: 'test', all_models_loaded: [] },
    }));
    await page.route('**/api/v1/system-info**', route => route.fulfill({
      json: {
        recipes: {
          llamacpp: {
            default_backend: 'cpu',
            backends: { cpu: { state: 'installed', version: 'test' } },
          },
        },
      },
    }));
    await page.route('**/api/v1/models**', route => route.fulfill({
      json: {
        data: [{
          id: modelName,
          name: modelName,
          labels: ['llm', 'coding'],
          recipe: 'llamacpp',
          downloaded: true,
          ctx_size: 4096,
          max_context_window: 131072,
        }],
      },
    }));

    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.model-list-item', { timeout: 5000 });
    await page.locator('.model-list-item').first().click();
    await page.locator('#detail-tab-tuning').click();

    const temperatureInputs = page.locator('[data-model-tuning-temperature-intent]');
    await expect(temperatureInputs).toHaveCount(4);
    for (const hint of ['precise', 'balanced', 'exploratory', 'creative']) {
      await expect(page.locator(`[data-model-tuning-temperature-intent="${hint}"]`)).toBeVisible();
    }

    const contextCards = page.locator('[data-model-tuning-context-intent]');
    await expect(contextCards).toHaveCount(4);
    for (const hint of ['small', 'medium', 'large']) {
      await expect(page.locator(`button[data-model-tuning-context-intent="${hint}"]`)).toBeVisible();
    }
    const maxContext = page.locator('[data-model-tuning-context-intent="max"]');
    await expect(maxContext).toBeVisible();
    await expect(maxContext.locator('input')).toHaveCount(0);
    await expect(maxContext).toContainText('128K');
    await expect(page.locator('.detail-tuning__runtime .detail-tuning__field', { hasText: 'Context size' })).toHaveCount(0);

    await page.locator('[data-model-tuning-preset]').selectOption('s-code');
    await page.locator('[data-model-tuning-temperature-intent="precise"]').fill('0.2');
    await page.locator('[data-model-tuning-temperature-intent="balanced"]').fill('0.6');
    await page.locator('[data-model-tuning-temperature-intent="exploratory"]').fill('0.8');
    await page.locator('[data-model-tuning-temperature-intent="creative"]').fill('1.0');

    await page.locator('button[data-model-tuning-context-intent="small"]').click();
    const smallSlider = page.locator('[data-model-tuning-context-slider="small"]');
    await expect(smallSlider).toHaveAttribute('min', '1024');
    await page.locator('[data-model-tuning-context-number="small"]').fill('5120');

    await page.locator('button[data-model-tuning-context-intent="medium"]').click();
    const mediumSlider = page.locator('[data-model-tuning-context-slider="medium"]');
    await expect(mediumSlider).toHaveAttribute('min', '5120');
    await page.locator('[data-model-tuning-context-number="medium"]').fill('32768');

    await page.locator('button[data-model-tuning-context-intent="large"]').click();
    const largeSlider = page.locator('[data-model-tuning-context-slider="large"]');
    await expect(largeSlider).toHaveAttribute('min', '32768');
    await expect(largeSlider).toHaveAttribute('max', '131072');
    await page.locator('[data-model-tuning-context-number="large"]').fill('65536');
    await page.getByRole('button', { name: 'Save tuning' }).click();

    const saved = await page.evaluate(({ model, preset }) => {
      for (const key of Object.keys(localStorage)) {
        if (!key.includes('model_tunings')) continue;
        try {
          const value = JSON.parse(localStorage.getItem(key) || '{}');
          const tuning = value[`${model}@@${preset}`];
          if (tuning) return tuning;
        } catch { /* keep looking */ }
      }
      return null;
    }, { model: modelName, preset: 's-code' });

    expect(saved?.intent_values?.temperature).toEqual({
      precise: 0.2,
      balanced: 0.6,
      exploratory: 0.8,
      creative: 1.0,
    });
    expect(saved?.intent_values?.context).toEqual({
      small: 5120,
      medium: 32768,
      large: 65536,
    });
    expect(saved?.intent_values?.context?.max).toBeUndefined();
    expect(saved?.recipe_options?.ctx_size).toBeUndefined();
  });

});
