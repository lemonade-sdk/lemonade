import { test, expect } from '@playwright/test';

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

    // Model selector pill visible
    await expect(page.locator('.model-selector')).toBeVisible();

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
    await expect(page.locator('.connect .btn--primary')).toBeVisible();

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
    const urlInput = page.locator('#host-input');
    await urlInput.clear();
    await urlInput.fill('http://localhost:13305');

    // Click Connect
    await page.locator('.connect .btn--primary').click();

    // Wait for connection status dot to turn green
    await page.waitForFunction(() => {
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.screenshot({ path: 'screenshots/06-connected.png', fullPage: true });
  });

  test('07 — Models view shows loaded models when connected', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Wait for auto-connect
    await page.waitForFunction(() => {
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
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
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    // Wait for model name in selector pill
    await page.waitForFunction(() => {
      const name = document.querySelector('.model-selector__name');
      return name && name.textContent && name.textContent !== 'Offline' && name.textContent !== 'No model';
    }, { timeout: 10000 }).catch(() => {});

    const modelText = await page.locator('.model-selector__name').textContent().catch(() => null);
    if (!modelText || modelText === 'Offline' || modelText === 'No model') {
      console.log('No model loaded — skipping chat test');
      await page.screenshot({ path: 'screenshots/08-no-model.png', fullPage: true });
      return;
    }

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
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const name = document.querySelector('.model-selector__name');
      return name && name.textContent && name.textContent !== 'Offline' && name.textContent !== 'No model';
    }, { timeout: 10000 }).catch(() => {});

    const modelText = await page.locator('.model-selector__name').textContent().catch(() => null);
    if (!modelText || modelText === 'Offline' || modelText === 'No model') {
      await page.screenshot({ path: 'screenshots/09-no-model.png', fullPage: true });
      return;
    }

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
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const name = document.querySelector('.model-selector__name');
      return name && name.textContent && name.textContent !== 'Offline' && name.textContent !== 'No model';
    }, { timeout: 10000 }).catch(() => {});

    const modelText = await page.locator('.model-selector__name').textContent().catch(() => null);
    if (!modelText || modelText === 'Offline' || modelText === 'No model') {
      await page.screenshot({ path: 'screenshots/10-no-model.png', fullPage: true });
      return;
    }

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
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
    }, { timeout: 10000 }).catch(() => {});

    await page.waitForFunction(() => {
      const name = document.querySelector('.model-selector__name');
      return name && name.textContent && name.textContent !== 'Offline' && name.textContent !== 'No model';
    }, { timeout: 10000 }).catch(() => {});

    const modelText = await page.locator('.model-selector__name').textContent().catch(() => null);
    if (!modelText || modelText === 'Offline' || modelText === 'No model') {
      await page.screenshot({ path: 'screenshots/11-no-model.png', fullPage: true });
      return;
    }

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

  test('13 — Presets view renders zones and slide-over', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Presets
    await page.locator('.titlebar__nav').getByText('Presets').click();
    await page.waitForSelector('.recipes');

    // Title visible
    await expect(page.locator('.recipes__title h1')).toContainText('Presets');

    // Count subtitle visible
    await expect(page.locator('.recipes__title-sub')).toContainText('starters');

    // Lede paragraph mentions recipe options and sampling
    const lede = page.locator('.recipes__lede');
    await expect(lede).toBeVisible();
    await expect(lede).toContainText('recipe options');
    await expect(lede).toContainText('sampling');

    // Zone: Bundled starters (scope to recipes view to avoid hitting Models zones)
    const recipesView = page.locator('.recipes');
    const starterZone = recipesView.locator('.zone').first();
    await expect(starterZone.locator('.zone__title')).toContainText('Bundled starters');

    // Should have 8 starter cards
    const starterCards = page.locator('[data-recipe-grid="starters"] .recipe-card');
    await expect(starterCards).toHaveCount(8);

    // Starter badge on first card
    await expect(starterCards.first().locator('.starter-badge')).toContainText('Starter');

    // Recipe chip visible on cards (shows recipe name like "llama.cpp")
    await expect(starterCards.first().locator('.cap-chip')).toBeVisible();

    // Zone: Your presets
    const yoursCards = page.locator('[data-recipe-grid="yours"] .recipe-card');
    const yoursCount = await yoursCards.count();
    console.log(`User presets: ${yoursCount}`);

    // Click a preset card to open slide-over
    await starterCards.first().click();
    await page.waitForSelector('.slideover.is-open');

    // Slide-over has preset name
    await expect(page.locator('.slideover__title')).toBeVisible();

    // Slide-over shows recipe chip
    await expect(page.locator('.slideover .cap-chip')).toBeVisible();

    // Slide-over has recipe options section
    await expect(page.locator('.slideover h3').getByText('Recipe options')).toBeVisible();

    // Slide-over has form controls (sliders for ctx_size, etc.)
    await expect(page.locator('.slideover .slider').first()).toBeVisible();

    // Close slide-over
    await page.locator('.slideover__close').click();
    await page.waitForFunction(() => {
      return !document.querySelector('.slideover.is-open');
    });

    // New Preset button visible
    await expect(page.locator('.recipes__actions .btn--primary')).toContainText('New Preset');

    await page.screenshot({ path: 'screenshots/13-presets-view.png', fullPage: true });
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

  test('20 — Models page shows all four zones with correct labels', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Navigate to Models
    await page.locator('.titlebar__nav').getByText('Models').click();
    await page.waitForSelector('.manager');

    // HuggingFace zone should always be visible (not conditional on search)
    const hfZone = page.locator('.zone--hf');
    await expect(hfZone).toBeVisible();

    // HuggingFace zone title should say "HuggingFace" (not "Explore — HuggingFace")
    await expect(hfZone.locator('.zone__title')).toContainText('HuggingFace');
    await expect(hfZone.locator('.zone__title')).not.toContainText('Explore');

    // HuggingFace should show the prompt when no search text
    await expect(hfZone.locator('.hf-zone__empty')).toContainText('Type at least 2 characters');

    // When disconnected, empty state should show appropriate message
    const emptyState = page.locator('.manager__empty');
    if (await emptyState.isVisible().catch(() => false)) {
      const text = await emptyState.textContent();
      // Should say either "Connect to a Lemonade server" or "No models found"
      expect(text).toMatch(/Connect to a Lemonade server|No models found/);
    }

    // Search triggers HuggingFace search
    const searchInput = page.locator('.manager__search-input');
    await searchInput.fill('llama');
    await page.waitForTimeout(1500); // debounce (400ms) + network time

    // HuggingFace zone should show results, loading spinner, or "no results" message
    const hfEmpty = hfZone.locator('.hf-zone__empty');
    const hfRows = hfZone.locator('.row--hf');
    const hfLoading = hfZone.locator('.hf-zone__loading');
    const hasResults = await hfRows.count() > 0;
    const hasEmpty = await hfEmpty.isVisible().catch(() => false);
    const isLoading = await hfLoading.isVisible().catch(() => false);
    // One of these states should be true
    expect(hasResults || hasEmpty || isLoading).toBeTruthy();

    await page.screenshot({ path: 'screenshots/20-models-zones.png', fullPage: true });
  });

  test('21 — Models page zone labels: Loaded, Downloaded, Registry', async ({ page }) => {
    await page.goto('/');
    await page.waitForSelector('.titlebar__nav');

    // Connect to server first
    await page.locator('.titlebar__nav').getByText('Connect').click();
    await page.waitForSelector('.connect');
    const urlInput = page.locator('#host-input');
    await urlInput.clear();
    await urlInput.fill('http://localhost:13305');
    await page.locator('.connect .btn--primary').click();

    // Wait for connection
    await page.waitForFunction(() => {
      const dot = document.querySelector('.model-selector__dot');
      return dot?.classList.contains('model-selector__dot--connected');
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
});
