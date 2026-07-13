import { test, expect, Page } from '@playwright/test';

const CHAT_MODEL = 'org/chat-model';
const VISION_MODEL = 'org/vision-model';

const MODELS_PAYLOAD = {
  data: [
    { id: CHAT_MODEL, name: CHAT_MODEL, labels: ['llm'], recipe: 'llamacpp', downloaded: true, max_context_window: 32768 },
    { id: VISION_MODEL, name: VISION_MODEL, labels: ['llm', 'vision'], recipe: 'llamacpp', downloaded: true, max_context_window: 32768 },
  ],
};

const SYSTEM_INFO_PAYLOAD = {
  'Physical Memory': '64.0 GB',
  recipes: { llamacpp: { default_backend: 'vulkan', backends: { vulkan: { state: 'installed' } } } },
};

const COMPLETED_RUN = {
  id: 'run-done',
  model: CHAT_MODEL,
  status: 'completed',
  budget: 'standard',
  created_at: '2026-07-01T10:00:00Z',
  finished_at: '2026-07-01T10:15:00Z',
  summary: 'Vulkan wins on this GPU',
  lemonade_version: '1.2.0',
};

const COMPLETED_RUN_DETAIL = {
  ...COMPLETED_RUN,
  answers: { parallel: { mode: 'single' }, kv_cache_quant: 'q8_0', ram_headroom: 'normal', allow_network: true },
  stages: [
    { name: 'Probe memory fit', status: 'completed', duration_ms: 3200 },
    { name: 'Benchmark backends', status: 'completed', duration_ms: 431000 },
    { name: 'Pick recommendation', status: 'completed', duration_ms: 90 },
  ],
  measurements: {
    fit: [{ ctx: 8192, fits: true }],
    bench: [
      { b: 512, ub: 256, pp_ts: 950.2, tg_ts: 32.1 },
      { b: 1024, ub: 512, pp_ts: 1020.5, tg_ts: 31.9 },
    ],
  },
  result: {
    primary: {
      label: 'Vulkan · ctx 8K',
      llamacpp_backend: 'vulkan',
      ctx_size: 8192,
      mmproj_enabled: false,
      llamacpp_args: '-b 512 -ub 256 -ctk q8_0 -ctv q8_0',
      rationale: ['Fastest prompt processing on this GPU', 'Fits with normal RAM headroom'],
      expected: { pp_ts: { d0: 1020.5, d30000: 640.2 }, tg_ts: 31.9, vram_mib: 5400 },
    },
    alternatives: [
      {
        label: 'CPU fallback',
        llamacpp_backend: 'cpu',
        ctx_size: 16384,
        llamacpp_args: '-b 256 -ub 128',
        rationale: ['Largest usable context'],
        tradeoff: 'Much slower generation',
        expected: { pp_ts: { d0: 120.0, d30000: 80.1 }, tg_ts: 9.5, vram_mib: 0 },
      },
    ],
    sampling_defaults: { temperature: 0.7, top_p: 0.9, top_k: 40, min_p: 0.05, source: 'gguf' },
  },
};

interface MockOptions {
  loadedModels?: Array<Record<string, unknown>>;
  runs?: () => Array<Record<string, unknown>>;
  details?: Record<string, Record<string, unknown>>;
}

async function mockServer(page: Page, options: MockOptions = {}) {
  const loaded = options.loadedModels || [];
  await page.route('**/api/v1/health**', route => route.fulfill({
    json: { status: 'ok', version: 'test', all_models_loaded: loaded },
  }));
  await page.route('**/api/v1/models**', route => route.fulfill({ json: MODELS_PAYLOAD }));
  await page.route('**/api/v1/system-info**', route => route.fulfill({ json: SYSTEM_INFO_PAYLOAD }));
  await page.route('**/api/v1/autoopt/runs', route => route.fulfill({
    json: { runs: options.runs ? options.runs() : [] },
  }));
  await page.route('**/api/v1/autoopt/runs/*', route => {
    const id = decodeURIComponent(route.request().url().split('/').pop() || '');
    const detail = options.details?.[id];
    if (route.request().method() === 'DELETE') {
      return route.fulfill({ json: { status: 'ok' } });
    }
    if (detail) return route.fulfill({ json: detail });
    return route.fulfill({ status: 404, json: { error: 'unknown run' } });
  });
}

async function openPresets(page: Page) {
  await page.goto('/');
  await page.waitForSelector('.titlebar__nav');
  await page.locator('.titlebar__nav').getByText('Presets').click();
  await page.waitForSelector('.recipes');
}

