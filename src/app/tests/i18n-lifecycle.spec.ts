import { expect, test, type Page } from '@playwright/test';

type RequestCounter = { stats: number };

async function installSystemLocaleControl(page: Page): Promise<void> {
  await page.addInitScript(() => {
    try { localStorage.removeItem('lemonade:locale'); } catch { /* best effort */ }
    let testLanguages = ['en-US'];
    Object.defineProperty(navigator, 'languages', {
      configurable: true,
      get: () => [...testLanguages],
    });
    Object.defineProperty(navigator, 'language', {
      configurable: true,
      get: () => testLanguages[0] || 'en-US',
    });
    (window as any).__setTestSystemLanguages = (next: string[]) => {
      testLanguages = [...next];
      window.dispatchEvent(new Event('languagechange'));
    };
  });
}

async function switchSystemLocale(page: Page, locale: string): Promise<void> {
  await page.evaluate((nextLocale) => {
    const setLanguages = (window as any).__setTestSystemLanguages;
    if (typeof setLanguages !== 'function') throw new Error('System locale test control is unavailable.');
    setLanguages([nextLocale]);
  }, locale);
  await expect(page.locator('html')).toHaveAttribute('lang', locale);
}

async function installHostSettings(page: Page): Promise<void> {
  await page.addInitScript(() => {
    (window as any).api = {
      getSettings: async () => ({
        baseURL: { value: 'http://127.0.0.1:13305', useDefault: false },
        apiKey: { value: 'persisted-key', useDefault: false },
      }),
      saveSettings: async () => undefined,
    };
  });
}

async function mockRendererApi(
  page: Page,
  models: Array<Record<string, unknown>> = [],
  counter?: RequestCounter,
): Promise<void> {
  await page.route('**/api/v1/health**', route => route.fulfill({
    json: {
      status: 'ok',
      version: 'test',
      model_loaded: null,
      websocket_port: 0,
      all_models_loaded: [],
      max_models: {},
    },
  }));
  await page.route(/\/api\/v1\/models(?:\?[^#]*)?$/, route => route.fulfill({ json: { data: models } }));
  await page.route('**/api/v1/system-info**', route => route.fulfill({
    json: {
      lemonade_version: 'test',
      devices: { cpu: { name: 'Test CPU', available: true } },
      recipes: {
        llamacpp: {
          default_backend: 'cpu',
          backends: { cpu: { state: 'installed', version: 'test', devices: ['cpu'] } },
        },
      },
      cloud: { providers: [] },
    },
  }));
  await page.route('**/api/v1/stats**', route => {
    if (counter) counter.stats += 1;
    return route.fulfill({ json: {} });
  });
  await page.route('**/api/v1/system-stats**', route => route.fulfill({
    json: { cpu_percent: 12, memory_gb: 2, gpu_percent: -1, vram_gb: 0, npu_percent: -1 },
  }));
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: [] }));
  await page.route('**/internal/config**', route => route.fulfill({
    json: { models_dir: 'auto', extra_models_dir: '' },
  }));
}

