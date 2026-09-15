for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.resolve(__dirname, '..', '..', '..', 'src', 'app');
let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  module.exports = {
    tests: [{
      name: 'mobile store redirect suite',
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
      esModuleInterop: true,
      module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs,
      target: ts.ScriptTarget.ES2020,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const { getMobileStoreUrl } = require(
  path.join(appRoot, 'src', 'renderer', 'utils', 'mobileStoreRedirect.ts'),
);

if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
else delete require.extensions['.ts'];

const tests = [
  {
    name: 'Tauri desktop ignores WebKitGTK Android compatibility token',
    run() {
      const userAgent =
        'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15 Linux; like Android 4.4';
      assert.equal(getMobileStoreUrl(userAgent, true, false), null);
    },
  },
  {
    name: 'Android browser redirects to Google Play',
    run() {
      assert.equal(
        getMobileStoreUrl('Mozilla/5.0 (Linux; Android 15; Pixel 9)', false, false),
        'https://play.google.com/store/apps/details?id=com.lemonade.mobile.chat.ai',
      );
    },
  },
  {
    name: 'iPhone browser redirects to App Store',
    run() {
      assert.equal(
        getMobileStoreUrl('Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X)', false, false),
        'https://apps.apple.com/ca/app/lemonade-mobile/id6757372210',
      );
    },
  },
  {
    name: 'desktop browser does not redirect',
    run() {
      assert.equal(getMobileStoreUrl('Mozilla/5.0 (X11; Linux x86_64)', false, false), null);
    },
  },
];

module.exports = { tests };
