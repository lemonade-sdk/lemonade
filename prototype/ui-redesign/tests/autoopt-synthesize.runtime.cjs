const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundleSynthesize() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-autoopt-synth-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/features/autoOpt/autoOptSynthesize.ts'),
    output: {
      path: outputPath,
      filename: 'autoOptSynthesize.cjs',
      library: { type: 'commonjs2' },
    },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: {
      rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }],
    },
    optimization: { minimize: false },
  };

  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });

  return { outputPath, modulePath: path.join(outputPath, 'autoOptSynthesize.cjs') };
}

async function bundleRecipe() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-autoopt-recipe-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/features/autoOpt/autoOptRecipe.ts'),
    output: {
      path: outputPath,
      filename: 'autoOptRecipe.cjs',
      library: { type: 'commonjs2' },
    },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: {
      rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }],
    },
    optimization: { minimize: false },
  };

  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });

  return { outputPath, modulePath: path.join(outputPath, 'autoOptRecipe.cjs') };
}

function igpuHw() {
  return {
    gpus: [{ vendor: 'amd', name: 'Radeon 8060S', family: 'gfx1151', vram_gb: 96.0 }],
    has_igpu: true,
    ram_is_vram: true,
    host_ram_gb: 128,
    installed_backends: ['vulkan', 'rocm-stable'],
    os: 'linux',
  };
}

function dgpuHw() {
  return {
    gpus: [{ vendor: 'nvidia', name: 'RTX 4090', family: 'sm_89', vram_gb: 24.0 }],
    has_igpu: false,
    ram_is_vram: false,
    host_ram_gb: 64,
    installed_backends: ['cuda'],
    os: 'linux',
  };
}

function denseModel() {
  return {
    architecture: 'qwen3',
    block_count: 36,
    expert_count: 0,
    full_attention_interval: 0,
    swa_layer_count: 0,
    n_ctx_train: 131072,
    kv_bytes_per_token: 90112,
    weights_mib: 18000,
    is_moe: false,
    is_hybrid_or_recurrent: false,
    has_mtp: false,
    base_model_repo: '',
    checkpoint: '',
    metadata_present: true,
  };
}

function answers(overrides = {}) {
  return {
    parallel: false,
    slots: 1,
    dedicated_slots: true,
    kv_cache_quant: 'none',
    ram_headroom: 'normal',
    allow_network: true,
    backends_to_consider: [],
    ...overrides,
  };
}

// A heuristic fit that fully fits, with a chosen f16 context cap.
function fitting(backend, ctx, overrides = {}) {
  return {
    backend,
    fits_fully: true,
    fitted_ctx: ctx,
    fitted_ngl: -1,
    fitted_ncmoe: 0,
    weights_mib: 18000,
    kv_mib: 4000,
    compute_mib: 512,
    total_mib: 22512,
    available_mib: 98304,
    degraded: false,
    ok: true,
    ...overrides,
  };
}

// A measured bench point in the new TTFT/TPS shape.
function duelPoint(backend, depth, ttftMs, tps) {
  return {
    backend, label: `${backend} · d${depth}`, ctx_size: depth > 0 ? 32768 : 4096,
    llamacpp_args: '', params: { d: depth }, ttft_ms: ttftMs, tps, vram_gb: 20, ok: true,
  };
}

function ladderPoint(backend, b, ttftMs) {
  return {
    backend, label: `${backend} · b${b}`, ctx_size: 4096,
    llamacpp_args: `-b ${b} -ub ${b}`, params: { ladder: true, b, ub: b, d: 0 },
    ttft_ms: ttftMs, tps: 40, vram_gb: 20, ok: true,
  };
}

function mtpPoint(backend, n, tps) {
  return {
    backend, label: `${backend} · mtp${n}`, ctx_size: 4096,
    llamacpp_args: '', params: { spec_n: n, d: 0 }, ttft_ms: 100, tps, vram_gb: 20, ok: true,
  };
}

function hasArg(preset, needle) {
  return preset.llamacpp_args.includes(needle);
}

let failures = 0;
function check(name, ok) {
  console.log(`[${ok ? 'PASS' : 'FAIL'}] ${name}`);
  if (!ok) failures += 1;
}