test.describe('AutoOpt wizard', () => {

  test('wizard flow — steps, RAM suggestion, vision skip, consent gate, start request body', async ({ page }) => {
    let startBody: Record<string, unknown> | null = null;
    let started = false;
    await mockServer(page, {
      loadedModels: [{ model_name: CHAT_MODEL, type: 'llm', recipe: 'llamacpp', device: 'gpu', checkpoint: '', backend_url: '', pid: 1, last_use: Date.now() }],
      runs: () => started ? [{ id: 'run-new', model: CHAT_MODEL, status: 'queued', budget: 'quick', created_at: new Date().toISOString() }] : [],
    });
    await page.route('**/api/v1/autoopt/start', async route => {
      startBody = route.request().postDataJSON();
      started = true;
      await route.fulfill({ status: 202, json: { id: 'run-new' } });
    });

    await openPresets(page);
    await page.locator('[data-autoopt-run-optimizer]').click();
    await page.waitForSelector('[data-autoopt-wizard]');

    // Model step: pre-filled with the loaded chat model.
    await expect(page.locator('[data-autoopt-model-select]')).toHaveValue(CHAT_MODEL, { timeout: 10000 });
    await page.locator('[data-autoopt-next]').click();

    // Expected-use step.
    await expect(page.locator('[data-autoopt-step="parallel"]')).toBeVisible();
    await page.locator('[data-autoopt-option="parallel:parallel"]').click();
    await page.locator('[data-autoopt-slots]').fill('4');
    await page.locator('[data-autoopt-next]').click();

    // KV cache quantization step.
    await expect(page.locator('[data-autoopt-step="kv"]')).toBeVisible();
    await page.locator('[data-autoopt-option="kv:q5_1"]').click();
    await page.locator('[data-autoopt-next]').click();

    // RAM headroom step with machine suggestion.
    await expect(page.locator('[data-autoopt-step="ram"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-ram-suggestion]')).toContainText('Suggested for this machine (64 GB RAM)');
    await expect(page.locator('[data-autoopt-option="ram:normal"]')).toHaveAttribute('aria-pressed', 'true');
    await page.locator('[data-autoopt-next]').click();

    // Vision step must be skipped for a text-only model.
    await expect(page.locator('[data-autoopt-step="budget"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-step="vision"]')).toHaveCount(0);
    await page.locator('[data-autoopt-option="budget:quick"]').click();

    // Consent gate: without the checkbox, Start stays disabled.
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-step="review"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-start]')).toBeDisabled();
    await page.locator('[data-autoopt-back]').click();
    await page.locator('[data-autoopt-consent]').check();
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-start]')).toBeEnabled();

    await page.locator('[data-autoopt-start]').click();
    await expect(page.locator('[data-autoopt-step="running"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-close-note]')).toContainText('the run continues on the server');

    expect(startBody).not.toBeNull();
    expect(startBody!.model).toBe(CHAT_MODEL);
    expect(startBody!.budget).toBe('quick');
    expect(startBody!.allow_unload).toBe(true);
    const answers = startBody!.answers as Record<string, unknown>;
    expect(answers.parallel).toEqual({ mode: 'parallel', slots: 4, dedicated: false });
    expect(answers.kv_cache_quant).toBe('q5_1');
    expect(answers.ram_headroom).toBe('normal');
    expect(answers.allow_network).toBe(true);
    expect(answers.use_vision).toBeUndefined();
  });

  test('wizard shows the vision step for vision-capable models', async ({ page }) => {
    await mockServer(page);
    await openPresets(page);
    await page.locator('[data-autoopt-run-optimizer]').click();
    await page.waitForSelector('[data-autoopt-wizard]');

    await page.locator('[data-autoopt-model-select]').selectOption(VISION_MODEL);
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-step="vision"]')).toBeVisible();
    await page.locator('[data-autoopt-option="vision:false"]').click();
    await expect(page.locator('[data-autoopt-option="vision:false"]')).toHaveAttribute('aria-pressed', 'true');
  });
});

