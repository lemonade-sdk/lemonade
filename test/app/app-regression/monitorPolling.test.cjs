const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');
const hookPath = path.join(appRoot, 'src', 'hooks', 'useMonitorData.ts');

let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  module.exports = {
    tests: [{
      name: 'Monitor polling rejects stale snapshots',
      run: () => ({ skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" }),
    }],
  };
  return;
}

const model = (recipe = 'flm') => ({
  model_name: 'test-model',
  recipe,
  type: 'llm',
  device: 'gpu',
});

const health = (recipe = 'flm', loaded = true) => ({
  version: 'test',
  model_loaded: loaded ? 'test-model' : null,
  all_models_loaded: loaded ? [model(recipe)] : [],
  websocket_port: 0,
});

const stats = (requestCount) => ({
  input_tokens: 0,
  output_tokens: 0,
  tokens_per_second: 0,
  request_count_total: requestCount,
  live: { active_requests: 0 },
});

const slot = () => ({
  id: 0,
  n_ctx: 1024,
  n_decoded: 1,
  n_prompt_tokens_processed: 1,
  cache_tokens: [],
  is_processing: true,
  state: 'processing',
});

function sequence(values) {
  let index = 0;
  return () => Promise.resolve(values[Math.min(index++, values.length - 1)]);
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

async function flush() {
  for (let i = 0; i < 8; i += 1) await Promise.resolve();
}

function sameDependencies(left, right) {
  return left && right && left.length === right.length
    && left.every((value, index) => Object.is(value, right[index]));
}

function createMonitorHarness({
  healthValues = [health()],
  statsValues = [stats(1)],
  slotsValues = [[]],
} = {}) {
  const state = [];
  const refs = [];
  const effectRecords = [];
  const pendingEffects = [];
  const callbacks = [];
  const memoizedValues = [];
  const timers = [];
  let stateIndex = 0;
  let refIndex = 0;
  let effectIndex = 0;
  let callbackIndex = 0;
  let memoIndex = 0;
  let healthCallCount = 0;
  const healthResponses = sequence(healthValues);

  const fakeReact = {
    useState(initial) {
      const index = stateIndex++;
      if (!(index in state)) state[index] = typeof initial === 'function' ? initial() : initial;
      return [state[index], (next) => {
        state[index] = typeof next === 'function' ? next(state[index]) : next;
      }];
    },
    useRef(initial) {
      const index = refIndex++;
      if (!(index in refs)) refs[index] = { current: initial };
      return refs[index];
    },
    useCallback(callback, dependencies) {
      const index = callbackIndex++;
      const previous = callbacks[index];
      if (!previous || !sameDependencies(previous.dependencies, dependencies)) {
        callbacks[index] = { callback, dependencies };
      }
      return callbacks[index].callback;
    },
    useMemo(factory, dependencies) {
      const index = memoIndex++;
      const previous = memoizedValues[index];
      if (!previous || !sameDependencies(previous.dependencies, dependencies)) {
        memoizedValues[index] = { value: factory(), dependencies };
      }
      return memoizedValues[index].value;
    },
    useEffect(effect, dependencies) {
      const index = effectIndex++;
      const previous = effectRecords[index];
      if (!previous || !sameDependencies(previous.dependencies, dependencies)) {
        pendingEffects.push({ index, effect, dependencies });
      }
    },
  };

  const api = {
    systemInfoData: {},
    lastConnectionError: '',
    health: () => {
      healthCallCount += 1;
      return healthResponses();
    },
    stats: sequence(statsValues),
    systemStats: () => Promise.resolve({
      cpu_percent: 10,
      memory_gb: 2,
      gpu_percent: 0,
      vram_gb: null,
      npu_percent: 0,
      gpus: [],
    }),
    systemInfo: () => Promise.resolve({}),
    slots: sequence(slotsValues),
    onStatusChange: () => () => {},
    connectLogStream: () => ({ close() {} }),
  };
  const apiModule = {
    __esModule: true,
    default: api,
    friendlyErrorMessage: (error) => error instanceof Error ? error.message : String(error),
  };
  const originalTsLoader = require.extensions['.ts'];
  const originalModuleLoad = Module._load;
  const originalSetInterval = global.setInterval;
  const originalClearInterval = global.clearInterval;

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
  Module._load = function loadWithMonitorStubs(request, parent) {
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

  delete require.cache[hookPath];
  let useMonitorData;
  try {
    ({ useMonitorData } = require(hookPath));
  } finally {
    Module._load = originalModuleLoad;
    if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
    else delete require.extensions['.ts'];
  }

  const render = () => {
    stateIndex = 0;
    refIndex = 0;
    effectIndex = 0;
    callbackIndex = 0;
    memoIndex = 0;
    pendingEffects.length = 0;
    const data = useMonitorData(true);
    for (const pending of pendingEffects) {
      effectRecords[pending.index]?.cleanup?.();
      const cleanup = pending.effect();
      effectRecords[pending.index] = {
        dependencies: pending.dependencies,
        cleanup: typeof cleanup === 'function' ? cleanup : null,
      };
    }
    return data;
  };
  const timerFor = (interval) => timers.find((timer) => timer.interval === interval && timer.active);

  return {
    async mount() {
      render();
      await flush();
      return this;
    },
    poll() {
      const timer = timerFor(2000);
      assert.ok(timer, 'Monitor should register its two-second poll');
      return timer.callback();
    },
    data() { return render(); },
    pollingActive() { return Boolean(timerFor(2000)); },
    get healthCallCount() { return healthCallCount; },
    dispose() {
      for (const effect of effectRecords) effect?.cleanup?.();
      global.setInterval = originalSetInterval;
      global.clearInterval = originalClearInterval;
      delete require.cache[hookPath];
    },
  };
}

const tests = [
  {
    name: 'Monitor retains a newer snapshot when an older poll finishes last',
    async run() {
      const older = deferred();
      const monitor = createMonitorHarness({ statsValues: [stats(1), older.promise, stats(3)] });
      try {
        await monitor.mount();
        const oldPoll = monitor.poll();
        const newPoll = monitor.poll();
        await newPoll;
        older.resolve(stats(2));
        await oldPoll;
        assert.equal(monitor.data().stats.request_count_total, 3);
      } finally { monitor.dispose(); }
    },
  },
  {
    name: 'Monitor accepts a completed snapshot while a newer poll is pending',
    async run() {
      const older = deferred();
      const newer = deferred();
      const monitor = createMonitorHarness({ statsValues: [stats(1), older.promise, newer.promise] });
      try {
        await monitor.mount();
        const oldPoll = monitor.poll();
        const newPoll = monitor.poll();
        older.resolve(stats(2));
        await oldPoll;
        assert.equal(monitor.data().stats.request_count_total, 2);
        newer.resolve(stats(3));
        await newPoll;
        assert.equal(monitor.data().stats.request_count_total, 3);
      } finally { monitor.dispose(); }
    },
  },
  {
    name: 'Monitor waits for delayed slots before committing health and stats',
    async run() {
      const delayedSlots = deferred();
      const updatedHealth = { ...health('llamacpp'), version: 'updated' };
      const monitor = createMonitorHarness({
        healthValues: [health('llamacpp'), updatedHealth],
        statsValues: [stats(1), stats(2)],
        slotsValues: [[], delayedSlots.promise],
      });
      try {
        await monitor.mount();
        const poll = monitor.poll();
        await flush();
        assert.equal(monitor.data().stats.request_count_total, 1);
        assert.equal(monitor.data().health.version, 'test');
        delayedSlots.resolve([slot()]);
        await poll;
        assert.equal(monitor.data().stats.request_count_total, 2);
        assert.equal(monitor.data().health.version, 'updated');
        assert.equal(monitor.data().slots.length, 1);
      } finally { monitor.dispose(); }
    },
  },
  {
    name: 'Monitor ignores stale slots success after a newer snapshot',
    async run() {
      const delayedSlots = deferred();
      const monitor = createMonitorHarness({
        healthValues: [health('llamacpp'), health('llamacpp'), health('llamacpp', false)],
        statsValues: [stats(1), stats(2), stats(3)],
        slotsValues: [[], delayedSlots.promise],
      });
      try {
        await monitor.mount();
        const oldPoll = monitor.poll();
        await flush();
        await monitor.poll();
        delayedSlots.resolve([slot()]);
        await oldPoll;
        const data = monitor.data();
        assert.equal(data.health.model_loaded, null);
        assert.equal(data.stats.request_count_total, 3);
        assert.deepEqual(data.slots, []);
        assert.equal(data.getSlotTarget(0), undefined);
        assert.equal(data.counters.prevSlotTokens.has(0), false);
      } finally { monitor.dispose(); }
    },
  },
  {
    name: 'Monitor ignores stale slots rejection after a newer snapshot',
    async run() {
      const delayedSlots = deferred();
      const monitor = createMonitorHarness({
        healthValues: [health('llamacpp'), health('llamacpp'), health('llamacpp', false)],
        statsValues: [stats(1), stats(2), stats(3)],
        slotsValues: [[], delayedSlots.promise],
      });
      try {
        await monitor.mount();
        const oldPoll = monitor.poll();
        await flush();
        await monitor.poll();
        delayedSlots.reject(new Error('stale slots failure'));
        await oldPoll;
        const data = monitor.data();
        assert.equal(data.slotsUnsupported, false);
        assert.equal(data.slotStatus, 'Load a llama.cpp or vLLM chat model to see slot telemetry.');
        assert.equal(data.lastError, null);
        assert.equal(data.stats.request_count_total, 3);
      } finally { monitor.dispose(); }
    },
  },
  {
    name: 'Monitor ignores stale health failure after a newer success',
    async run() {
      const delayedHealth = deferred();
      const monitor = createMonitorHarness({
        healthValues: [health(), delayedHealth.promise, health(), null, null, null],
        statsValues: [stats(1), stats(2), stats(3), stats(4), stats(5), stats(6)],
      });
      try {
        await monitor.mount();
        const oldPoll = monitor.poll();
        const newPoll = monitor.poll();
        await newPoll;
        delayedHealth.reject(new Error('stale health failure'));
        await oldPoll;
        assert.equal(monitor.data().lastError, null);
        assert.equal(monitor.data().paused, false);
        assert.equal(monitor.data().stats.request_count_total, 3);
        await monitor.poll();
        assert.equal(monitor.data().paused, false);
        await monitor.poll();
        assert.equal(monitor.data().paused, false);
        await monitor.poll();
        assert.equal(monitor.data().paused, true);
        assert.equal(monitor.pollingActive(), false);
        assert.equal(monitor.healthCallCount, 6);
      } finally { monitor.dispose(); }
    },
  },
];

module.exports = { tests };