(async () => {
  const { outputPath, modulePath } = await bundleSynthesize();

  try {
    const engine = require(modulePath);
    const {
      synthesize, checkpointRepoId, computeFitEstimate, benchScore, roundUpCtx,
      selectCandidates, availableMib, availableMibForBackend, backendLoadArgs, NO_GPU_BACKEND_ERROR,
    } = engine;

    // ── checkpoint_repo_id ────────────────────────────────────────────
    check('ckpt: plain repo', checkpointRepoId('Qwen/Qwen3-32B') === 'Qwen/Qwen3-32B');
    check('ckpt: variant suffix stripped',
      checkpointRepoId('ilintar/Agents-A1-GGUF:IQ4_XS') === 'ilintar/Agents-A1-GGUF');
    check('ckpt: url form',
      checkpointRepoId('https://huggingface.co/Qwen/Qwen3-32B') === 'Qwen/Qwen3-32B');
    check('ckpt: bare name rejected', checkpointRepoId('just-a-name') === '');

    // ── roundUpCtx ────────────────────────────────────────────────────
    check('roundUp: 30000 -> 32768', roundUpCtx(30000) === 32768);
    check('roundUp: 2048 -> 2048', roundUpCtx(2048) === 2048);

    // ── benchScore: monotonic in the right directions ─────────────────
    check('score: higher tps wins at equal ttft',
      benchScore(50, 100, 50, 100) > benchScore(40, 100, 50, 100));
    check('score: lower ttft wins at equal tps',
      benchScore(50, 100, 50, 100) > benchScore(50, 200, 50, 100));
    check('score: best on both = 1.0',
      Math.abs(benchScore(50, 100, 50, 100) - 1.0) < 1e-9);
    check('score: tps weighted higher than ttft',
      // candidate A: best tps, worst ttft; candidate B: worst tps, best ttft.
      benchScore(50, 200, 50, 100) > benchScore(25, 100, 50, 100));

    // ── computeFitEstimate: heuristic memory fit ──────────────────────
    {
      // Fits fully with the whole trained window.
      const full = computeFitEstimate({
        backend: 'vulkan', availableMib: 98304, weightsMib: 18000,
        kvBytesPerToken: 90112, blockCount: 36, isMoe: false, nCtxTrain: 32768, degraded: false,
      });
      check('fit: full fit, no cap', full.fits_fully && full.fitted_ctx === 0 && full.fitted_ngl === -1);

      // Weights fit but KV must be capped below the trained window.
      const capped = computeFitEstimate({
        backend: 'vulkan', availableMib: 24000, weightsMib: 18000,
        kvBytesPerToken: 1048576, blockCount: 36, isMoe: false, nCtxTrain: 131072, degraded: false,
      });
      check('fit: weights fit, ctx capped', capped.fits_fully && capped.fitted_ctx > 0 && capped.fitted_ctx < 131072);

      // Dense weights don't fit → partial -ngl offload.
      const dense = computeFitEstimate({
        backend: 'vulkan', availableMib: 8000, weightsMib: 18000,
        kvBytesPerToken: 90112, blockCount: 36, isMoe: false, nCtxTrain: 32768, degraded: false,
      });
      check('fit: dense offload sets partial ngl',
        !dense.fits_fully && dense.fitted_ngl >= 0 && dense.fitted_ngl < 36 && dense.fitted_ncmoe === 0);

      // MoE weights don't fit → -n-cpu-moe.
      const moe = computeFitEstimate({
        backend: 'vulkan', availableMib: 8000, weightsMib: 40000,
        kvBytesPerToken: 90112, blockCount: 48, isMoe: true, nCtxTrain: 32768, degraded: false,
      });
      check('fit: moe offload sets ncmoe', !moe.fits_fully && moe.fitted_ncmoe > 0);

      // Degraded (no metadata): uses a default kv/token and flags degraded.
      const degraded = computeFitEstimate({
        backend: 'vulkan', availableMib: 98304, weightsMib: 12000,
        kvBytesPerToken: 0, blockCount: 0, isMoe: false, nCtxTrain: 0, degraded: true,
      });
      check('fit: degraded flagged, still ok', degraded.degraded && degraded.ok);
    }

    // ── synthesis: dense partial offload EMITS -ngl (review #4) ────────
    {
      const denseFit = fitting('vulkan', 4096, {
        fits_fully: false, fitted_ngl: 20, fitted_ncmoe: 0, fitted_ctx: 4096,
      });
      const r = synthesize(igpuHw(), denseModel(), answers(), [denseFit], [], undefined);
      check('dense offload: -ngl 20 emitted', hasArg(r.primary, '-ngl 20'));
      check('dense offload: rationale mentions layers',
        r.primary.rationale.some(s => s.includes('20 of 36 layers')));
    }

    // ── lever: cache ram scale + hybrid bump ──────────────────────────
    {
      let r = synthesize(igpuHw(), denseModel(), answers({ ram_headroom: 'reduced' }),
        [fitting('vulkan', 65536)], [], undefined);
      check('cache: reduced scale', hasArg(r.primary, '--cache-ram 4096 -ctxcp 16'));

      r = synthesize(igpuHw(), denseModel(), answers({ ram_headroom: 'normal' }),
        [fitting('vulkan', 65536)], [], undefined);
      check('cache: normal omits flags', !hasArg(r.primary, '--cache-ram'));

      const mf = { ...denseModel(), full_attention_interval: 4, is_hybrid_or_recurrent: true };
      r = synthesize(igpuHw(), mf, answers({ ram_headroom: 'disabled' }),
        [fitting('vulkan', 65536)], [], undefined);
      check('cache: hybrid never disabled', hasArg(r.primary, '--cache-ram 2048 -ctxcp 8'));
    }

    // ── lever: kv quant + alternatives ────────────────────────────────
    {
      const r = synthesize(igpuHw(), denseModel(), answers({ kv_cache_quant: 'q8_0' }),
        [fitting('vulkan', 32768)], [], undefined);
      check('kv: ctk/ctv emitted', hasArg(r.primary, '-ctk q8_0 -ctv q8_0'));
      check('kv: ctx doubled by q8_0', r.primary.ctx_size === 65536);
      check('kv: max-quality alternative present',
        r.alternatives.length > 0 && r.alternatives[0].label === 'Maximum quality'
        && !r.alternatives[0].llamacpp_args.includes('-ctk'));
      check('kv: max-context alternative q4_0',
        r.alternatives.length >= 2 && r.alternatives[1].llamacpp_args.includes('-ctk q4_0'));
    }

    // ── lever: tensor split ───────────────────────────────────────────
    {
      const hw = dgpuHw();
      hw.gpus.push({ vendor: 'nvidia', name: 'RTX 4090', family: 'sm_89', vram_gb: 24.0 });
      let r = synthesize(hw, denseModel(), answers(), [fitting('cuda', 65536)], [], undefined);
      check('split: tensor split on dual identical CUDA', hasArg(r.primary, '--split-mode tensor'));

      const vk = igpuHw();
      vk.gpus.push({ vendor: 'amd', name: 'RX 7900', family: 'gfx1100', vram_gb: 24.0 });
      r = synthesize(vk, denseModel(), answers(), [fitting('vulkan', 65536)], [], undefined);
      check('split: no tensor split off CUDA', !hasArg(r.primary, '--split-mode'));
    }

    // ── lever: parallel slots ─────────────────────────────────────────
    {
      let r = synthesize(igpuHw(), denseModel(),
        answers({ parallel: true, slots: 4, dedicated_slots: true }),
        [fitting('vulkan', 65536)], [], undefined);
      check('parallel: dedicated slots', hasArg(r.primary, '-np 4 -no-kvu'));

      r = synthesize(igpuHw(), denseModel(),
        answers({ parallel: true, slots: 4, dedicated_slots: false }),
        [fitting('vulkan', 65536)], [], undefined);
      check('parallel: shared pool', hasArg(r.primary, '-np 4 -kvu'));
    }

    // ── lever: speculative + MTP argmax by tps ────────────────────────
    {
      let r = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], [], undefined);
      check('spec: spec-default always', hasArg(r.primary, '--spec-default'));
      check('spec: no mtp flags on non-mtp', !hasArg(r.primary, 'draft-mtp'));

      const mtp = { ...denseModel(), has_mtp: true };
      const sweep = [mtpPoint('vulkan', 1, 41), mtpPoint('vulkan', 2, 55), mtpPoint('vulkan', 3, 43), mtpPoint('vulkan', 4, 44)];
      r = synthesize(igpuHw(), mtp, answers(), [fitting('vulkan', 65536)], sweep, undefined);
      check('spec: measured argmax n=2 (highest tps)', hasArg(r.primary, '--spec-draft-n-max 2'));

      r = synthesize(igpuHw(), mtp, answers(), [fitting('vulkan', 65536)], [], undefined);
      check('spec: default n=3 unmeasured', hasArg(r.primary, '--spec-draft-n-max 3'));
    }

    // ── lever: backend duel by TTFT/TPS ───────────────────────────────
    {
      // vulkan: higher tps AND lower ttft → wins.
      const duel = [
        duelPoint('vulkan', 0, 90, 42), duelPoint('vulkan', 30000, 300, 40),
        duelPoint('rocm-stable', 0, 120, 37), duelPoint('rocm-stable', 30000, 380, 35),
      ];
      const r = synthesize(igpuHw(), denseModel(), answers(),
        [fitting('vulkan', 65536), fitting('rocm-stable', 65536)], duel, undefined);
      check('duel: measured winner (better tps+ttft)', r.primary.llamacpp_backend === 'vulkan');
      check('duel: expected carries ttft/tps',
        r.primary.expected && r.primary.expected.tps > 0 && r.primary.expected.ttft_ms > 0);

      const rh = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], [], undefined);
      check('duel: heuristic fallback picks installed', rh.primary.llamacpp_backend === 'vulkan');
    }

    // ── lever: sampling passthrough ───────────────────────────────────
    {
      const sd = { temperature: 0.7, top_p: 0.8, top_k: 20, source: 'hf:Qwen/Qwen3-32B/generation_config.json' };
      const r = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], [], sd);
      check('sampling: passthrough', r.sampling_defaults && r.sampling_defaults.temperature === 0.7);
      check('sampling: not in args', !hasArg(r.primary, 'temp'));
    }

    // ── lever: batch ladder by LOWEST ttft ────────────────────────────
    {
      // b=2048 has the lowest TTFT (fastest prefill) and clears the 5% gate.
      const ladder = [ladderPoint('vulkan', 512, 100), ladderPoint('vulkan', 2048, 70), ladderPoint('vulkan', 8192, 80)];
      const r = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], ladder, undefined);
      check('ladder: lowest-ttft rung chosen', hasArg(r.primary, '-b 2048 -ub 2048'));

      // Not enough improvement (all ~equal) → no ladder flag.
      const flat = [ladderPoint('vulkan', 512, 100), ladderPoint('vulkan', 2048, 99), ladderPoint('vulkan', 8192, 98)];
      const rf = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], flat, undefined);
      check('ladder: no flag without meaningful gain', !hasArg(rf.primary, '-b 2048'));

      // iGPU heuristic without any bench data.
      const rq = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 65536)], [], undefined);
      check('ladder: iGPU heuristic without bench', hasArg(rq.primary, '-b 2048 -ub 2048'));

      const rd = synthesize(dgpuHw(), denseModel(), answers(), [fitting('cuda', 65536)], [], undefined);
      check('ladder: no heuristic on dGPU', !hasArg(rd.primary, '-b '));
    }

    // ── lever: ROCm direct-io ─────────────────────────────────────────
    {
      let r = synthesize(igpuHw(), denseModel(), answers({ backends_to_consider: ['rocm-stable'] }),
        [fitting('rocm-stable', 65536)], [], undefined);
      check('rocm: gets --direct-io', hasArg(r.primary, '--direct-io'));

      r = synthesize(igpuHw(), denseModel(), answers({ backends_to_consider: ['vulkan'] }),
        [fitting('vulkan', 65536)], [], undefined);
      check('rocm: vulkan does not', !hasArg(r.primary, '--direct-io'));
    }

    // ── lever: cpu-moe fit strategy + conservative alternative ────────
    {
      const moe = { ...denseModel(), is_moe: true, expert_count: 128 };
      const f = fitting('vulkan', 32768, { fits_fully: false, fitted_ngl: -1, fitted_ncmoe: 12, fitted_ctx: 4096 });
      const r = synthesize(igpuHw(), moe, answers(), [f], [], undefined);
      check('moe: n-cpu-moe from fit', hasArg(r.primary, '--n-cpu-moe 12'));
      check('moe: conservative alternative',
        r.alternatives.some(a => a.llamacpp_args.includes('--cpu-moe')));
    }

    // ── lever: ctx rounded to fit / capped at trained window ──────────
    {
      const r = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 40000)], [], undefined);
      check('ctx: rounded down to fit', r.primary.ctx_size === 32768);

      const rmax = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 0)], [], undefined);
      check('ctx: full-window fit uses trained window', rmax.primary.ctx_size === 131072);
    }

    // ── review #1: available-memory unit (GiB→MiB after the floor) ────
    {
      const unified = (gb) => availableMib({
        gpus: [{ vendor: 'amd', name: '', family: '', vram_gb: gb }],
        has_igpu: true, ram_is_vram: true, host_ram_gb: gb,
        installed_backends: ['vulkan'], os: 'linux',
      });
      check('mem: 16 GB unified ≈ 0.9x in MiB', Math.abs(unified(16) - 16 * 0.9 * 1024) < 1);
      check('mem: 64 GB unified ≈ 0.9x in MiB', Math.abs(unified(64) - 64 * 0.9 * 1024) < 1);
      check('mem: 128 GB unified ≈ 0.9x in MiB', Math.abs(unified(128) - 128 * 0.9 * 1024) < 1);
      // Regression: the old `max(1024, gb*0.9)*1024` treated a 64 GB box as ~1 TiB.
      check('mem: 64 GB not treated as ~1 TiB', unified(64) < 128 * 1024);
      check('mem: 16 GB stays under 20 GiB', unified(16) < 20 * 1024);

      const cpuOnlyAvail = (gb) => availableMib({
        gpus: [], has_igpu: false, ram_is_vram: false, host_ram_gb: gb,
        installed_backends: ['cpu'], os: 'linux',
      });
      check('mem: cpu 64 GB ≈ 0.7x in MiB', Math.abs(cpuOnlyAvail(64) - 64 * 0.7 * 1024) < 1);
      check('mem: cpu 64 GB stays under host RAM', cpuOnlyAvail(64) < 64 * 1024);

      // A 16 GB unified box cannot fit an 18 GB model fully — it must offload with a finite ctx.
      const fit16 = computeFitEstimate({
        backend: 'vulkan', availableMib: unified(16), weightsMib: 18000,
        kvBytesPerToken: 90112, blockCount: 36, isMoe: false, nCtxTrain: 131072, degraded: false,
      });
      check('fit: 16 GB box offloads an 18 GB model',
        !fit16.fits_fully && fit16.fitted_ngl >= 0 && fit16.fitted_ngl < 36
        && fit16.fitted_ctx > 0 && Number.isFinite(fit16.fitted_ctx));
      // The same model on a 128 GB box fits fully.
      const fit128 = computeFitEstimate({
        backend: 'vulkan', availableMib: unified(128), weightsMib: 18000,
        kvBytesPerToken: 90112, blockCount: 36, isMoe: false, nCtxTrain: 32768, degraded: false,
      });
      check('fit: 128 GB box fits the same model fully', fit128.fits_fully);
    }

    // ── review #3: one candidate list; CPU-only rejected ──────────────
    {
      check('cand: gpu backends selected',
        selectCandidates(igpuHw(), answers()).join(',') === 'vulkan,rocm-stable');
      check('cand: metal kept as a gpu backend',
        selectCandidates({ ...igpuHw(), installed_backends: ['metal', 'vulkan'] }, answers()).join(',') === 'metal,vulkan');
      const cpuOnly = {
        gpus: [], has_igpu: false, ram_is_vram: false, host_ram_gb: 32,
        installed_backends: ['system', 'cpu'], os: 'linux',
      };
      check('cand: cpu-only yields no candidates', selectCandidates(cpuOnly, answers()).length === 0);
      let threw = '';
      try {
        synthesize(cpuOnly, denseModel(), answers(), [], [], undefined);
      } catch (e) {
        threw = String((e && e.message) || e);
      }
      check('cand: synthesize rejects cpu-only with the shared message', threw === NO_GPU_BACKEND_ERROR);
    }

    // ── review #2: recommendation capped to the ctx that actually loaded ─
    {
      const capPoint = {
        backend: 'vulkan', label: 'vulkan · d0', ctx_size: 2048, llamacpp_args: '',
        params: { d: 0 }, ttft_ms: 95, tps: 40, vram_gb: 20, ok: true, max_loaded_ctx: 2048,
      };
      const capped = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 0)], [capPoint], undefined);
      check('fallback-cap: ctx capped to the loaded 2048', capped.primary.ctx_size === 2048);
      check('fallback-cap: rationale explains the cap',
        capped.primary.rationale.some(s => s.includes('largest context that actually loaded')));

      const okPoint = { ...capPoint, ctx_size: 131072 };
      delete okPoint.max_loaded_ctx;
      const uncapped = synthesize(igpuHw(), denseModel(), answers(), [fitting('vulkan', 0)], [okPoint], undefined);
      check('fallback-cap: full window kept without a ceiling', uncapped.primary.ctx_size === 131072);
    }

    // ── review #1: the bench validates the ctx it will recommend (kv quant) ──
    {
      const recipeBundle = await bundleRecipe();
      try {
        const { buildBenchRecipe } = require(recipeBundle.modulePath);
        // Partial ctx fit: heuristic fitted_ctx is an f16 estimate; q8_0 doubles the
        // recommended window, so the benched load must also target the doubled ctx.
        const fit = fitting('vulkan', 8000, { fits_fully: true, fitted_ctx: 8000 });
        const mf = { ...denseModel(), n_ctx_train: 131072 };
        const ans = answers({ kv_cache_quant: 'q8_0' });

        const rec = synthesize(igpuHw(), mf, ans, [fit], [], undefined);
        const recipe = buildBenchRecipe([fit], ans, igpuHw(), mf, 'org/m', 'standard', ['vulkan']);
        const load0 = recipe.steps.find(s => s.id === 'load_vulkan_d0');
        const benchedCtx = load0 && load0.params && load0.params.ctx_size;

        check('#1: benched depth-0 ctx == recommended ctx', benchedCtx === rec.primary.ctx_size);
        check('#1: benched ctx is quant-scaled, not raw fitted_ctx', benchedCtx > 8000);
        check('#1: recommended ctx is the quant-scaled window', rec.primary.ctx_size === 12288);
        check('#1: depth-0 load carries -ctk quant', String(load0.params.llamacpp_args).includes('-ctk q8_0'));

        const probe = recipe.plan.find(e => e.backend === 'vulkan' && e.params && e.params.d === 0);
        check('#1: depth-0 plan entry is the ctx probe with a fallback',
          probe && probe.ctx_probe === true && probe.fallback_ctx_size === 2048);

        // If that quant-scaled load fails and only the fallback loads, cap the recommendation.
        const fellBack = {
          backend: 'vulkan', label: 'vulkan · d0', ctx_size: 2048, llamacpp_args: '-ctk q8_0 -ctv q8_0',
          params: { d: 0 }, ttft_ms: 90, tps: 40, vram_gb: 20, ok: true, max_loaded_ctx: 2048,
        };
        const capped = synthesize(igpuHw(), mf, ans, [fit], [fellBack], undefined);
        check('#1: failed quant-scaled load falls back to the loaded ctx', capped.primary.ctx_size === 2048);

        // ── round-2 #2: the bench loads the SAME placement flags that get recommended ──
        const partialFit = fitting('vulkan', 4096, { fits_fully: false, fitted_ngl: 18, fitted_ncmoe: 0, fitted_ctx: 4096 });
        const ansNo = answers({ kv_cache_quant: 'none' });
        const recPartial = synthesize(igpuHw(), denseModel(), ansNo, [partialFit], [], undefined);
        const recipePartial = buildBenchRecipe([partialFit], ansNo, igpuHw(), denseModel(), 'org/m', 'standard', ['vulkan']);
        const load0p = recipePartial.steps.find(s => s.id === 'load_vulkan_d0');
        check('r2#2: recommendation includes -ngl placement', recPartial.primary.llamacpp_args.includes('-ngl 18'));
        check('r2#2: benched load carries the same -ngl placement', String(load0p.params.llamacpp_args).includes('-ngl 18'));

        const moeFit = fitting('vulkan', 4096, { fits_fully: false, fitted_ngl: -1, fitted_ncmoe: 10, fitted_ctx: 4096 });
        const moeModel = { ...denseModel(), is_moe: true, expert_count: 128 };
        const recMoe = synthesize(igpuHw(), moeModel, ansNo, [moeFit], [], undefined);
        const recipeMoe = buildBenchRecipe([moeFit], ansNo, igpuHw(), moeModel, 'org/m', 'standard', ['vulkan']);
        const load0m = recipeMoe.steps.find(s => s.id === 'load_vulkan_d0');
        check('r2#2: recommendation includes --n-cpu-moe placement', recMoe.primary.llamacpp_args.includes('--n-cpu-moe 10'));
        check('r2#2: benched load carries the same --n-cpu-moe placement', String(load0m.params.llamacpp_args).includes('--n-cpu-moe 10'));

        // Per-backend load args: rocm gets --direct-io in BOTH the bench and the recommendation.
        check('r2#2: shared builder emits rocm --direct-io',
          backendLoadArgs(igpuHw(), 'rocm-stable', partialFit, denseModel(), 'none').includes('--direct-io'));

        // ── round-3 #2: the deep probe loads at the recommended ctx, not a larger one ──
        const fitMid = fitting('vulkan', 8192, { fits_fully: true, fitted_ctx: 8192 });
        const recipeMid = buildBenchRecipe([fitMid], ansNo, igpuHw(), denseModel(), 'org/m', 'standard', ['vulkan']);
        const recMid = synthesize(igpuHw(), denseModel(), ansNo, [fitMid], [], undefined);
        const deepStep = recipeMid.steps.find(s => /^load_vulkan_d\d+$/.test(s.id) && s.id !== 'load_vulkan_d0');
        const deepPlan = recipeMid.plan.find(e => (e.params.d ?? 0) > 0 && e.backend === 'vulkan');
        check('r3#2: a deep probe exists', !!deepStep && !!deepPlan);
        check('r3#2: deep probe loads at the recommended ctx, not larger',
          deepStep.params.ctx_size === recMid.primary.ctx_size);
        check('r3#2: deep prompt depth fits inside the recommended ctx',
          deepPlan.params.d < recMid.primary.ctx_size);
      } finally {
        fs.rmSync(recipeBundle.outputPath, { recursive: true, force: true });
      }
    }

    // ── round-2 #1: memory is accounted PER backend / device group ────
    {
      const mixed = {
        gpus: [
          { vendor: 'amd', name: 'APU', family: 'gfx1151', vram_gb: 100 },
          { vendor: 'nvidia', name: 'RTX 4090', family: 'sm_89', vram_gb: 24 },
        ],
        has_igpu: true, ram_is_vram: false, host_ram_gb: 110,
        installed_backends: ['vulkan', 'rocm', 'cuda'], os: 'linux',
      };
      check('r2#1: cuda fitted vs NVIDIA VRAM', Math.abs(availableMibForBackend(mixed, 'cuda') - 24 * 0.92 * 1024) < 1);
      check('r2#1: rocm fitted vs AMD budget', Math.abs(availableMibForBackend(mixed, 'rocm') - 100 * 0.92 * 1024) < 1);
      check('r2#1: cuda budget differs from rocm budget on a mixed box',
        availableMibForBackend(mixed, 'cuda') !== availableMibForBackend(mixed, 'rocm'));

      // round-3 #3: vulkan is budgeted conservatively (smallest pool) on a mixed-vendor box.
      check('r3#3: vulkan uses the conservative (smaller) pool on a mixed box',
        Math.abs(availableMibForBackend(mixed, 'vulkan') - 24 * 0.92 * 1024) < 1);
      check('r3#3: vulkan budget is smaller than the AMD rocm budget on a mixed box',
        availableMibForBackend(mixed, 'vulkan') < availableMibForBackend(mixed, 'rocm'));

      const apu = { gpus: [{ vendor: 'amd', name: 'APU', family: 'gfx1151', vram_gb: 96 }], has_igpu: true, ram_is_vram: true, host_ram_gb: 128, installed_backends: ['rocm', 'vulkan'], os: 'linux' };
      check('r2#1: unified APU rocm uses 0.9x host RAM', Math.abs(availableMibForBackend(apu, 'rocm') - 128 * 0.9 * 1024) < 1);
      check('r3#3: single-vendor APU vulkan matches rocm (AMD unified)',
        availableMibForBackend(apu, 'vulkan') === availableMibForBackend(apu, 'rocm'));
    }

    // ── round-2 #3: a backend that FAILS the deep run cannot win the duel ──
    {
      const dp = (backend, depth, ok, tps) => ({
        backend, label: `${backend} d${depth}`, ctx_size: depth > 0 ? 32768 : 4096, llamacpp_args: '',
        params: { d: depth }, ttft_ms: ok ? 100 : 0, tps: ok ? tps : 0, vram_gb: ok ? 20 : -1, ok,
      });
      // vulkan has a HIGHER depth-0 tps but FAILS the deep (target-context) run;
      // rocm-stable is slower at depth 0 but succeeds at depth. rocm must win.
      const bench = [
        dp('vulkan', 0, true, 99),
        dp('vulkan', 30000, false, 0),
        dp('rocm-stable', 0, true, 40),
        dp('rocm-stable', 30000, true, 38),
      ];
      const r = synthesize(igpuHw(), denseModel(), answers(),
        [fitting('vulkan', 65536), fitting('rocm-stable', 65536)], bench, undefined);
      check('r2#3: deep-run failure disqualifies the faster-at-d0 backend', r.primary.llamacpp_backend === 'rocm-stable');

      // When neither backend has a deep run (small ctx), depth-0 comparison stands.
      const shallow = [dp('vulkan', 0, true, 99), dp('rocm-stable', 0, true, 40)];
      const rs = synthesize(igpuHw(), denseModel(), answers(),
        [fitting('vulkan', 65536), fitting('rocm-stable', 65536)], shallow, undefined);
      check('r2#3: no deep runs → depth-0 winner stands', rs.primary.llamacpp_backend === 'vulkan');

      // round-3 #2 (synthesize): a backend whose deep run SUCCEEDS is not disqualified and wins.
      const okDeep = [
        dp('vulkan', 0, true, 90), dp('vulkan', 6553, true, 85),
        dp('rocm-stable', 0, true, 40), dp('rocm-stable', 6553, true, 38),
      ];
      const rOk = synthesize(igpuHw(), denseModel(), answers(),
        [fitting('vulkan', 8192), fitting('rocm-stable', 8192)], okDeep, undefined);
      check('r3#2: backend that runs the recommended-ctx deep run wins', rOk.primary.llamacpp_backend === 'vulkan');
    }

    // ── round-3 #1: MTP/ladder tuning comes from the DUEL WINNER, not candidates[0] ──
    {
      const mk = (backend, extra) => ({
        backend, label: backend, ctx_size: 4096, llamacpp_args: '',
        ttft_ms: 100, tps: 40, vram_gb: 20, ok: true, params: { d: 0 }, ...extra,
      });
      const bench = [
        mk('vulkan', { tps: 40 }),
        mk('rocm-stable', { tps: 80 }),
        mk('vulkan', { params: { spec_n: 2 }, tps: 200 }),
        mk('vulkan', { params: { spec_n: 3 }, tps: 40 }),
        mk('rocm-stable', { params: { spec_n: 5 }, tps: 95 }),
        mk('rocm-stable', { params: { spec_n: 2 }, tps: 30 }),
        mk('vulkan', { params: { ladder: true, b: 512, ub: 512 }, ttft_ms: 100 }),
        mk('vulkan', { params: { ladder: true, b: 2048, ub: 2048 }, ttft_ms: 70 }),
        mk('rocm-stable', { params: { ladder: true, b: 512, ub: 512 }, ttft_ms: 100 }),
        mk('rocm-stable', { params: { ladder: true, b: 4096, ub: 4096 }, ttft_ms: 55 }),
      ];
      const mtpModel = { ...denseModel(), has_mtp: true };
      const r = synthesize(igpuHw(), mtpModel, answers(),
        [fitting('vulkan', 65536), fitting('rocm-stable', 65536)], bench, undefined);
      check('r3#1: duel winner is rocm-stable (not candidates[0])', r.primary.llamacpp_backend === 'rocm-stable');
      check('r3#1: MTP draft length comes from the winner (n=5)', r.primary.llamacpp_args.includes('--spec-draft-n-max 5'));
      check('r3#1: MTP not taken from candidates[0] (n=2)', !r.primary.llamacpp_args.includes('--spec-draft-n-max 2'));
      check('r3#1: batch size comes from the winner (b=4096)', r.primary.llamacpp_args.includes('-b 4096 -ub 4096'));
      check('r3#1: batch not taken from candidates[0] (b=2048)', !r.primary.llamacpp_args.includes('-b 2048'));
    }

    if (failures > 0) {
      throw new Error(`${failures} synthesis checks failed`);
    }
    console.log('\nAutoOpt synthesis runtime tests passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
