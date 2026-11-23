const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const helperPath = path.join(root, 'src/features/chat/contextUsage.ts');
const meterPath = path.join(root, 'src/components/ContextMeter.tsx');

function compile(file, jsx = false) {
  const source = fs.readFileSync(file, 'utf8');
  const output = ts.transpileModule(source, {
    fileName: file,
    reportDiagnostics: true,
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      ...(jsx ? { jsx: ts.JsxEmit.React } : {}),
      esModuleInterop: true,
    },
  });
  const errors = (output.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.deepEqual(errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')), [], file);
  return { source, compiled: output.outputText };
}

const { compiled } = compile(helperPath);
compile(meterPath, true);

const moduleRef = { exports: {} };
new Function('module', 'exports', compiled)(moduleRef, moduleRef.exports);
const { newChatTokenCounts, collectChatTokenCounts, buildChatContextUsage } = moduleRef.exports;

// llama.cpp final timings: the cache is a *part* of the input, not extra input.
const t = newChatTokenCounts();
collectChatTokenCounts(t, { timings: { prompt_n: 7, cache_n: 93, predicted_n: 20 } });
assert.equal(t.timingPrompt, 100);
assert.equal(t.timingCompletion, 20);
assert.equal(t.timingCache, 93);

// A usage-only chunk can arrive before or after timings. Explicit usage wins.
collectChatTokenCounts(t, {
  choices: [],
  usage: { prompt_tokens: 125, completion_tokens: 22, prompt_tokens_details: { cached_tokens: 104 } },
});
collectChatTokenCounts(t, { timings: { prompt_n: 8, cache_n: 95, predicted_n: 19 } });
assert.equal(t.usagePrompt ?? t.timingPrompt, 125);
assert.equal(t.usageCompletion ?? t.timingCompletion, 22);
assert.equal(t.usageCache ?? t.timingCache, 104);

const messages = [
  { role: 'system', content: 'Follow the instructions and be helpful.' },
  { role: 'user', content: 'hello how are you' },
  { role: 'assistant', content: 'I am fine' },
  { role: 'tool', content: 'Weather: sunny' },
];
const tools = [{ type: 'function', function: { name: 'weather', parameters: { type: 'object' } } }];
const context = buildChatContextUsage(messages, tools, 125, 22, 104);
assert.equal(context.promptTokens, 125);
assert.equal(context.completionTokens, 22);
assert.equal(context.cachedTokens, 104);
assert.equal(context.systemTokens + context.toolDefinitionTokens + context.conversationTokens +
  context.toolResultTokens + context.otherTokens, 125, 'estimated slices must add up to measured prompt');
assert.equal(context.promptTokens + context.completionTokens, 147, 'cached tokens must not be added twice');
assert.ok(context.toolDefinitionTokens > 0);
assert.ok(context.toolResultTokens > 0);

// Multimedia data-URLs are not text; their model tokens remain in Other.
const image = buildChatContextUsage([
  { role: 'user', content: [
    { type: 'text', text: 'describe the picture' },
    { type: 'image_url', image_url: { url: `data:image/png;base64,${'a'.repeat(50000)}` } },
  ] },
], [], 1500, 25, 0);
assert.ok(image.otherTokens > 1400, 'image payload must not be counted as text');

// Even when text estimates exceed the backend count (context truncation),
// numeric slices must never be negative or add up to more than the prompt.
const truncated = buildChatContextUsage([
  { role: 'system', content: 'A'.repeat(1000) },
  { role: 'user', content: 'B'.repeat(1000) },
], tools, 14, 3, null);
assert.equal(truncated.systemTokens + truncated.toolDefinitionTokens +
  truncated.conversationTokens + truncated.toolResultTokens + truncated.otherTokens, 14);
assert.ok(Object.values(truncated).filter(v => typeof v === 'number').every(v => v >= 0));

assert.equal(buildChatContextUsage(messages, tools, null, 4, null), null,
  'no backend prompt count means no fabricated utilization');
const partial = buildChatContextUsage(messages, [], 12, null, null);
assert.equal(partial.completionTokens, null, 'unknown answer tokens must not silently become zero');

// Wiring and placement: the button belongs directly before Effective Settings.
const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
assert.match(chat, /<ContextMeter[\s\S]{0,200}model=\{currentLoadedModel\}[\s\S]{0,200}usage=\{displayedContextUsage\}[\s\S]{0,260}\/>[\s\S]{0,90}\{canShowEffectiveSettings && \(/);
assert.match(chat, /message\.model\?\.name\?\.toLowerCase\(\) === currentModel\.toLowerCase\(\)/);
const hook = fs.readFileSync(path.join(root, 'src/hooks/useChatStreaming.ts'), 'utf8');
assert.match(hook, /api\.chatCompletion\([\s\S]{0,190}includeUsage: true/);
const api = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
assert.match(api, /collectChatTokenCounts\(tokenCounts, chunk\)/);
assert.match(api, /body\.stream_options = \{ \.\.\.options, include_usage: true \}/);
const css = fs.readFileSync(path.join(root, 'src/components/ContextMeter.css'), 'utf8');
assert.match(css, /\.composer__tools-toggle\.composer__context-trigger\s*\{[^}]*width: 30px;[^}]*height: 30px;/);

console.log('PASS: Context meter token accounting, multimodal fallback, usage priority and GUI placement');

// Follow-up: estimate tool/system overhead *before* first answer, including
// cases where the estimated overhead already exceeds a small model window.
const { estimateChatContextUsage, withGeneratedContextUsage } = moduleRef.exports;
const early = estimateChatContextUsage([
  { role: 'system', content: 'S'.repeat(400) },
], [{ type: 'function', function: { name: 't', description: 'T'.repeat(8000) } }]);
assert.equal(early.promptEstimated, true);
assert.ok(early.systemTokens > 0);
assert.ok(early.toolDefinitionTokens > 2000, 'preflight exposes oversized tool schemas');
assert.equal(early.completionTokens, null);

// 30k+ output tokens: the live total and thinking breakdown must grow without
// waiting for the final completion's usage. Never treat SSE chunks as tokens.
const progress = withGeneratedContextUsage(early, 120000, 20000, null);
assert.equal(progress.completionTokens, 35000);
assert.equal(progress.thinkingTokens + progress.answerTokens, progress.completionTokens);
assert.ok(progress.thinkingTokens > 0);
assert.equal(progress.completionEstimated, true);

const final = withGeneratedContextUsage(context, 120000, 20000, 34820);
assert.equal(final.completionTokens, 34820, 'backend total overrides live estimate');
assert.equal(final.thinkingTokens + final.answerTokens, 34820,
  'reasoning/answer split must not double-count backend completion tokens');
assert.equal(final.promptTokens, context.promptTokens);
assert.equal(final.cachedTokens, 104);
assert.equal(final.promptEstimated, false);
assert.equal(final.completionEstimated, false);
assert.equal(withGeneratedContextUsage(context, 0, 0, 0).completionTokens, 0);

assert.match(css, /context-meter__stack-highlight/);
assert.match(css, /data-dimmed/);
assert.match(css, /height: 12px/);
const component = fs.readFileSync(meterPath, 'utf8');
assert.match(component, /buildSelectedMcpRuntime/);
assert.match(component, /onMouseEnter=\{\(\) => setHighlighted\(part.id\)\}/);
assert.match(component, /t\('Thinking text'\)/);
const apiSource = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
assert.match(apiSource, /streamedAnswerLength \+= delta.content.length/);
assert.match(apiSource, /streamedThinkingLength \+= delta.reasoning_content.length/);
assert.match(apiSource, /context: currentContext\(\)/);
assert.match(apiSource, /streamedAnswerLength \+= tc.function.arguments.length/);
assert.match(chat, /setLiveError\(t\('Realtime microphone disconnected\.'\)\)/);
console.log('PASS: early baseline, streaming 35K tokens, thinking split, hover highlighting, i18n fix');
