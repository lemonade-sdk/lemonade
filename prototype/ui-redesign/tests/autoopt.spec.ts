import { test, expect, Page } from '@playwright/test';

const CHAT_MODEL = 'org/chat-model';
const NPU_MODEL = 'org/npu-model';
const REGISTRY_MODEL = 'org/registry-only-model';
const RUNS_KEY = 'lemonade_autoopt_runs_v2::http://127.0.0.1:13305';
const CHAT_CHECKPOINT = 'org/chat-model-GGUF:Q4_K_M';

const MODELS = [
  {
    id: CHAT_MODEL, name: CHAT_MODEL, labels: ['llm'], recipe: 'llamacpp', downloaded: true,
    checkpoint: CHAT_CHECKPOINT, size: 4, max_context_window: 32768,
    metadata: {
      architecture: 'qwen3', block_count: 36, context_length: 32768, expert_count: 0,
      full_attention_interval: 0, swa_layer_count: 0, kv_bytes_per_token: 90112,
      base_model_repo: 'https://huggingface.co/org/base-model',
    },
  },
  { id: NPU_MODEL, name: NPU_MODEL, labels: ['llm'], recipe: 'flm', downloaded: true, max_context_window: 32768 },
  { id: REGISTRY_MODEL, name: REGISTRY_MODEL, labels: ['llm'], recipe: 'llamacpp', downloaded: false, max_context_window: 32768 },
];

const SYSTEM_INFO = {
  'OS Version': 'Linux test',
  'Physical Memory': '64.0 GB',
  devices: {
    amd_gpu: [{ available: true, name: 'Radeon 8060S', family: 'gfx1151', vram_gb: 2.0, virtual_mem_gb: 62.0 }],
    nvidia_gpu: [{ available: false, name: '' }],
  },
  recipes: {
    llamacpp: {
      default_backend: 'vulkan',
      backends: {
        vulkan: { state: 'installed' },
        rocm: { state: 'installed' },
        system: { state: 'installed' },
        cpu: { state: 'installable' },
      },
    },
  },
};

function metricsForKey(measKey: string): { ttft: number; tps: number; vram: number } {
  let m = /^ladder_.+_b(\d+)$/.exec(measKey);
  if (m) { const b = Number(m[1]); return { ttft: b === 2048 ? 70 : (b === 8192 ? 80 : 100), tps: 40, vram: 20 }; }
  m = /^mtp_.+_n(\d+)$/.exec(measKey);
  if (m) { const n = Number(m[1]); return { ttft: 100, tps: n === 2 ? 55 : 40, vram: 20 }; }
  m = /^(.+)_d(\d+)$/.exec(measKey);
  if (m) {
    const deep = Number(m[2]) > 0;
    if (m[1] === 'vulkan') return { ttft: deep ? 300 : 90, tps: deep ? 40 : 42, vram: 20 };
    return { ttft: deep ? 380 : 120, tps: deep ? 35 : 37, vram: 21 };
  }
  return { ttft: 100, tps: 30, vram: 20 };
}

function measKeysFromSteps(steps: Array<Record<string, unknown>>): string[] {
  const keys = new Set<string>();
  for (const step of steps) {
    const extract = step.extract as Record<string, string> | undefined;
    if (!extract) continue;
    for (const k of Object.keys(extract)) keys.add(k.replace(/_(ttft|tps|vram)$/, ''));
  }
  return [...keys];
}

function contextForKeys(measKeys: string[]): Record<string, unknown> {
  const ctx: Record<string, unknown> = { inputs: {} };
  for (const mk of measKeys) {
    const met = metricsForKey(mk);
    ctx[`${mk}_ttft`] = met.ttft;
    ctx[`${mk}_tps`] = met.tps;
    ctx[`${mk}_vram`] = met.vram;
  }
  return ctx;
}

interface MockCounts {
  load: Array<Record<string, unknown>>;
  unload: Array<Record<string, unknown>>;
  chat: Array<Record<string, unknown>>;
  jobs: Array<Record<string, unknown>>;
  interrupts: string[];
  deletes: string[];
}

interface MockOptions {
  loadedModels?: Array<Record<string, unknown>>;
  jobRunningPolls?: number;
  jobFails?: boolean;
  failPrimaryLoads?: boolean;
  loadHandler?: (body: Record<string, unknown>) => { status?: number; json?: unknown } | null;
}

