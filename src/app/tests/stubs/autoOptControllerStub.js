const AUTOOPT_STAGES = ['snapshot', 'model_facts', 'hf_metadata', 'fit_estimate', 'bench_job', 'synthesize'];

const attached = [];

function track(id, cb, signal) {
  const entry = { id, cb, signal };
  attached.push(entry);
  return new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => {
      reject(Object.assign(new Error('cancelled by user'), { name: 'AbortError' }));
    }, { once: true });
  });
}

function resumeAutoOptRun(run, cb, signal) {
  return track(run.id, cb, signal);
}

function executeAutoOptRun(_request, signal, cb) {
  return track('(new)', cb, signal);
}

globalThis.__AO_CTRL__ = {
  attached,
  fire(id) {
    const entry = attached.find(a => a.id === id);
    if (!entry || !entry.cb) return;
    entry.cb.bench([{
      backend: 'vulkan', label: 'probe', ctx_size: 2048, llamacpp_args: '',
      params: { d: 0 }, ttft_ms: 10, tps: 10, vram_gb: 5, ok: true,
    }]);
  },
};

module.exports = { __esModule: true, AUTOOPT_STAGES, resumeAutoOptRun, executeAutoOptRun };
