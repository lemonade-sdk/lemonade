// HTTP error UX - app regression tests.
//
// Guards src/app/src/renderer/utils/httpErrors.ts. The LLM chat used to render
// the raw `HTTP error! status: 403` string and then append the selected model's
// backend install command to every failure. When the server refuses a request
// because the browser origin is not allowlisted, that backend hint is
// misleading: the failure has nothing to do with backends. These tests pin the
// classification and the user-facing copy.

for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');

// ── TypeScript loader ──────────────────────────────────────────────────────

let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  module.exports = {
    tests: [{
      name: 'http errors suite',
      run: () => ({ skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" }),
    }],
  };
  return;
}

const originalTsLoader = require.extensions['.ts'];
require.extensions['.ts'] = function loadTypeScript(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      esModuleInterop: true, module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs, target: ts.ScriptTarget.ES2020,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const { HttpError, readHttpError, describeChatError, buildChatErrorInfo, isServerRejection } = require(
  path.join(appRoot, 'src', 'renderer', 'utils', 'httpErrors.ts'),
);

if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
else delete require.extensions['.ts'];

// ── Helpers ────────────────────────────────────────────────────────────────

function response(status, body) {
  return {
    status,
    text: async () => body,
  };
}

const ORIGIN = 'http://192.168.1.50:13305';

const tests = [
  {
    name: 'origin rejection is classified and described with the page origin',
    async run() {
      const error = await readHttpError(
        response(403, JSON.stringify({ error: 'Origin not allowed' })),
      );
      assert.ok(error instanceof HttpError, 'expected an HttpError');
      assert.equal(error.originBlocked, true, '403 Origin not allowed must be flagged as an origin block');

      const info = describeChatError(error, ORIGIN);
      assert.match(info.title, /blocked/i, 'title should say the request was blocked');
      assert.ok(info.detail.includes(ORIGIN), 'detail should name the blocked page origin');
      assert.ok(info.command && info.command.includes(ORIGIN), 'command should include the origin');
      assert.ok(!/install|backends/i.test(info.detail), 'origin error must not carry a backend install hint');
    },
  },
  {
    name: 'origin fix command is runnable and warns against replacing existing origins',
    async run() {
      const error = await readHttpError(response(403, JSON.stringify({ error: 'Origin not allowed' })));
      const info = describeChatError(error, ORIGIN);
      assert.equal(
        info.command,
        `lemonade config set allowed_origins="${ORIGIN}"`,
        'the command should be directly runnable for the default empty allowlist',
      );
      assert.match(info.detail, /already allows other origins/i, 'detail must warn about preserving existing origins');
    },
  },
  {
    name: 'buildChatErrorInfo withholds the backend hint for a blocked origin',
    async run() {
      const error = await readHttpError(response(403, JSON.stringify({ error: 'Origin not allowed' })));
      const info = buildChatErrorInfo(error, ORIGIN, 'lemonade backends install llamacpp:vulkan');
      assert.ok(!info.detail.includes('lemonade backends install'), 'backend hint must be withheld on an origin block');
      assert.equal(info.title, describeChatError(error, ORIGIN).title, 'title should be unchanged');
    },
  },
  {
    name: 'buildChatErrorInfo keeps the backend hint for a real backend failure',
    async run() {
      const error = await readHttpError(response(500, JSON.stringify({ error: { message: 'Backend failed to load' } })));
      const info = buildChatErrorInfo(error, ORIGIN, 'lemonade backends install llamacpp:vulkan');
      assert.ok(info.detail.includes('Backend failed to load'), 'server message should be preserved');
      assert.ok(info.detail.includes('lemonade backends install llamacpp:vulkan'), 'backend hint should be appended');
    },
  },
  {
    name: 'isServerRejection covers auth and origin but not backend failures',
    async run() {
      const origin = await readHttpError(response(403, JSON.stringify({ error: 'Origin not allowed' })));
      const auth = await readHttpError(response(401, JSON.stringify({ error: 'Invalid or missing API key' })));
      const backend = await readHttpError(response(500, JSON.stringify({ error: 'Backend failed to load' })));
      assert.equal(isServerRejection(origin), true, '403 origin must be a server rejection');
      assert.equal(isServerRejection(auth), true, '401 must be a server rejection');
      assert.equal(isServerRejection(backend), false, '500 must not be a server rejection');
      assert.equal(isServerRejection(new Error('backend not installed')), false, 'plain errors are not rejections');
    },
  },
  {
    name: 'origin detail explains the non-browser clients are unaffected',
    async run() {
      const error = await readHttpError(response(403, JSON.stringify({ error: 'Origin not allowed' })));
      const info = describeChatError(error, ORIGIN);
      assert.match(info.detail, /command-line|SDK/i, 'detail should note CLI/SDK clients are unaffected');
      assert.match(info.detail, /reload/i, 'detail should tell the user to reload after fixing');
    },
  },
  {
    name: 'a non-origin 403 is not treated as an origin block',
    async run() {
      const error = await readHttpError(response(403, JSON.stringify({ error: 'Forbidden' })));
      assert.equal(error.originBlocked, false, 'a 403 with a non-origin message must not be an origin block');
      const info = describeChatError(error, ORIGIN);
      assert.equal(info.command, undefined, 'non-origin errors should not suggest an allowed_origins command');
      assert.ok(info.detail.includes('Forbidden'), 'the server message should be preserved');
    },
  },
  {
    name: '401 maps to an authentication message and keeps the server text',
    async run() {
      const error = await readHttpError(
        response(401, JSON.stringify({ error: { message: 'Invalid or missing API key' } })),
      );
      const info = describeChatError(error, ORIGIN);
      assert.match(info.title, /authentication/i, '401 should map to an authentication title');
      assert.ok(info.detail.includes('Invalid or missing API key'), 'nested error.message should be surfaced');
      assert.equal(info.command, undefined, '401 should not suggest an origin command');
    },
  },
  {
    name: 'other HTTP errors use the server message and the status code',
    async run() {
      const error = await readHttpError(
        response(500, JSON.stringify({ error: { message: 'Backend failed to load' } })),
      );
      const info = describeChatError(error, ORIGIN);
      assert.ok(info.title.includes('500'), 'title should carry the status code for unknown failures');
      assert.ok(info.detail.includes('Backend failed to load'), 'server message should be surfaced');
    },
  },
  {
    name: 'plain-text error bodies are preserved',
    async run() {
      const error = await readHttpError(response(502, 'Bad gateway'));
      assert.equal(error.serverMessage, 'Bad gateway', 'non-JSON bodies should be used verbatim');
    },
  },
  {
    name: 'an empty error body still classifies a 403 as an origin block',
    async run() {
      const error = await readHttpError(response(403, ''));
      assert.equal(error.originBlocked, true, 'an empty 403 body should still be treated as an origin block');
    },
  },
  {
    name: 'a network TypeError maps to an unreachable-server message',
    async run() {
      const info = describeChatError(new TypeError('Failed to fetch'), ORIGIN);
      assert.match(info.title, /cannot reach/i, 'network failures should say the server is unreachable');
      assert.equal(info.command, undefined, 'network failures have no fix command');
    },
  },
];

module.exports = { tests };
