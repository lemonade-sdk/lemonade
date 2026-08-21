for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');

let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  module.exports = {
    tests: [{
      name: 'generation parameter suite',
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

const metadata = require(
  path.join(appRoot, 'src', 'renderer', 'utils', 'generationParams.ts'),
);

if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
else delete require.extensions['.ts'];

const systemInfo = {
  recipes: {
    inventedaudio: {
      backends: {},
      generation_params: {
        'audio-generation': [
          {
            name: 'seconds', label: 'Duration', type_name: 'NUMBER', default: 12,
            min: 2, max: 123, step: 1, enum_values: [], help: '', group: '',
            exclusive_group: '', accept: '', random_sentinel: null,
          },
          {
            name: 'seed', label: 'Seed', type_name: 'SEED', default: null,
            min: 0, max: 100, step: 1, enum_values: [], help: '', group: '',
            exclusive_group: '', accept: '', random_sentinel: null,
          },
        ],
      },
    },
    inventedspeech: {
      backends: {},
      generation_params: {
        tts: [{
          name: 'sample', label: 'Clone sample', type_name: 'AUDIO_B64', default: null,
          min: null, max: null, step: null, enum_values: [], help: '', group: '',
          exclusive_group: 'voice', accept: '.wav', random_sentinel: null,
        }],
      },
    },
  },
};

const tests = [
  {
    name: 'model-specific speech/audio defaults survive /models normalization',
    run() {
      const source = fs.readFileSync(
        path.join(appRoot, 'src', 'renderer', 'utils', 'modelData.ts'),
        'utf8',
      );
      assert.ok(source.includes('modelInfo.speech_defaults'));
      assert.ok(source.includes('modelInfo.audio_defaults'));
    },
  },
  {
    name: 'TTS voice modes retain the plain path',
    run() {
      const source = fs.readFileSync(
        path.join(appRoot, 'src', 'renderer', 'components', 'panels', 'TTSPanel.tsx'),
        'utf8',
      );
      assert.ok(source.includes("type VoiceMode = 'plain' | 'describe' | 'clone'"));
      assert.ok(source.includes("voiceMode === 'plain'"));
    },
  },
  {
    name: 'generation controls retain server-declared bounds and defaults',
    run() {
      const params = metadata.generationParams(systemInfo, 'inventedaudio', 'audio-generation');
      assert.equal(params[0].max, 123);
      assert.equal(metadata.generationParamDefault(params[0], {}), 12);
      assert.equal(metadata.generationParamDefault(params[0], { seconds: 24 }), 24);
    },
  },
  {
    name: 'voice cloning is derived from parameter type, not recipe identity',
    run() {
      assert.equal(
        metadata.getTtsVoiceMode(systemInfo, { recipe: 'inventedspeech' }),
        'clone',
      );
      assert.equal(metadata.getTtsVoiceMode(systemInfo, { recipe: 'inventedaudio' }), 'fixed');
    },
  },
  {
    name: 'unsigned seed ranges produce a concrete random seed',
    run() {
      const seed = metadata.generationParams(
        systemInfo,
        'inventedaudio',
        'audio-generation',
      )[1];
      const originalRandom = Math.random;
      Math.random = () => 0.5;
      try {
        assert.equal(metadata.resolveGenerationSeed(seed), 50);
      } finally {
        Math.random = originalRandom;
      }
    },
  },
  {
    name: 'audio and TTS panels contain no backend identity allowlist',
    run() {
      const panels = [
        'AudioGenerationPanel.tsx',
        'TTSPanel.tsx',
      ].map(name => fs.readFileSync(
        path.join(appRoot, 'src', 'renderer', 'components', 'panels', name),
        'utf8',
      )).join('\n').toLowerCase();
      assert.ok(!panels.includes('openmoss'));
      assert.ok(!panels.includes('acestep'));
      assert.ok(panels.includes('generationparams'));
      assert.ok(panels.includes('max={durationmax}'));
    },
  },
];

module.exports = { tests };
