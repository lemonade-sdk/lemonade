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

const liveFlm = (overrides = {}) => ({
  inference_active: true,
  live_active_requests: 1,
  live_generated_chunks: 12,
  live_generation_rate_estimate: 6.5,
  live_generation_rate_unit: 'semantic_sse_chunks_per_second',
  live_generation_recipe: 'flm',
  live_generation_device: 'npu',
  live_generation_model: 'test-model',
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
  let resolve;
  const promise = new Promise((resolvePromise) => {
    resolve = resolvePromise;
  });
  return { promise, resolve };
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
    name: 'Dashboard aggregates completed FastFlowLM requests when slots are unsupported',
    async run() {
      const dashboard = createDashboardHarness({ statsValues: [stats(1), stats(1)] });
      try {
        await dashboard.mount();
        dashboard.tick();
        const data = dashboard.data();
        assert.equal(dashboard.slotsCalls, 0, 'FastFlowLM must not call the unsupported slots endpoint');
        assert.equal(data.slotsUnsupported, true);
        assert.equal(data.completedRequestFallback, true);
        assert.equal(data.nonSlotGraphMode, 'completed', 'old servers should retain the exact completed-throughput fallback');
        assert.equal(data.liveEstimateActive, false, 'missing optional live fields must not fabricate a stream estimate');
        assert.equal(data.counters.peakTps, 33.3, 'fallback should preserve the measured completion rate');
        assert.ok(Number.isFinite(data.latestTps) && data.latestTps > 0.05,
          `expected a completed-request throughput sample, got ${data.latestTps}`);
        assert.equal(data.latestPP, 0, 'completed requests do not provide prompt throughput');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard keeps live FLM chunk estimates separate from exact completed throughput',
    async run() {
      const dashboard = createDashboardHarness({
        statsValues: [
          stats(0, 0, liveFlm()),
          stats(1, 33.3, liveFlm({
            inference_active: false,
            live_active_requests: 0,
            live_generation_rate_estimate: 0,
          })),
        ],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        let data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'live-estimate');
        assert.equal(data.liveEstimateActive, true);
        assert.equal(data.liveEstimateUnit, 'semantic_sse_chunks_per_second');
        assert.ok(data.latestLiveEstimate > 0.05, 'active FLM streams should expose the server estimate');
        assert.equal(data.latestTps, 0, 'a live chunk estimate must not become exact completed tok/s');
        assert.ok(data.liveEstimateChartData.some((point) => point.liveEstimate > 0.05),
          'live estimate history should have its own chunks/s series');
        assert.ok(data.aggChartData.every((point) => point.genTps === 0),
          'the exact tok/s history must remain separate while streaming');

        await dashboard.poll();
        dashboard.tick(20);
        data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'completed');
        assert.equal(data.liveEstimateActive, false);
        assert.ok(data.latestTps > 0.05, 'completion should return to the exact completed tok/s sample');
        assert.ok(data.latestLiveEstimate < 0.05, 'completed data must not retain a live chunk estimate');
        assert.ok(data.aggChartData.some((point) => point.genTps > 0.05),
          'the exact completed history should receive the final server rate');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard does not fabricate a live FLM rate before stream events or after abort',
    async run() {
      const dashboard = createDashboardHarness({
        statsValues: [
          stats(0, 0, liveFlm({
            live_generated_chunks: 0,
            live_generation_rate_estimate: 0,
          })),
          stats(0, 0, liveFlm({
            inference_active: false,
            live_active_requests: 0,
            live_generated_chunks: 0,
            live_generation_rate_estimate: 0,
          })),
        ],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        let data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'live-estimate');
        assert.equal(data.liveEstimateActive, true);
        assert.equal(data.latestLiveEstimate, 0, 'zero events must remain an empty estimate');
        assert.equal(data.latestTps, 0, 'zero events must not create a completed rate');

        await dashboard.poll();
        dashboard.tick(20);
        data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'completed');
        assert.equal(data.liveEstimateActive, false, 'an aborted stream must clear the live mode');
        assert.equal(data.latestLiveEstimate, 0);
        assert.equal(data.latestTps, 0, 'an abort without a new request count has no exact completion rate');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard ignores incompatible live FLM metadata and uses exact completed throughput',
    async run() {
      const dashboard = createDashboardHarness({
        statsValues: [
          stats(1, 33.3, liveFlm({ live_generation_rate_unit: 'tokens_per_second' })),
          stats(2, 24, liveFlm({ live_generation_recipe: 'llamacpp' })),
          stats(3, 18, liveFlm({ live_generation_model: 'other-model' })),
        ],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        let data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'completed');
        assert.equal(data.liveEstimateActive, false, 'an unknown rate unit must not be rendered as chunks/s');
        assert.ok(data.latestTps > 0.05);

        await dashboard.poll();
        dashboard.tick(20);
        data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'completed');
        assert.equal(data.liveEstimateActive, false, 'a non-FLM recipe must not drive the FLM estimate');
        assert.ok(data.latestTps > 0.05);

        await dashboard.poll();
        dashboard.tick(20);
        data = dashboard.data();
        assert.equal(data.nonSlotGraphMode, 'completed');
        assert.equal(data.liveEstimateActive, false, 'an unknown FLM model must not drive a server-global estimate');
        assert.ok(data.latestTps > 0.05);
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard deduplicates unchanged completed requests by counter and samples the next identical rate',
    async run() {
      const dashboard = createDashboardHarness({
        statsValues: [stats(1), ...Array.from({ length: 8 }, () => stats(1)), stats(2)],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        const firstSample = dashboard.data().latestTps;
        assert.ok(firstSample > 0.05, 'the first completed request should produce a sample');

        for (let i = 0; i < 8; i += 1) {
          await dashboard.poll();
          dashboard.tick(20);
        }
        const decayed = dashboard.data().latestTps;
        assert.ok(decayed < 0.05, `unchanged stats should decay to zero, got ${decayed}`);

        await dashboard.poll();
        dashboard.tick();
        const nextSample = dashboard.data().latestTps;
        assert.ok(nextSample > 0.05,
          `a new request counter must sample even when its rate matches the prior request, got ${nextSample}`);
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard accepts a slow poll until a newer snapshot has actually applied',
    async run() {
      const older = deferred();
      const newer = deferred();
      const dashboard = createDashboardHarness({
        statsValues: [stats(1), older.promise, newer.promise],
      });
      let newerPoll;
      try {
        await dashboard.mount();
        const olderPoll = dashboard.poll();
        newerPoll = dashboard.poll();

        older.resolve(stats(2, 20));
        await olderPoll;
        assert.equal(dashboard.data().stats.request_count_total, 2,
          'a slow poll should apply before a newer in-flight poll responds');

        newer.resolve(stats(3, 30));
        await newerPoll;
        assert.equal(dashboard.data().stats.request_count_total, 3,
          'a newer completed snapshot should replace the older one');
      } finally {
        newer.resolve(stats(3, 30));
        if (newerPoll) await newerPoll;
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard ignores an older poll after a newer snapshot applies',
    async run() {
      const older = deferred();
      const newer = deferred();
      const dashboard = createDashboardHarness({
        statsValues: [stats(1), older.promise, newer.promise],
      });
      let olderPoll;
      try {
        await dashboard.mount();
        olderPoll = dashboard.poll();
        const newerPoll = dashboard.poll();

        newer.resolve(stats(3, 30));
        await newerPoll;
        assert.equal(dashboard.data().stats.request_count_total, 3);

        older.resolve(stats(2, 20));
        await olderPoll;
        assert.equal(dashboard.data().stats.request_count_total, 3,
          'late data must not overwrite a newer applied snapshot');
      } finally {
        older.resolve(stats(2, 20));
        if (olderPoll) await olderPoll;
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard does not replay slot-backend completions after returning to FastFlowLM',
    async run() {
      const slotPolls = Array.from({ length: 8 }, () => health('llamacpp'));
      const dashboard = createDashboardHarness({
        healthValues: [health('flm'), ...slotPolls, health('flm')],
        statsValues: [stats(1), ...Array.from({ length: 8 }, (_, index) => stats(index + 2)), stats(9)],
        slots: [],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        assert.ok(dashboard.data().latestTps > 0.05, 'the initial FastFlowLM completion should sample once');

        for (let i = 0; i < 8; i += 1) {
          await dashboard.poll();
          dashboard.tick(20);
        }
        assert.ok(dashboard.data().latestTps < 0.05, 'slot-backed polls should decay the prior completed-request sample');

        await dashboard.poll();
        dashboard.tick();
        assert.equal(dashboard.data().latestTps, 0,
          'returning to FastFlowLM must not replay a completion already observed on a slot backend');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard ignores reset and invalid completed-request samples without storing non-finite rates',
    async run() {
      const dashboard = createDashboardHarness({
        statsValues: [
          stats(0, 33.3),
          stats(7),
          stats(0, 33.3),
          ...Array.from({ length: 8 }, () => stats(1, Number.NaN)),
          stats(2, 24),
        ],
      });
      try {
        await dashboard.mount();
        dashboard.tick(20);
        assert.equal(dashboard.data().latestTps, 0,
          'a zero request counter must not plot a leftover completion rate');

        await dashboard.poll();
        dashboard.tick(20);
        assert.ok(dashboard.data().latestTps > 0.05, 'a positive completed request should sample once');
        await dashboard.poll();
        dashboard.tick(20);
        for (let i = 0; i < 8; i += 1) {
          await dashboard.poll();
          dashboard.tick(20);
        }
        const afterInvalid = dashboard.data();
        assert.ok(afterInvalid.latestTps < 0.05,
          `reset and invalid samples must not replay the prior rate, got ${afterInvalid.latestTps}`);
        assert.ok(afterInvalid.aggChartData.every((point) => Number.isFinite(point.genTps)),
          'invalid stats must never enter the aggregate chart');

        await dashboard.poll();
        dashboard.tick();
        assert.ok(dashboard.data().latestTps > 0.05,
          'the first valid completion after a reset must be sampled once');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard keeps slot telemetry as the aggregate source for llama.cpp',
    async run() {
      const dashboard = createDashboardHarness({
        recipe: 'llamacpp',
        statsValues: [stats(1)],
        slots: [],
      });
      try {
        await dashboard.mount();
        dashboard.tick();
        const data = dashboard.data();
        assert.equal(dashboard.slotsCalls, 1, 'llama.cpp should continue querying slots');
        assert.equal(data.slotsUnsupported, false);
        assert.equal(data.latestTps, 0, 'completed-request fallback must not override live slot telemetry');
      } finally {
        dashboard.dispose();
      }
    },
  },
  {
    name: 'Dashboard preserves live llama.cpp generation and prompt telemetry',
    async run() {
      const dashboard = createDashboardHarness({
        recipe: 'llamacpp',
        statsValues: [stats(1, 33.3, liveFlm())],
        slotsValues: [
          [slot({ decoded: 10, prompted: 80, promptPerSecond: 11 })],
          [slot({ decoded: 30, prompted: 80, promptPerSecond: 11 })],
        ],
      });
      try {
        await dashboard.mount();
        dashboard.advance(1000);
        await dashboard.poll();
        dashboard.tick(20);
        const data = dashboard.data();
        assert.equal(dashboard.slotsCalls, 2);
        assert.equal(data.completedRequestFallback, false);
        assert.equal(data.nonSlotGraphMode, 'slots', 'active llama.cpp slots must remain the primary graph');
        assert.equal(data.liveEstimateActive, false, 'FLM global estimates must not replace active slot telemetry');
        assert.ok(data.latestTps > 0.05, 'live slot deltas should remain the generation source');
        assert.ok(data.latestPP > 0.05, 'live slot prompt telemetry should remain available');
      } finally {
        dashboard.dispose();
      }
    },
  },
];

module.exports = { tests };