async function mockServer(page: Page, options: MockOptions = {}): Promise<MockCounts> {
  const counts: MockCounts = { load: [], unload: [], chat: [], jobs: [], interrupts: [], deletes: [] };
  const loaded = options.loadedModels || [];
  const runningPolls = options.jobRunningPolls ?? 0;

  const jobs = new Map<string, { steps: Array<Record<string, unknown>>; polls: number }>();
  let jobSeq = 0;

  await page.route('**/api/v1/health**', route => route.fulfill({
    json: { status: 'ok', version: 'test', all_models_loaded: loaded },
  }));
  await page.route('**/api/v1/models**', route => route.fulfill({ json: { data: MODELS } }));
  await page.route('**/api/v1/models/*', route => {
    const id = decodeURIComponent(route.request().url().split('/').pop() || '').split('?')[0];
    const model = MODELS.find(m => m.id === id);
    if (!model) return route.fulfill({ status: 404, json: { error: 'unknown model' } });
    return route.fulfill({ json: model });
  });
  await page.route('**/api/v1/system-info**', route => route.fulfill({ json: SYSTEM_INFO }));
  await page.route('**/api/v1/system-stats', route => route.fulfill({
    json: { cpu_percent: 10, memory_gb: 40, gpu_percent: 80, vram_gb: 20, npu_percent: 0 },
  }));
  await page.route('**/api/v1/unload', route => {
    counts.unload.push(route.request().postDataJSON() || {});
    return route.fulfill({ json: { status: 'ok' } });
  });
  await page.route('**/api/v1/load', async route => {
    const body = route.request().postDataJSON() as Record<string, unknown>;
    counts.load.push(body);
    const override = options.loadHandler ? options.loadHandler(body) : null;
    if (override) return route.fulfill({ status: override.status ?? 200, json: override.json ?? { status: 'ok' } });
    return route.fulfill({ json: { status: 'ok' } });
  });
  await page.route('**/api/v1/chat/completions', route => {
    counts.chat.push(route.request().postDataJSON() || {});
    return route.fulfill({ json: { choices: [{ message: { role: 'assistant', content: 'ok' } }] } });
  });

  await page.route('**/api/v1/jobs', async route => {
    if (route.request().method() !== 'POST') return route.fulfill({ json: { jobs: [] } });
    const body = route.request().postDataJSON() as Record<string, unknown>;
    counts.jobs.push(body);
    const steps = ((body.definition as Record<string, unknown>)?.steps || []) as Array<Record<string, unknown>>;
    const id = `job-test-${++jobSeq}`;
    jobs.set(id, { steps, polls: 0 });
    return route.fulfill({ status: 202, json: { id } });
  });
  await page.route('**/api/v1/jobs/*/interrupt', route => {
    counts.interrupts.push(decodeURIComponent(route.request().url().split('/').slice(-2)[0]));
    return route.fulfill({ json: { status: 'ok' } });
  });
  await page.route('**/api/v1/jobs/*/pause', route => route.fulfill({ json: { status: 'ok' } }));
  await page.route('**/api/v1/jobs/*/resume', route => route.fulfill({ json: { status: 'ok' } }));
  await page.route('**/api/v1/jobs/*', async route => {
    const id = decodeURIComponent(route.request().url().split('/').pop() || '').split('?')[0];
    if (route.request().method() === 'DELETE') {
      counts.deletes.push(id);
      return route.fulfill({ json: { status: 'ok' } });
    }
    const entry = jobs.get(id) || { steps: [], polls: 0 };
    if (!jobs.has(id)) jobs.set(id, entry);
    entry.polls += 1;

    const measKeys = entry.steps.length ? measKeysFromSteps(entry.steps) : ['vulkan_d0', 'rocm_d0'];
    const stepIds: string[] = entry.steps.length
      ? entry.steps.map(s => String(s.id))
      : ['load_vulkan_d0', 'chat_vulkan_d0'];

    const running = entry.polls <= runningPolls;
    if (running) {
      return route.fulfill({
        json: {
          id, name: 'autoopt', status: 'running', context: { inputs: {} },
          cursor: stepIds[0],
          steps: stepIds.map((sid, i) => ({ id: sid, op: 'load', status: i === 0 ? 'running' : 'pending' })),
        },
      });
    }
    if (options.jobFails) {
      return route.fulfill({
        json: {
          id, name: 'autoopt', status: 'failed', context: { inputs: {} },
          steps: stepIds.map(sid => ({ id: sid, op: 'load', status: 'failed', error: 'CUDA out of memory' })),
        },
      });
    }
    const context = contextForKeys(measKeys);
    if (options.failPrimaryLoads) {
      for (const mk of measKeys) {
        if (/_d\d+$/.test(mk) && !mk.endsWith('_lo')) {
          delete context[`${mk}_ttft`];
          delete context[`${mk}_tps`];
          delete context[`${mk}_vram`];
        }
      }
    }
    return route.fulfill({
      json: {
        id, name: 'autoopt', status: 'completed', context,
        steps: stepIds.map(sid => ({ id: sid, op: 'load', status: 'completed', duration_ms: 100 })),
      },
    });
  });

  await page.route('https://huggingface.co/org/base-model/resolve/main/generation_config.json',
    route => route.fulfill({ json: { temperature: 0.7, top_p: 0.8, top_k: 20 } }));
  await page.route('https://huggingface.co/org/base-model/resolve/main/config.json',
    route => route.fulfill({ status: 404, body: 'Not Found' }));
  await page.route('https://huggingface.co/api/models/**', route => route.fulfill({
    json: { cardData: { base_model: 'org/base-model' } },
  }));

  return counts;
}

