const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');
const hookPath = path.join(appRoot, 'src', 'hooks', 'useDashboardData.ts');

let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  module.exports = {
    tests: [{
      name: 'Dashboard completed-request throughput fallback',
      run: () => ({ skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" }),
    }],
  };
  return;
}

const model = (recipe = 'flm') => ({
  model_name: 'test-model',
  recipe,
  type: 'llm',
  device: recipe === 'flm' ? 'npu' : 'gpu',
});

const health = (recipe = 'flm') => ({
  version: 'test',
  model_loaded: 'test-model',
  all_models_loaded: [model(recipe)],
  websocket_port: 0,
});

const stats = (requestCount, tokensPerSecond = 33.3, overrides = {}) => ({
  input_tokens: 233,
  output_tokens: 40,
  time_to_first_token: 0.842,
  tokens_per_second: tokensPerSecond,
  decode_token_times: [],
  prompt_tokens: 233,
  request_count_total: requestCount,
  ...overrides,
});

const systemStats = {
  cpu_percent: 14,
  memory_gb: 7.8,
  gpu_percent: 8,
  vram_gb: null,
  npu_percent: 85,
};

const slot = ({ decoded, prompted, promptPerSecond = 0 }) => ({
  id: 0,
  n_ctx: 1024,
  n_decoded: decoded,
  n_prompt_tokens: prompted,
  n_prompt_tokens_processed: prompted,
  state: 0,
  is_processing: true,
  model: 'test-model',
  temperature: 0,
  top_k: 0,
  top_p: 0,
  cache_tokens: [],
  timings: { prompt_per_second: promptPerSecond },
  prompt: '',
  truncated: false,
  stopped_eos: false,
  stopped_word: false,
  stopped_limit: false,
});

function sequence(values) {
  let index = 0;
  return () => {
    const value = values[Math.min(index, values.length - 1)];
    index += 1;
    return Promise.resolve(value);
  };
}

function deferred() {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

async function flush() {
  for (let i = 0; i < 8; i += 1) await Promise.resolve();
}

function createDashboardHarness({
  recipe = 'flm',
  healthValues = [health(recipe)],
  statsValues,
  slots = [],
  slotsValues = [slots],
}) {
  const state = [];
  const refs = [];
  const effects = [];
  const timers = [];
  let stateIndex = 0;
  let refIndex = 0;
  let collectEffects = true;
  let output = null;
  let slotsCalls = 0;
  let now = 1_000_000;

  const fakeReact = {
    useState(initial) {
      const index = stateIndex;
      stateIndex += 1;
      if (!(index in state)) state[index] = typeof initial === 'function' ? initial() : initial;
      return [state[index], (next) => {
        state[index] = typeof next === 'function' ? next(state[index]) : next;
      }];
    },
    useRef(initial) {
      const index = refIndex;
      refIndex += 1;
      if (!(index in refs)) refs[index] = { current: initial };
      return refs[index];
    },
    useCallback(callback) {
      return callback;
    },
    useMemo(factory) {
      return factory();
    },
    useEffect(effect) {
      if (collectEffects) effects.push(effect);
    },
  };

  const api = {
    systemInfoData: {},
    lastConnectionError: '',
    health: sequence(healthValues),
    stats: sequence(statsValues),
    systemStats: () => Promise.resolve(systemStats),
    systemInfo: () => Promise.resolve({}),
    slots: (() => {
      const nextSlotResponse = sequence(slotsValues);
      return () => {
        slotsCalls += 1;
        return nextSlotResponse();
      };
    })(),
    onStatusChange: () => () => {},
    connectLogStream: () => ({ close() {} }),
  };
  const apiModule = {
    __esModule: true,
    default: api,
    getCacheTokenCount: (slot) => slot.cache_tokens || 0,
    friendlyErrorMessage: (error) => String(error),
  };

  const originalTsLoader = require.extensions['.ts'];
  const originalModuleLoad = Module._load;
  const originalSetInterval = global.setInterval;
  const originalClearInterval = global.clearInterval;
  const originalDateNow = Date.now;

  require.extensions['.ts'] = function loadTypeScript(module, filename) {
    const source = fs.readFileSync(filename, 'utf8');
    const compiled = ts.transpileModule(source, {
      compilerOptions: {
        esModuleInterop: true,
        module: ts.ModuleKind.CommonJS,
        moduleResolution: ts.ModuleResolutionKind.NodeJs,
        target: ts.ScriptTarget.ES2020,
      },
      fileName: filename,
    }).outputText;
    module._compile(compiled, filename);
  };
  Module._load = function loadWithDashboardStubs(request, parent, isMain) {
    if (request === 'react') return fakeReact;
    if (request === '../api' && parent?.filename === hookPath) return apiModule;
    return originalModuleLoad.apply(this, arguments);
  };
  global.setInterval = (callback, interval) => {
    const timer = { callback, interval, active: true };
    timers.push(timer);
    return timer;
  };
  global.clearInterval = (timer) => {
    if (timer) timer.active = false;
  };
  Date.now = () => now;

  delete require.cache[hookPath];
  let useDashboardData;
  try {
    ({ useDashboardData } = require(hookPath));
  } finally {
    Module._load = originalModuleLoad;
    if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
    else delete require.extensions['.ts'];
  }

  const render = () => {
    stateIndex = 0;
    refIndex = 0;
    output = useDashboardData(true);
    return output;
  };

  const timerFor = (interval) => timers.find((timer) => timer.interval === interval && timer.active);

  return {
    async mount() {
      render();
      for (const effect of effects) effect();
      await flush();
      return this;
    },
    async poll() {
      const timer = timerFor(2000);
      assert.ok(timer, 'dashboard should register its two-second poll');
      await timer.callback();
      await flush();
    },
    tick(count = 1) {
      const timer = timerFor(200);
      assert.ok(timer, 'dashboard should register its interpolation ticker');
      for (let i = 0; i < count; i += 1) timer.callback();
    },
    advance(milliseconds) {
      now += milliseconds;
    },
    data() {
      collectEffects = false;
      return render();
    },
    get slotsCalls() {
      return slotsCalls;
    },
    dispose() {
      global.setInterval = originalSetInterval;
      global.clearInterval = originalClearInterval;
      Date.now = originalDateNow;
      delete require.cache[hookPath];
    },
  };
}

const tests = [
  {
    name: 'Dashboard retains a newer failure when an older success arrives',
    async run() {
      const old = deferred();
      const dashboard = createDashboardHarness({healthValues: [health(), old.promise, null], statsValues: [stats(1), stats(2), stats(3)]});
      try {
        await dashboard.mount(); const p1 = dashboard.poll();
        await dashboard.poll();
        const error = dashboard.data().lastError;
        assert.ok(error);
        old.resolve(health()); await p1;
        assert.equal(dashboard.data().lastError, error);
        assert.equal(dashboard.data().stats.request_count_total, 1);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard still pauses after three current health failures',
    async run() {
      const healthValues = [health()];
      for (let i = 0; i < 3; i++) {
        const failure = deferred();
        healthValues.push(failure.promise);
        failure.resolve(null);
      }
      const dashboard = createDashboardHarness({healthValues, statsValues: [stats(1)]});
      try {
        await dashboard.mount();
        for (let i = 0; i < 3; i++) await dashboard.poll();
        assert.equal(dashboard.data().paused, true);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard ignores a stale slots rejection after a newer poll',
    async run() {
      const delayed = deferred();
      const initial = health('llamacpp');
      const unloaded = {...initial, model_loaded: null, all_models_loaded: []};
      const dashboard = createDashboardHarness({
        recipe: 'llamacpp', healthValues: [initial, initial, unloaded],
        statsValues: [stats(1), stats(2), stats(3)], slotsValues: [[], delayed.promise],
      });
      try {
        await dashboard.mount();
        const p1 = dashboard.poll(); await flush();
        await dashboard.poll();
        delayed.reject(new Error('stale slots failure')); await p1;
        assert.equal(dashboard.data().slotsUnsupported, false);
        assert.equal(dashboard.data().slotStatus, 'Load a llama.cpp or vLLM chat model to see slot telemetry.');
        assert.equal(dashboard.data().stats.request_count_total, 3);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard ignores an older stats response after a newer snapshot applies',
    async run() {
      const old = deferred();
      const newer = deferred();
      const dashboard = createDashboardHarness({ statsValues: [stats(1), old.promise, newer.promise] });
      try {
        await dashboard.mount();
        const p1 = dashboard.poll();
        const p2 = dashboard.poll();
        newer.resolve(stats(3, 30)); await p2;
        old.resolve(stats(2, 20)); await p1;
        assert.equal(dashboard.data().stats.request_count_total, 3);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard applies a slow snapshot while a newer poll is still pending',
    async run() {
      const old = deferred(); const newer = deferred();
      const dashboard = createDashboardHarness({statsValues: [stats(1), old.promise, newer.promise]});
      try {
        await dashboard.mount();
        const p1 = dashboard.poll(); const p2 = dashboard.poll();
        old.resolve(stats(2)); await p1;
        assert.equal(dashboard.data().stats.request_count_total, 2);
        newer.resolve(stats(3)); await p2;
        assert.equal(dashboard.data().stats.request_count_total, 3);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard commits health and slots together despite delayed slots',
    async run() {
      const delayedSlots = deferred();
      const initial = health('llamacpp');
      const unloaded = {...initial, model_loaded: null, all_models_loaded: []};
      const dashboard = createDashboardHarness({
        recipe: 'llamacpp', healthValues: [initial, initial, unloaded],
        statsValues: [stats(1), stats(2), stats(3)], slotsValues: [[], delayedSlots.promise],
      });
      try {
        await dashboard.mount();
        const p1 = dashboard.poll(); await flush();
        assert.equal(dashboard.data().stats.request_count_total, 1, 'partial snapshots must not apply before slots');
        await dashboard.poll();
        delayedSlots.resolve([slot({decoded: 10, prompted: 80})]); await p1;
        assert.equal(dashboard.data().health.model_loaded, null);
        assert.deepEqual(dashboard.data().slots, []);
        assert.equal(dashboard.data().stats.request_count_total, 3);
      } finally { dashboard.dispose(); }
    },
  },
  {
    name: 'Dashboard ignores stale failures after newer success',
    async run() {
      const old = deferred();
      const dashboard = createDashboardHarness({ healthValues: [health(), old.promise, health()], statsValues: [stats(1), stats(2), stats(3)] });
      try {
        await dashboard.mount(); const p1 = dashboard.poll();
        await dashboard.poll(); old.reject(new Error('stale failure')); await p1;
        assert.equal(dashboard.data().lastError, null);
        assert.equal(dashboard.data().paused, false);
      } finally { dashboard.dispose(); }
    },
  },
];
module.exports = {tests};