test.describe('AutoOpt rail', () => {

  test('run lifecycle queued → running → completed via polling', async ({ page }) => {
    let phase = 0;
    await mockServer(page, {
      runs: () => {
        phase += 1;
        const base = { id: 'run-live', model: CHAT_MODEL, budget: 'standard', created_at: '2026-07-10T10:00:00Z' };
        if (phase <= 1) return [{ ...base, status: 'queued' }];
        if (phase <= 3) {
          return [{
            ...base,
            status: 'running',
            progress: { stage: 'Benchmark backends', stage_index: 1, stage_count: 3, percent: 40 },
          }];
        }
        return [{ ...base, status: 'completed', finished_at: '2026-07-10T10:20:00Z' }];
      },
      details: { 'run-live': COMPLETED_RUN_DETAIL },
    });

    await openPresets(page);
    const run = page.locator('[data-autoopt-run="run-live"]');
    await expect(run).toBeVisible({ timeout: 10000 });
    await expect(run.locator('.autoopt-status-chip--running')).toContainText('Benchmark backends · 2/3', { timeout: 15000 });
    await expect(run.locator('.autoopt-status-chip--completed')).toBeVisible({ timeout: 15000 });
    await expect(page.locator('[data-autoopt-announcement]')).toContainText(`AutoOpt run for ${CHAT_MODEL} completed.`);
  });

  test('failed run surfaces the server error in rail and detail', async ({ page }) => {
    const failedRun = {
      id: 'run-bad',
      model: CHAT_MODEL,
      status: 'failed',
      budget: 'standard',
      created_at: '2026-07-09T10:00:00Z',
      finished_at: '2026-07-09T10:03:00Z',
      error: 'llama-server exited with code 137 (out of memory)\nfull backend log follows',
    };
    await mockServer(page, {
      runs: () => [failedRun],
      details: {
        'run-bad': {
          ...failedRun,
          stages: [
            { name: 'Probe memory fit', status: 'completed', duration_ms: 3100 },
            { name: 'Benchmark backends', status: 'failed', duration_ms: 42000, error: 'llama-server exited with code 137 (out of memory)' },
            { name: 'Pick recommendation', status: 'skipped' },
          ],
        },
      },
    });

    await openPresets(page);
    const run = page.locator('[data-autoopt-run="run-bad"]');
    await expect(run).toBeVisible({ timeout: 10000 });
    await expect(run.locator('.autoopt-status-chip--failed')).toContainText('llama-server exited with code 137 (out of memory)');
    await expect(run.locator('.autoopt-status-chip--failed')).not.toContainText('full backend log');

    await page.locator('[data-autoopt-inspect="run-bad"]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('[data-autoopt-detail-error]')).toContainText('llama-server exited with code 137 (out of memory)');
    await expect(page.locator('.autoopt-stage--failed .autoopt-stage__error')).toContainText('out of memory');
  });

  test('cancel posts to the cancel endpoint and marks the run cancelled', async ({ page }) => {
    let cancelBody: Record<string, unknown> | null = null;
    await mockServer(page, {
      runs: () => [{
        id: 'run-active',
        model: CHAT_MODEL,
        status: cancelBody ? 'cancelled' : 'running',
        budget: 'standard',
        created_at: '2026-07-10T10:00:00Z',
        ...(cancelBody ? {} : { progress: { stage: 'Benchmark backends', stage_index: 1, stage_count: 3 } }),
      }],
    });
    await page.route('**/api/v1/autoopt/cancel', async route => {
      cancelBody = route.request().postDataJSON();
      await route.fulfill({ json: { status: 'ok' } });
    });

    await openPresets(page);
    const run = page.locator('[data-autoopt-run="run-active"]');
    await expect(run).toBeVisible({ timeout: 10000 });
    await page.locator('[data-autoopt-cancel="run-active"]').click();
    await expect(run.locator('.autoopt-status-chip--cancelled')).toBeVisible();
    expect(cancelBody).toEqual({ id: 'run-active' });
  });

  test('delete removes the run; a failed delete rolls the row back', async ({ page }) => {
    let failDelete = true;
    let deleted = false;
    await mockServer(page, { runs: () => deleted ? [] : [COMPLETED_RUN] });
    await page.unroute('**/api/v1/autoopt/runs/*');
    await page.route('**/api/v1/autoopt/runs/*', async route => {
      if (route.request().method() === 'DELETE') {
        if (failDelete) return route.fulfill({ status: 409, json: { error: 'run is busy' } });
        deleted = true;
        return route.fulfill({ json: { status: 'ok' } });
      }
      return route.fulfill({ json: COMPLETED_RUN_DETAIL });
    });

    await openPresets(page);
    const run = page.locator('[data-autoopt-run="run-done"]');
    await expect(run).toBeVisible({ timeout: 10000 });

    await page.locator('[data-autoopt-delete="run-done"]').click();
    await expect(run).toBeVisible();
    await expect(page.locator('[data-autoopt-rail-error]')).toContainText('run is busy');

    failDelete = false;
    await page.locator('[data-autoopt-delete="run-done"]').click();
    await expect(run).toHaveCount(0);
  });
});

test.describe('AutoOpt run detail actions', () => {

  async function openCompletedRunDetail(page: Page) {
    await mockServer(page, {
      runs: () => [COMPLETED_RUN],
      details: { 'run-done': COMPLETED_RUN_DETAIL },
    });
    await openPresets(page);
    await expect(page.locator('[data-autoopt-run="run-done"]')).toBeVisible({ timeout: 10000 });
    await page.locator('[data-autoopt-inspect="run-done"]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('[data-autoopt-recommendation]')).toBeVisible({ timeout: 10000 });
  }

  test('detail shows stages, batch ladder, recommendation, and alternatives', async ({ page }) => {
    await openCompletedRunDetail(page);

    await expect(page.locator('[data-autoopt-detail-stages] .autoopt-stage')).toHaveCount(3);
    await expect(page.locator('[data-autoopt-bench-ladder] tbody tr')).toHaveCount(2);
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('-b 512 -ub 256 -ctk q8_0 -ctv q8_0');
    await expect(page.locator('[data-autoopt-alternatives] tbody tr')).toHaveCount(2);
    await expect(page.locator('[data-autoopt-alternatives] thead')).toContainText('pp t/s @0');
    await expect(page.locator('[data-autoopt-alternatives] thead')).toContainText('pp t/s @30000');
  });

  test('"Create preset" writes an optimized preset and model tuning', async ({ page }) => {
    await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('Created preset');

    const stored = await page.evaluate((model) => {
      let preset: any = null;
      let tuning: any = null;
      for (const key of Object.keys(localStorage)) {
        if (key.includes('user_presets')) {
          const presets = JSON.parse(localStorage.getItem(key) || '[]');
          preset = presets.find((p: any) => p.auto_opt_run_id === 'run-done') || preset;
        }
        if (key.includes('model_tunings')) {
          const tunings = JSON.parse(localStorage.getItem(key) || '{}');
          for (const [tuningKey, value] of Object.entries(tunings)) {
            if (tuningKey.startsWith(`${model}@@`) && (value as any).source === 'optimized') tuning = value;
          }
        }
      }
      return { preset, tuning };
    }, CHAT_MODEL);

    expect(stored.preset).not.toBeNull();
    expect(stored.preset.auto_opt_run_id).toBe('run-done');
    expect(stored.preset.name).toContain('AutoOpt');
    expect(stored.tuning).not.toBeNull();
    expect(stored.tuning.source).toBe('optimized');
    expect(stored.tuning.auto_opt_run_id).toBe('run-done');
    expect(stored.tuning.recipe_options.llamacpp_args).toBe('-b 512 -ub 256 -ctk q8_0 -ctv q8_0');
    expect(stored.tuning.sampling.min_p).toBe(0.05);
  });

  test('"Try now without saving" loads with the recommended runtime options only', async ({ page }) => {
    let loadBody: Record<string, unknown> | null = null;
    await openCompletedRunDetail(page);
    await page.route('**/api/v1/load', async route => {
      loadBody = route.request().postDataJSON();
      await route.fulfill({ json: { status: 'ok' } });
    });

    await page.locator('[data-autoopt-try-now]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('nothing saved');

    expect(loadBody).not.toBeNull();
    expect(loadBody!.model_name).toBe(CHAT_MODEL);
    expect(loadBody!.ctx_size).toBe(8192);
    expect(loadBody!.llamacpp_args).toBe('-b 512 -ub 256 -ctk q8_0 -ctv q8_0');
    expect(loadBody!.llamacpp_backend).toBe('vulkan');
    expect(loadBody!.mmproj_enabled).toBe(false);
    expect(loadBody!.save_options).toBe(false);

    const stored = await page.evaluate(() => {
      for (const key of Object.keys(localStorage)) {
        if (key.includes('user_presets')) {
          const presets = JSON.parse(localStorage.getItem(key) || '[]');
          if (presets.some((p: any) => p.auto_opt_run_id === 'run-done')) return 'preset-created';
        }
      }
      return 'nothing-saved';
    });
    expect(stored).toBe('nothing-saved');
  });

  test('"Use this instead" swaps the CTA target to the alternative', async ({ page }) => {
    let loadBody: Record<string, unknown> | null = null;
    await openCompletedRunDetail(page);
    await page.route('**/api/v1/load', async route => {
      loadBody = route.request().postDataJSON();
      await route.fulfill({ json: { status: 'ok' } });
    });

    await page.locator('[data-autoopt-use-alternative="CPU fallback"]').click();
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('-b 256 -ub 128');
    await page.locator('[data-autoopt-try-now]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('nothing saved');
    expect(loadBody!.ctx_size).toBe(16384);
    expect(loadBody!.llamacpp_backend).toBe('cpu');
  });

  test('preset editor links back to the producing run via the AutoOpt chip', async ({ page }) => {
    await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('Created preset');
    await page.locator('[data-autoopt-detail] .slideover__close').click();

    const yourCards = page.locator('[data-recipe-grid="yours"] .recipe-card');
    await expect(yourCards.first()).toBeVisible();
    await yourCards.first().locator('.recipe-card__overlay-btn').click();
    await page.waitForSelector('.slideover.is-open');

    await expect(page.locator('[data-preset-autoopt-chip]')).toBeVisible();
    await page.locator('[data-preset-autoopt-chip]').click();
    await expect(page.locator('[data-autoopt-detail]')).toBeVisible();
  });
});