const COMPLETED_RUN = {
  id: 'ao-20260712-101500-cafe',
  model: CHAT_MODEL,
  checkpoint: CHAT_CHECKPOINT,
  budget: 'standard',
  answers: { parallel: { mode: 'single' }, kv_cache_quant: 'q8_0', ram_headroom: 'normal', allow_network: true },
  allow_unload: true,
  status: 'completed',
  created_at: '2026-07-12T10:00:00Z',
  finished_at: '2026-07-12T10:15:00Z',
  summary: 'vulkan · ctx 32768 · -ctk q8_0 -ctv q8_0 --spec-default -b 2048 -ub 2048',
  lemonade_version: 'test',
  job_id: 'job-test-old',
  stages: [
    { name: 'snapshot', status: 'completed', duration_ms: 12 },
    { name: 'model_facts', status: 'completed', duration_ms: 8 },
    { name: 'hf_metadata', status: 'completed', duration_ms: 420 },
    { name: 'fit_estimate', status: 'completed', duration_ms: 3 },
    { name: 'bench_job', status: 'completed', duration_ms: 431000 },
    { name: 'synthesize', status: 'completed', duration_ms: 2 },
  ],
  measurements: {
    fit: [
      { backend: 'vulkan', fits_fully: true, fitted_ctx: 0, fitted_ngl: -1, fitted_ncmoe: 0, weights_mib: 4096, kv_mib: 2816, compute_mib: 512, total_mib: 7424, available_mib: 58982, degraded: false, ok: true },
    ],
    bench: [
      { backend: 'vulkan', label: 'vulkan · d0', ctx_size: 2048, llamacpp_args: '-ctk q8_0 -ctv q8_0', params: { d: 0 }, ttft_ms: 90, tps: 42, vram_gb: 20, ok: true },
      { backend: 'rocm', label: 'rocm · d0', ctx_size: 2048, llamacpp_args: '-ctk q8_0 -ctv q8_0', params: { d: 0 }, ttft_ms: 120, tps: 37, vram_gb: 21, ok: true },
      { backend: 'vulkan', label: 'vulkan · b512', ctx_size: 2048, llamacpp_args: '-b 512 -ub 512', params: { ladder: true, b: 512, ub: 512, d: 0 }, ttft_ms: 100, tps: 40, vram_gb: 20, ok: true },
      { backend: 'vulkan', label: 'vulkan · b2048', ctx_size: 2048, llamacpp_args: '-b 2048 -ub 2048', params: { ladder: true, b: 2048, ub: 2048, d: 0 }, ttft_ms: 70, tps: 40, vram_gb: 20, ok: true },
    ],
  },
  result: {
    primary: {
      label: 'Recommended',
      llamacpp_backend: 'vulkan',
      ctx_size: 32768,
      llamacpp_args: '-ctk q8_0 -ctv q8_0 --spec-default -b 2048 -ub 2048',
      rationale: ['vulkan chosen over rocm: best measured throughput/latency balance'],
      expected: { ttft_ms: 90, tps: 42, vram_gb: 20 },
    },
    alternatives: [
      {
        label: 'Maximum quality',
        llamacpp_backend: 'vulkan',
        ctx_size: 16384,
        llamacpp_args: '--spec-default -b 2048 -ub 2048',
        rationale: ['Unquantized f16 KV cache'],
        tradeoff: 'smaller context window',
      },
    ],
    sampling_defaults: { temperature: 0.7, top_p: 0.9, top_k: 40, min_p: 0.05, source: 'hf:org/base-model/generation_config.json' },
  },
};

const SYNTH_INPUTS = {
  hardware: { gpus: [{ vendor: 'amd', name: 'x', family: 'gfx1151', vram_gb: 62 }], has_igpu: true, ram_is_vram: true, host_ram_gb: 64, installed_backends: ['vulkan', 'rocm'], os: 'linux' },
  facts: { architecture: 'qwen3', block_count: 36, expert_count: 0, full_attention_interval: 0, swa_layer_count: 0, n_ctx_train: 32768, kv_bytes_per_token: 90112, weights_mib: 4096, is_moe: false, is_hybrid_or_recurrent: false, has_mtp: false, base_model_repo: '', checkpoint: CHAT_CHECKPOINT, metadata_present: true },
  fits: [{ backend: 'vulkan', fits_fully: true, fitted_ctx: 0, fitted_ngl: -1, fitted_ncmoe: 0, weights_mib: 4096, kv_mib: 2816, compute_mib: 512, total_mib: 7424, available_mib: 58982, degraded: false, ok: true }],
  plan: [
    { label: 'Benchmarking vulkan at depth 0', backend: 'vulkan', ctx_size: 32768, llamacpp_args: '', params: { d: 0 }, ttft_key: 'vulkan_d0_ttft', tps_key: 'vulkan_d0_tps', vram_key: 'vulkan_d0_vram' },
    { label: 'Benchmarking rocm at depth 0', backend: 'rocm', ctx_size: 2048, llamacpp_args: '', params: { d: 0 }, ttft_key: 'rocm_d0_ttft', tps_key: 'rocm_d0_tps', vram_key: 'rocm_d0_vram' },
  ],
  step_labels: { load_vulkan_d0: 'Benchmarking vulkan at depth 0', chat_vulkan_d0: 'Benchmarking vulkan at depth 0' },
};

