const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/features/monitor/throughput.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  fileName: filename,
  reportDiagnostics: true,
});
assert.deepEqual(
  (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error),
  [],
  'monitor throughput helpers must compile',
);

// The import is type-only and disappears during transpilation.
const exported = {};
Function('exports', 'require', compiled.outputText)(exported, require);
const {
  decodedTokensInSlot, getCacheTokenCount, isSlotProcessing, percentDigits, sampledSlotTps, selectGenerationTps,
} = exported;

// Older schemas, then the current one that only reports n_prompt_tokens.
assert.equal(getCacheTokenCount({ n_cache_tokens: 12, n_prompt_tokens: 99 }), 12);
assert.equal(getCacheTokenCount({ cache_tokens: 13, n_prompt_tokens: 99 }), 13);
assert.equal(getCacheTokenCount({ cache_tokens: [1, 2, 3], n_prompt_tokens: 99 }), 3);
assert.equal(getCacheTokenCount({ n_prompt_tokens: 617, n_prompt_tokens_cache: 0 }), 617);
assert.equal(getCacheTokenCount({ id: 0, n_ctx: 262144 }), 0, 'a fresh slot reports no prompt fields');
assert.equal(getCacheTokenCount({ id: 0, n_ctx: 262144, is_processing: false, n_prompt_tokens: 2122 }),
  2122, 'Strata prompt-token telemetry remains visible while idle');

assert.equal(percentDigits(0), '0');
assert.equal(percentDigits(0.2), '<1');
assert.equal(percentDigits(1), '1');
assert.equal(percentDigits(42.6), '43');

// Old llama.cpp, documented newer schema, and latest upstream array schema.
assert.equal(decodedTokensInSlot({ n_decoded: 18 }), 18);
assert.equal(decodedTokensInSlot({ next_token: { n_decoded: 23 } }), 23);
assert.equal(decodedTokensInSlot({ next_token: [{ n_decoded: 31 }] }), 31);
assert.equal(decodedTokensInSlot({ next_token: [] }), 0);
assert.equal(decodedTokensInSlot({ n_decoded: -2 }), 0);

const activeStats = {
  live: { active_requests: 1, estimated_tokens_per_second: 47 },
  tokens_per_second: 65,
  output_tokens: 35,
};
assert.equal(selectGenerationTps(36, activeStats, false, true), 36,
  'actual slot deltas are preferred to estimates');
assert.equal(selectGenerationTps(0, activeStats, false, true), 47,
  'a missing first slot delta must not blank the graph');
assert.equal(selectGenerationTps(0, activeStats, true, true), 47,
  'ongoing inference remains the authoritative graph sample');

const idleStats = {
  live: { active_requests: 0, estimated_tokens_per_second: 47 },
  tokens_per_second: 65,
  output_tokens: 35,
};
assert.equal(selectGenerationTps(0, idleStats, true, true), 65,
  'a short, completed request must be graphed once');
assert.equal(selectGenerationTps(0, idleStats, false, true), 0,
  'stale last-request TPS must never look live');
assert.equal(selectGenerationTps(0, { ...idleStats, output_tokens: 0 }, true, true), 0,
  'a completion with no generated tokens is not a throughput sample');
assert.equal(selectGenerationTps(0, {
  ...idleStats,
  live: { active_requests: 0, estimated_tokens_per_second: Infinity },
}, false, true), 0, 'invalid live rates must be discarded');
assert.equal(selectGenerationTps(0, null, false, true), 0);

// Idle slots must ignore stale count deltas and stop immediately.
assert.equal(isSlotProcessing({ is_processing: true, state: 1 }, 0), true);
assert.equal(isSlotProcessing({ is_processing: false, state: 1 }, 26), false);
assert.equal(isSlotProcessing({ state: 'processing' }, 0), true);
assert.equal(isSlotProcessing({ state: 'idle' }, 26), false);
assert.equal(isSlotProcessing({ state: 3 }, 26), true,
  'unknown slot states preserve the legacy delta-based activity fallback');
assert.equal(sampledSlotTps(26, false), 0,
  'an idle slot must drop immediately');
assert.equal(sampledSlotTps(26, true), 26,
  'active slots must show the measured TPS without artificial ramp-up');
assert.equal(sampledSlotTps(0.02, true), 0,
  'near-zero active samples remain clamped');
assert.deepEqual([47, 47, 47, 47, 0].map((value, i) =>
  sampledSlotTps(value, i < 4)), [47, 47, 47, 47, 0],
  'steady throughput followed by idle must have no artificial sawtooth');

// FastFlowLM does not expose /slots. Keep the non-slot live stats path,
// including its existing behavior when the active request flag is missing.
assert.equal(selectGenerationTps(90, activeStats, false, false), 47,
  'non-slot backends must prefer server live telemetry over slot data');
assert.equal(selectGenerationTps(0, idleStats, false, false), 47,
  'non-slot telemetry must not introduce an active_requests requirement');
const quietStats = {
  ...idleStats,
  live: { active_requests: 0, estimated_tokens_per_second: 0 },
};
assert.equal(selectGenerationTps(0, quietStats, false, false), 0,
  'an idle FLM backend without live output must read zero');
assert.equal(selectGenerationTps(0, quietStats, true, false), 65,
  'a completed FLM request may be sampled once');

// Protect the backend routing contract: FLM must not trigger a /slots request.
const hookSource = fs.readFileSync(path.resolve(__dirname, '../src/hooks/useMonitorData.ts'), 'utf8');
const slotsGate = hookSource.match(/const supportsSlots = (Boolean\([\s\S]*?\n\s*\));/);
assert.ok(slotsGate, 'monitor slot capability gate must be present');
const supportsSlots = new Function('activeModel', `return ${slotsGate[1]};`);
for (const recipe of ['llamacpp', 'vllm', 'strata']) {
  assert.equal(supportsSlots({ recipe }), true, `${recipe} must poll slots`);
}
for (const model of [undefined, null, { recipe: 'flm' }, { recipe: 'whispercpp' }]) {
  assert.equal(supportsSlots(model), false, 'unloaded and incompatible backends must not poll slots');
}
assert.match(hookSource, /if \(supportsSlots\) \{[\s\S]*?const response = await api\.slots\(\)/);
assert.match(hookSource, /selectGenerationTps\(aggTps, st, newCompletion, supportsSlots\)/);
assert.match(hookSource, /slotActive = isSlotProcessing\(s, slotTps\)/);
assert.match(hookSource, /slotTps = 0;[\s\S]*?winMap\.set\(s\.id, samples\.slice\(-1\)\)/);
assert.match(hookSource, /sampledSlotTps\(target\.tps, target\.isActive\)/);
assert.doesNotMatch(hookSource, /finishedSlot\.tps = 0/,
  'log events must not zero out active slots between /slots snapshots');
assert.match(hookSource, /if \(currDecoded > 0\) slotPpTps = 0;/,
  'prefill should stop when decoding starts');
assert.match(hookSource, /aI\.ppTps = aTarget\.ppTps/,
  'prompt samples should not exhibit an exponential release tail');

console.log('Monitor throughput regression checks passed.');
