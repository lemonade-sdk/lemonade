const statusListeners = [];
const state = { base: globalThis.__AO_INITIAL_BASE__ || 'http://server-a:1111' };
const interruptCalls = [];
const deleteCalls = [];

const api = {
  get baseUrl() { return state.base; },
  get healthData() { return { version: 'test' }; },
  get allModels() { return []; },
  onStatusChange(fn) {
    statusListeners.push(fn);
    return () => {
      const i = statusListeners.indexOf(fn);
      if (i >= 0) statusListeners.splice(i, 1);
    };
  },
  interruptJob(id) { interruptCalls.push(id); return Promise.resolve(); },
  deleteJob(id) { deleteCalls.push(id); return Promise.resolve(); },
};

globalThis.__AO_API_CONTROL__ = {
  interruptCalls,
  deleteCalls,
  setBase(b) {
    state.base = b;
    statusListeners.slice().forEach(fn => { try { fn('connected'); } catch {} });
  },
  setBaseSilent(b) {
    state.base = b;
  },
};

module.exports = { __esModule: true, default: api };