async function seedRuns(page: Page, runs: Array<Record<string, unknown>>, key = RUNS_KEY) {
  await page.addInitScript(([k, payload]) => {
    localStorage.setItem(k as string, JSON.stringify({ version: 2, runs: payload }));
  }, [key, runs] as const);
}

async function openPresets(page: Page) {
  await page.goto('/');
  await page.waitForSelector('.titlebar__nav');
  await page.locator('.titlebar__nav').getByText('Presets').click();
  await page.waitForSelector('.recipes');
}

async function openWizard(page: Page) {
  await openPresets(page);
  await page.locator('[data-autoopt-run-optimizer]').click();
  await page.waitForSelector('[data-autoopt-wizard]');
}

async function walkToBudget(page: Page, model = CHAT_MODEL) {
  await page.locator('[data-autoopt-model-select]').selectOption(model);
  await page.locator('[data-autoopt-next]').click();
  await page.locator('[data-autoopt-next]').click();
  await page.locator('[data-autoopt-next]').click();
  await page.locator('[data-autoopt-next]').click();
  await expect(page.locator('[data-autoopt-step="budget"]')).toBeVisible();
}

test.describe('AutoOpt wizard + server-side bench job', () => {

  test('full Benchmark flow — posts a recipe, polls the job, synthesizes the result', async ({ page }) => {
    const counts = await mockServer(page, {
      loadedModels: [{ model_name: CHAT_MODEL, type: 'llm', recipe: 'llamacpp', device: 'gpu', checkpoint: '', backend_url: '', pid: 1, last_use: Date.now() }],
    });

    await openWizard(page);
    await expect(page.locator('[data-autoopt-model-select]')).toHaveValue(CHAT_MODEL, { timeout: 10000 });
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-option="kv:q8_0"]')).toHaveAttribute('aria-pressed', 'true');
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-option="ram:minimal"]').click();
    await page.locator('[data-autoopt-next]').click();

    await expect(page.locator('[data-autoopt-step="budget"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-step="vision"]')).toHaveCount(0);
    await page.locator('[data-autoopt-option="budget:standard"]').click();

    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-start]')).toBeDisabled();
    await page.locator('[data-autoopt-back]').click();
    await page.locator('[data-autoopt-consent]').check();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-start]').click();

    await expect(page.locator('[data-autoopt-step="running"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-close-note]')).toContainText('keeps running on the server');
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--completed')).toHaveCount(6, { timeout: 20000 });
    await page.locator('[data-autoopt-wizard] .slideover__close').click();

    const run = page.locator('[data-autoopt-run]');
    await expect(run.locator('.autoopt-status-chip--completed')).toBeVisible();
    await expect(page.locator('[data-autoopt-announcement]')).toContainText(`AutoOpt run for ${CHAT_MODEL} completed.`);

    expect(counts.jobs).toHaveLength(1);
    const recipe = counts.jobs[0] as { inputs: Record<string, unknown>; definition: { steps: Array<Record<string, unknown>> } };
    const steps = recipe.definition.steps;
    const ops = new Set(steps.map(s => s.op));
    expect(ops.has('load')).toBe(true);
    expect(ops.has('chat')).toBe(true);
    expect(ops.has('unload')).toBe(true);

    const loadBackends = new Set(steps.filter(s => s.op === 'load').map(s => (s.params as any)?.llamacpp_backend));
    expect(loadBackends.has('vulkan')).toBe(true);
    expect(loadBackends.has('rocm')).toBe(true);

    const extractKeys = steps.flatMap(s => Object.keys((s.extract as Record<string, string>) || {}));
    expect(extractKeys.some(k => k.endsWith('_ttft'))).toBe(true);
    expect(extractKeys.some(k => k.endsWith('_tps'))).toBe(true);

    expect(extractKeys).toEqual(expect.arrayContaining([
      'vulkan_d0_tps', 'vulkan_d30000_tps',
      'ladder_vulkan_b512_ttft', 'ladder_vulkan_b2048_ttft', 'ladder_vulkan_b8192_ttft',
      'ladder_rocm_b512_ttft',
    ]));

    const fallbackLoad = steps.find(s => s.op === 'load' && typeof s.on_fail === 'string' && String(s.on_fail).startsWith('loadlo_'));
    expect(fallbackLoad).toBeTruthy();
    expect(steps.some(s => String(s.id).startsWith('loadlo_'))).toBe(true);

    expect(counts.load).toHaveLength(0);
    expect(counts.chat).toHaveLength(0);

    await page.locator('[data-autoopt-inspect]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    const args = page.locator('[data-autoopt-rec-args]');
    await expect(args).toContainText('-ctk q8_0 -ctv q8_0');
    await expect(args).toContainText('--cache-ram 2048 -ctxcp 8');
    await expect(args).toContainText('-b 2048 -ub 2048');
    await expect(page.locator('.autoopt-rec-card__chips')).toContainText('vulkan');
    await expect(page.locator('[data-autoopt-bench-duel] thead')).toContainText('TTFT ms');
    await expect(page.locator('[data-autoopt-bench-ladder] thead')).toContainText('TTFT ms');
  });

  test('reload re-attaches to an in-flight server job and finishes it (review #6a)', async ({ page }) => {
    await mockServer(page, { jobRunningPolls: 1 });
    await seedRuns(page, [{
      ...COMPLETED_RUN,
      id: 'ao-reattach',
      status: 'running',
      finished_at: undefined,
      summary: undefined,
      result: undefined,
      job_id: 'job-reattach-1',
      synth_inputs: { ...SYNTH_INPUTS, facts: { ...SYNTH_INPUTS.facts }, plan: SYNTH_INPUTS.plan },
      answers: { parallel: { mode: 'single' }, kv_cache_quant: 'none', ram_headroom: 'normal', allow_network: true },
      stages: [
        { name: 'snapshot', status: 'completed' },
        { name: 'model_facts', status: 'completed' },
        { name: 'hf_metadata', status: 'completed' },
        { name: 'fit_estimate', status: 'completed' },
        { name: 'bench_job', status: 'running' },
        { name: 'synthesize', status: 'pending' },
      ],
      measurements: { fit: SYNTH_INPUTS.fits, bench: [] },
    }]);

    await openPresets(page);

    const run = page.locator('[data-autoopt-run="ao-reattach"]');
    await expect(run.locator('.autoopt-status-chip--running')).toBeVisible();

    await expect(run.locator('.autoopt-status-chip--completed')).toBeVisible({ timeout: 15000 });
    await page.locator('[data-autoopt-inspect="ao-reattach"]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('.autoopt-rec-card__chips')).toContainText('vulkan');
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('--spec-default');
  });

  test('Fast Scan skips the bench job and never touches the model slot', async ({ page }) => {
    const counts = await mockServer(page, {
      loadedModels: [{ model_name: CHAT_MODEL, type: 'llm', recipe: 'llamacpp', device: 'gpu', checkpoint: '', backend_url: '', pid: 1, last_use: Date.now() }],
    });

    await openWizard(page);
    await expect(page.locator('[data-autoopt-model-select]')).toHaveValue(CHAT_MODEL, { timeout: 10000 });
    await walkToBudget(page);
    await page.locator('[data-autoopt-option="budget:quick"]').click();
    await expect(page.locator('[data-autoopt-consent]')).toHaveCount(0);
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-step="review"]')).toContainText('Not needed for Fast Scan');
    await page.locator('[data-autoopt-start]').click();

    await expect(page.locator('[data-autoopt-step="running"]')).toBeVisible();
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--completed')).toHaveCount(5, { timeout: 15000 });
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--skipped')).toHaveCount(1);

    expect(counts.jobs).toHaveLength(0);
    expect(counts.load).toHaveLength(0);
    expect(counts.unload).toHaveLength(0);
  });

  test('a late system-info reply must not clobber an explicit RAM-headroom choice', async ({ page }) => {
    await mockServer(page);
    await page.unroute('**/api/v1/system-info**');
    await page.route('**/api/v1/system-info**', async route => {
      await new Promise(resolve => setTimeout(resolve, 2500));
      await route.fulfill({ json: SYSTEM_INFO });
    });

    await openWizard(page);
    await page.locator('[data-autoopt-model-select]').selectOption(CHAT_MODEL);
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-step="ram"]')).toBeVisible();
    await page.locator('[data-autoopt-option="ram:minimal"]').click();
    await page.waitForTimeout(3000);
    await expect(page.locator('[data-autoopt-option="ram:minimal"]')).toHaveAttribute('aria-pressed', 'true');

    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-option="budget:quick"]').click();
    await page.locator('[data-autoopt-next]').click();
    await expect(page.locator('[data-autoopt-step="review"]')).toContainText('Minimal');
    await page.locator('[data-autoopt-start]').click();
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--completed')).toHaveCount(5, { timeout: 15000 });

    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    await page.locator('[data-autoopt-inspect]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('--cache-ram 2048 -ctxcp 8');
  });

  async function runFastScan(page: Page) {
    await openWizard(page);
    await walkToBudget(page);
    await page.locator('[data-autoopt-option="budget:quick"]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-start]').click();
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--skipped')).toHaveCount(1, { timeout: 15000 });
  }

  test('a model without published sampling defaults completes hf_metadata with a neutral note', async ({ page }) => {
    await mockServer(page);
    await page.unroute('https://huggingface.co/org/base-model/resolve/main/generation_config.json');
    await page.route('https://huggingface.co/org/base-model/resolve/main/generation_config.json',
      route => route.fulfill({ status: 404, body: 'Not Found' }));

    await runFastScan(page);
    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--failed')).toHaveCount(0);
    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    await page.locator('[data-autoopt-inspect]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('.autoopt-stage__note')).toContainText('no sampling defaults published');
  });

  test('hf_metadata falls back to generation defaults inside config.json', async ({ page }) => {
    await mockServer(page);
    await page.unroute('https://huggingface.co/org/base-model/resolve/main/generation_config.json');
    await page.route('https://huggingface.co/org/base-model/resolve/main/generation_config.json',
      route => route.fulfill({ status: 404, body: 'Not Found' }));
    await page.unroute('https://huggingface.co/org/base-model/resolve/main/config.json');
    await page.route('https://huggingface.co/org/base-model/resolve/main/config.json',
      route => route.fulfill({ json: { architectures: ['Qwen3ForCausalLM'], generation_config: { temperature: 0.6, top_k: 20 } } }));

    await runFastScan(page);
    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    await page.locator('[data-autoopt-inspect]').click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('.autoopt-rec-card__chips')).toContainText('temp 0.6');
  });

  test('model picker only offers downloaded llama.cpp models and explains the scope', async ({ page }) => {
    await mockServer(page);
    await openWizard(page);

    await expect(page.locator('[data-autoopt-model-note]')).toContainText('AutoOpt currently supports downloaded llama.cpp models');
    const select = page.locator('[data-autoopt-model-select]');
    await expect(select.locator(`option[value="${CHAT_MODEL}"]`)).toHaveCount(1);
    await expect(select.locator(`option[value="${NPU_MODEL}"]`)).toHaveCount(0);
    await expect(select.locator(`option[value="${REGISTRY_MODEL}"]`)).toHaveCount(0);
  });

  test('cancel interrupts the server job and marks the run cancelled', async ({ page }) => {
    const counts = await mockServer(page, { jobRunningPolls: 1000 });

    await openWizard(page);
    await walkToBudget(page);
    await page.locator('[data-autoopt-option="budget:standard"]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-start]').click();

    await expect(page.locator('[data-autoopt-step="running"]')).toBeVisible();
    await expect.poll(() => counts.jobs.length, { timeout: 15000 }).toBe(1);
    await page.locator('[data-autoopt-cancel-run]').click();

    await expect.poll(() => counts.interrupts.length, { timeout: 15000 }).toBeGreaterThan(0);
    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    await expect(page.locator('[data-autoopt-run] .autoopt-status-chip--cancelled')).toBeVisible({ timeout: 10000 });
  });

  test('when the primary ctx fails to load, the recommendation reflects the fallback ctx (review #2)', async ({ page }) => {
    await mockServer(page, { failPrimaryLoads: true });

    await openWizard(page);
    await walkToBudget(page);
    await page.locator('[data-autoopt-option="budget:standard"]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-start]').click();

    await expect(page.locator('[data-autoopt-stage-list] .autoopt-stage--completed')).toHaveCount(6, { timeout: 20000 });
    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    await page.locator('[data-autoopt-inspect]').click();
    await page.waitForSelector('[data-autoopt-detail]');

    await expect(page.locator('.autoopt-rec-card__chips')).toContainText('ctx 2,048');
    await expect(page.locator('.autoopt-rec-card__chips')).not.toContainText('ctx 32,768');
    await expect(page.locator('.autoopt-rec-card__rationale')).toContainText('largest context that actually loaded');
  });

  test('a server job that fails surfaces the error in rail and detail', async ({ page }) => {
    await mockServer(page, { jobFails: true });

    await openWizard(page);
    await walkToBudget(page);
    await page.locator('[data-autoopt-option="budget:standard"]').click();
    await page.locator('[data-autoopt-next]').click();
    await page.locator('[data-autoopt-start]').click();

    await expect(page.locator('[data-autoopt-step="running"] .preset-error')).toContainText('CUDA out of memory', { timeout: 20000 });
    await page.locator('[data-autoopt-wizard] .slideover__close').click();
    const run = page.locator('[data-autoopt-run]');
    await expect(run.locator('.autoopt-status-chip--failed')).toContainText('CUDA out of memory');
  });

  test('the rail fires no server calls before a run is started', async ({ page }) => {
    const counts = await mockServer(page);
    await openPresets(page);
    await expect(page.locator('[data-autoopt-run-optimizer]')).toBeEnabled();
    await page.waitForTimeout(500);
    expect(counts.jobs).toHaveLength(0);
    expect(counts.load).toHaveLength(0);
  });
});

