for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') {
    delete process.env[key];
  }
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.resolve(__dirname, '..', '..', 'src', 'app');
let ts;
try {
  ts = require(path.join(appRoot, 'node_modules', 'typescript'));
} catch (_) {
  ts = require('typescript');
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

const appSettingsPath = path.join(appRoot, 'src', 'renderer', 'utils', 'appSettings.ts');
const { mergeWithDefaultSettings } = require(appSettingsPath);

if (originalTsLoader) {
  require.extensions['.ts'] = originalTsLoader;
} else {
  delete require.extensions['.ts'];
}

const tests = [];

function defineTest(name, fn) {
  tests.push({ name, fn });
}

defineTest('mergeWithDefaultSettings preserves light theme', () => {
  const settings = mergeWithDefaultSettings({
    layout: {
      theme: 'light',
    },
  });

  assert.equal(settings.layout.theme, 'light');
});

defineTest('mergeWithDefaultSettings rejects invalid theme', () => {
  const settings = mergeWithDefaultSettings({
    layout: {
      theme: 'neon-pink',
    },
  });

  assert.equal(settings.layout.theme, 'dark');
});

let failures = 0;

for (const { name, fn } of tests) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (error) {
    failures += 1;
    console.error(`not ok - ${name}`);
    console.error(error);
  }
}

if (failures > 0) {
  process.exitCode = 1;
}