test.describe('i18n locale-switch lifecycle invariants', () => {
  test('preserves unsaved ConnectView drafts when the system locale changes', async ({ page }) => {
    await installSystemLocaleControl(page);
    await installHostSettings(page);
    await mockRendererApi(page);

    await page.goto('/#/connect/server');
    const hostInput = page.locator('#host-input');
    const keyInput = page.locator('#key-input');
    await expect(hostInput).toHaveValue('http://127.0.0.1:13305');
    await expect(keyInput).toHaveValue('persisted-key');

    await hostInput.fill('http://unsaved.example:4242');
    await keyInput.fill('unsaved-secret');
    await switchSystemLocale(page, 'de-DE');

    await expect(hostInput).toHaveValue('http://unsaved.example:4242');
    await expect(keyInput).toHaveValue('unsaved-secret');
  });

  test('preserves an unsaved RouterEditor draft when the system locale changes', async ({ page }) => {
    await installSystemLocaleControl(page);
    await page.addInitScript(() => {
      const request = {
        version: '1',
        model_name: 'user.locale-router',
        recipe: 'collection.router',
        components: ['candidate-a', 'candidate-b'],
        routing: {
          candidates: ['candidate-a', 'candidate-b'],
          default_model: 'candidate-a',
          rules: [{ id: 'rule-1', match: { keywords_any: ['hello'] }, route_to: 'candidate-a' }],
        },
      };
      localStorage.setItem('lemonade:router_collections', JSON.stringify({
        version: 1,
        routers: [{
          model_name: 'user.locale-router',
          display_name: 'Locale Router',
          request,
          createdAt: 1_700_000_000_000,
          updatedAt: 1_700_000_000_000,
        }],
      }));
    });
    await mockRendererApi(page, [
      { id: 'candidate-a', name: 'candidate-a', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
      { id: 'candidate-b', name: 'candidate-b', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
    ]);

    await page.goto('/#/models');
    const routerRow = page.locator('.model-list-panel__list .workspace-list-row').filter({ hasText: 'Locale Router' });
    await expect(routerRow).toBeVisible();
    await routerRow.click();
    await page.getByRole('button', { name: 'Open router editor', exact: true }).click();
    await expect(page.locator('.router-editor')).toBeVisible();

    const routerNameInput = page.locator('.router-editor__form-grid input').first();
    await routerNameInput.fill('Unsaved Router Draft');
    await switchSystemLocale(page, 'de-DE');

    await expect(page.locator('.router-editor')).toBeVisible();
    await expect(routerNameInput).toHaveValue('Unsaved Router Draft');
  });

  test('does not refresh a paused Monitor when the system locale changes', async ({ page }) => {
    await installSystemLocaleControl(page);
    const requests: RequestCounter = { stats: 0 };
    await mockRendererApi(page, [], requests);

    await page.goto('/#/dashboard/performance');
    const pollToggle = page.locator('[data-dashboard-poll-toggle]');
    await expect(pollToggle).toBeVisible();
    await expect.poll(() => requests.stats).toBeGreaterThan(0);

    await pollToggle.click();
    await expect(pollToggle).toHaveClass(/is-paused/);
    await page.waitForTimeout(100);
    const beforeLocaleSwitch = requests.stats;

    await switchSystemLocale(page, 'de-DE');
    await page.waitForTimeout(300);
    expect(requests.stats).toBe(beforeLocaleSwitch);
  });

  test('preserves Inspect test-modal state when the system locale changes', async ({ page }) => {
    await installSystemLocaleControl(page);
    await mockRendererApi(page, [
      { id: 'optimizer-model', name: 'optimizer-model', labels: ['llm', 'tool'], recipe: 'llamacpp', downloaded: true },
    ]);

    await page.goto('/#/dashboard/telemetry');
    await page.waitForSelector('[data-view="dashboard"]');
    await page.evaluate(() => {
      const trace = {
        id: 'locale-trace',
        traceId: 'trace-locale',
        spanId: 'span-locale',
        kind: 'LLM',
        operation: 'chat',
        status: 'ok',
        model: 'optimizer-model',
        timestamp: new Date().toISOString(),
        startTimeMs: Date.now(),
        dur: 100,
        temp: 0.7,
        topP: 1,
        topK: 50,
        max: 256,
        messages: [
          { role: 'system', content: 'Original system prompt' },
          { role: 'user', content: 'Original user prompt' },
        ],
        output: 'answer',
        improveData: {
          critique: [{ category: 'clarity', severity: 'low', finding: 'Clearer wording', rationale: 'Test fixture' }],
          parameter_diff: {
            temperature: { suggested: 0.5, rationale: 'Test fixture' },
            system_vs_user_split: false,
          },
          optimized_prompt: {
            system_instructions: 'Optimized system prompt',
            user_prompt: 'Optimized user prompt',
          },
          key_improvements: ['Clearer wording'],
        },
        improveRawOutput: '{}',
      };
      (window as any).inspectStore.setState({ traces: [trace], selectedTraceId: trace.id });
    });

    const improveTab = page.getByRole('tab', { name: 'Improve', exact: true });
    await expect(improveTab).toBeVisible();
    await improveTab.click();
    await page.getByRole('button', { name: 'Test', exact: true }).click();

    const testMessage = page.locator('#test-message-input');
    await expect(testMessage).toBeVisible();
    await testMessage.fill('Unsaved test message');
    await switchSystemLocale(page, 'de-DE');

    await expect(testMessage).toBeVisible();
    await expect(testMessage).toHaveValue('Unsaved test message');
  });
});