test.describe('AutoOpt rail persistence + server scoping', () => {

  test('a client-only run interrupted by reload is failed as interrupted', async ({ page }) => {
    await mockServer(page);
    await seedRuns(page, [{ ...COMPLETED_RUN, id: 'ao-dead', status: 'running', job_id: undefined, synth_inputs: undefined, finished_at: undefined, result: undefined, summary: undefined }]);

    await openPresets(page);
    const run = page.locator('[data-autoopt-run="ao-dead"]');
    await expect(run).toBeVisible();
    await expect(run.locator('.autoopt-status-chip--failed')).toContainText('interrupted');
  });

  test('completed runs survive a reload with their result intact', async ({ page }) => {
    await mockServer(page);
    await seedRuns(page, [COMPLETED_RUN]);

    await openPresets(page);
    await expect(page.locator(`[data-autoopt-run="${COMPLETED_RUN.id}"] .autoopt-status-chip--completed`)).toBeVisible();
    await page.locator(`[data-autoopt-inspect="${COMPLETED_RUN.id}"]`).click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('-b 2048 -ub 2048');
    await expect(page.locator('[data-autoopt-bench-duel] tbody tr')).toHaveCount(2);
    await expect(page.locator('[data-autoopt-bench-ladder] tbody tr')).toHaveCount(2);
  });

  test('runs measured on a different server do not appear here', async ({ page }) => {
    await mockServer(page);
    await seedRuns(page, [{ ...COMPLETED_RUN, id: 'ao-other-server' }], 'lemonade_autoopt_runs_v2::http://other-server:9999');

    await openPresets(page);
    await expect(page.locator('[data-autoopt-empty]')).toBeVisible();
    await expect(page.locator('[data-autoopt-run="ao-other-server"]')).toHaveCount(0);
  });

  test('delete removes a run permanently; a failed persist rolls the row and selection back', async ({ page }) => {
    const otherRun = { ...COMPLETED_RUN, id: 'ao-20260713-110000-beef', created_at: '2026-07-13T11:00:00Z' };
    await mockServer(page);
    await seedRuns(page, [otherRun, COMPLETED_RUN]);

    await openPresets(page);
    const run = page.locator(`[data-autoopt-run="${COMPLETED_RUN.id}"]`);
    await expect(run).toBeVisible();
    await run.locator('.auto-run-card__main').click();
    await expect(run.locator('.auto-run-card__main')).toHaveAttribute('aria-pressed', 'true');
    await page.evaluate((key) => {
      const original = localStorage.setItem.bind(localStorage);
      let armed = true;
      localStorage.setItem = (k: string, v: string) => {
        if (k === key && armed) { armed = false; throw new Error('storage quota exceeded'); }
        return original(k, v);
      };
    }, RUNS_KEY);

    await page.locator(`[data-autoopt-delete="${COMPLETED_RUN.id}"]`).click();
    await expect(run).toBeVisible();
    await expect(page.locator('[data-autoopt-rail-error]')).toContainText('storage quota exceeded');
    await expect(run.locator('.auto-run-card__main')).toHaveAttribute('aria-pressed', 'true');

    await page.locator(`[data-autoopt-delete="${COMPLETED_RUN.id}"]`).click();
    await expect(run).toHaveCount(0);
    const persisted = await page.evaluate((key) => {
      const parsed = JSON.parse(localStorage.getItem(key) || '{}');
      return Array.isArray(parsed.runs) ? parsed.runs.map((r: any) => r.id) : [];
    }, RUNS_KEY);
    expect(persisted).toEqual([otherRun.id]);
  });
});

test.describe('AutoOpt run detail actions', () => {

  async function openCompletedRunDetail(page: Page, options: MockOptions = {}): Promise<MockCounts> {
    const counts = await mockServer(page, options);
    await seedRuns(page, [COMPLETED_RUN]);
    await openPresets(page);
    await expect(page.locator(`[data-autoopt-run="${COMPLETED_RUN.id}"]`)).toBeVisible();
    await page.locator(`[data-autoopt-inspect="${COMPLETED_RUN.id}"]`).click();
    await page.waitForSelector('[data-autoopt-detail]');
    await expect(page.locator('[data-autoopt-recommendation]')).toBeVisible();
    return counts;
  }

  test('"Create preset" writes an optimized preset and model tuning without loading', async ({ page }) => {
    const counts = await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('selected it for future loads');
    expect(counts.load).toHaveLength(0);

    const stored = await page.evaluate((model) => {
      let preset: any = null;
      let tuning: any = null;
      for (const key of Object.keys(localStorage)) {
        if (key.includes('user_presets')) {
          const presets = JSON.parse(localStorage.getItem(key) || '[]');
          preset = presets.find((p: any) => p.auto_opt_run_id) || preset;
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

    expect(stored.preset?.auto_opt_run_id).toBe(COMPLETED_RUN.id);
    expect(stored.preset.name).toContain('AutoOpt');
    expect(stored.tuning?.source).toBe('optimized');
    expect(stored.tuning.recipe_options.llamacpp_args).toBe('-ctk q8_0 -ctv q8_0 --spec-default -b 2048 -ub 2048');
    expect(stored.tuning.sampling.min_p).toBe(0.05);
  });

  test('"Create preset & apply now" also loads the model', async ({ page }) => {
    const counts = await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-apply]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('applied it to');
    expect(counts.load).toHaveLength(1);
    expect(counts.load[0].model_name).toBe(CHAT_MODEL);
  });

  test('"Create preset & apply now" keeps the created preset when the load fails', async ({ page }) => {
    await openCompletedRunDetail(page);
    await page.unroute('**/api/v1/load');
    await page.route('**/api/v1/load', route => route.fulfill({ status: 500, json: { error: { message: 'llama-server failed to start' } } }));

    await page.locator('[data-autoopt-create-apply]').click();
    const error = page.locator('[data-autoopt-detail] .preset-error');
    await expect(error).toContainText('was created and selected, but loading');
    await expect(error).toContainText('llama-server failed to start');

    const stored = await page.evaluate(() => {
      for (const key of Object.keys(localStorage)) {
        if (!key.includes('user_presets')) continue;
        const presets = JSON.parse(localStorage.getItem(key) || '[]');
        const preset = presets.find((p: any) => p.auto_opt_run_id);
        if (preset) return preset;
      }
      return null;
    });
    expect(stored?.name).toContain('AutoOpt');
  });

  test('"Try now without saving" loads a not-loaded model with the exact recommended options', async ({ page }) => {
    const counts = await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-try-now]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('nothing saved');

    expect(counts.load).toHaveLength(1);
    const body = counts.load[0];
    expect(body.model_name).toBe(CHAT_MODEL);
    expect(body.ctx_size).toBe(32768);
    expect(body.llamacpp_args).toBe('-ctk q8_0 -ctv q8_0 --spec-default -b 2048 -ub 2048');
    expect(body.llamacpp_backend).toBe('vulkan');
    expect(body.save_options).toBe(false);
    const saved = await page.evaluate(() => {
      for (const key of Object.keys(localStorage)) {
        if (key.includes('user_presets')) {
          const presets = JSON.parse(localStorage.getItem(key) || '[]');
          if (presets.some((p: any) => p.auto_opt_run_id)) return 'preset-created';
        }
      }
      return 'nothing-saved';
    });
    expect(saved).toBe('nothing-saved');
  });

  test('"Try now without saving" RELOADS a model that is already loaded (review #2)', async ({ page }) => {
    const counts = await openCompletedRunDetail(page, {
      loadedModels: [{ model_name: CHAT_MODEL, type: 'llm', recipe: 'llamacpp', device: 'gpu', checkpoint: '', backend_url: '', pid: 1, last_use: Date.now() }],
    });
    await page.locator('[data-autoopt-try-now]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('nothing saved');

    expect(counts.unload.some(b => b.model_name === CHAT_MODEL)).toBe(true);
    expect(counts.load).toHaveLength(1);
    expect(counts.load[0].save_options).toBe(false);
    expect(counts.load[0].llamacpp_args).toBe('-ctk q8_0 -ctv q8_0 --spec-default -b 2048 -ub 2048');
  });

  test('"Use this instead" swaps the CTA target to the alternative', async ({ page }) => {
    const counts = await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-use-alternative="Maximum quality"]').click();
    await expect(page.locator('[data-autoopt-rec-args]')).toContainText('--spec-default -b 2048 -ub 2048');
    await page.locator('[data-autoopt-try-now]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('nothing saved');
    expect(counts.load[0].ctx_size).toBe(16384);
  });

  test('a run whose checkpoint no longer matches the model is refused on apply (review #5)', async ({ page }) => {
    await mockServer(page);
    await seedRuns(page, [{ ...COMPLETED_RUN, checkpoint: 'org/chat-model-GGUF:OLD_QUANT' }]);
    await openPresets(page);
    await page.locator(`[data-autoopt-inspect="${COMPLETED_RUN.id}"]`).click();
    await page.waitForSelector('[data-autoopt-detail]');

    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail] .preset-error')).toContainText('different build');
    const saved = await page.evaluate(() => {
      for (const key of Object.keys(localStorage)) {
        if (key.includes('user_presets')) {
          const presets = JSON.parse(localStorage.getItem(key) || '[]');
          if (presets.some((p: any) => p.auto_opt_run_id)) return true;
        }
      }
      return false;
    });
    expect(saved).toBe(false);
  });

  test('a created preset is discoverable: sorted first, flashed, and named after its model', async ({ page }) => {
    await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('Created preset');
    await page.locator('[data-autoopt-detail] .slideover__close').click();

    const zoneTitles = await page.locator('.recipes__body .zone__title').allTextContents();
    expect(zoneTitles.indexOf('Your presets')).toBeLessThan(zoneTitles.indexOf('Bundled starters'));

    const card = page.locator('[data-recipe-grid="yours"] .recipe-card').first();
    await expect(card).toHaveClass(/recipe-card--flash/);
    await expect(card.locator('[data-preset-linked-models]')).toContainText(`Optimized for ${CHAT_MODEL}`);
  });

  test('preset editor links back to the producing run via the AutoOpt chip', async ({ page }) => {
    await openCompletedRunDetail(page);
    await page.locator('[data-autoopt-create-preset]').click();
    await expect(page.locator('[data-autoopt-detail-notice]')).toContainText('Created preset');
    await page.locator('[data-autoopt-detail] .slideover__close').click();

    const yourCards = page.locator('[data-recipe-grid="yours"] .recipe-card');
    await yourCards.first().locator('.recipe-card__overlay-btn').click();
    await page.waitForSelector('.slideover.is-open');
    await expect(page.locator('[data-preset-editor-linked]')).toContainText(`Optimized for ${CHAT_MODEL}`);
    await page.locator('[data-preset-autoopt-chip]').click();
    await expect(page.locator('[data-autoopt-detail]')).toBeVisible();
  });
});
