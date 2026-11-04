const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundleMetadata() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-recipe-options-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/features/backends/recipeMetadata.ts'),
    output: { path: outputPath, filename: 'recipeMetadata.cjs', library: { type: 'commonjs2' } },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: { rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }] },
    optimization: { minimize: false },
  };
  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });
  return { outputPath, modulePath: path.join(outputPath, 'recipeMetadata.cjs') };
}

function option(name, extra = {}) {
  return {
    name,
    cli_flag: '',
    default: 0,
    type_name: 'SIZE',
    help: `help for ${name}`,
    group: 'Stable Diffusion Options',
    user_configurable: true,
    ...extra,
  };
}

function systemInfoWith(options, selectableBackend = false) {
  return { recipes: { 'sd-cpp': { selectable_backend: selectableBackend, options } } };
}

(async () => {
  const { outputPath, modulePath } = await bundleMetadata();
  try {
    const { recipeOptions, recipeConfigurableOptionNames } = require(modulePath);

    const imageOptions = [
      option('steps'),
      option('cfg_scale'),
      option('sampling_method', { type_name: 'ARGS' }),
      option('soft_pixel_cap', { user_configurable: false }),
    ];
    const systemInfo = systemInfoWith(imageOptions);

    // The flag survives the /system-info -> metadata mapping, and a server that
    // predates it leaves every option configurable.
    const mapped = Object.fromEntries(recipeOptions(systemInfo, 'sd-cpp').map(o => [o.name, o.userConfigurable]));
    assert.deepEqual(mapped, { steps: true, cfg_scale: true, sampling_method: true, soft_pixel_cap: false });
    const legacy = systemInfoWith([{ name: 'steps', cli_flag: '', default: 20, type_name: 'SIZE', help: '', group: '' }]);
    assert.equal(recipeOptions(legacy, 'sd-cpp')[0].userConfigurable, true,
      'an option without user_configurable must stay configurable');

    // A value that describes the checkpoint is not offered as a control.
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'sd-cpp'),
      ['steps', 'cfg_scale', 'sampling_method']);

    // The auto-created backend option is still listed; hiding options must not
    // disturb the rest of the order.
    assert.deepEqual(
      recipeConfigurableOptionNames(systemInfoWith(imageOptions, true), 'sd-cpp'),
      ['sd-cpp_backend', 'steps', 'cfg_scale', 'sampling_method'],
    );
    assert.deepEqual(
      recipeConfigurableOptionNames(systemInfoWith([...imageOptions, option('sd-cpp_backend', { type_name: 'BACKEND' })], true), 'sd-cpp'),
      ['steps', 'cfg_scale', 'sampling_method', 'sd-cpp_backend'],
      'a declared backend option is listed once, in descriptor order');

    // Hiding a saved value would leave it with no way to be seen or cleared, so
    // a saved key is listed whether the recipe withholds the control or no longer
    // declares the option at all.
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'sd-cpp', ['soft_pixel_cap']),
      ['steps', 'cfg_scale', 'sampling_method', 'soft_pixel_cap']);
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'sd-cpp', ['steps']),
      ['steps', 'cfg_scale', 'sampling_method'],
      'a saved key the recipe already lists is not duplicated');
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'sd-cpp', ['retired_option']),
      ['steps', 'cfg_scale', 'sampling_method', 'retired_option']);
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'sd-cpp', ['', null]),
      ['steps', 'cfg_scale', 'sampling_method'],
      'blank saved keys list nothing');

    // A recipe the server does not surface lists only what the user saved.
    assert.deepEqual(recipeConfigurableOptionNames(systemInfo, 'no-such-recipe', ['soft_pixel_cap']),
      ['soft_pixel_cap']);

    // The detail panel must route its field list through the rule rather than
    // through every descriptor option, and must hand it what the user saved.
    const panel = fs.readFileSync(path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx'), 'utf8');
    assert.match(panel, /recipeConfigurableOptionNames\(info, recipe, savedKeys\)/,
      'the tuning field list must come from recipeConfigurableOptionNames');
    assert.doesNotMatch(panel, /recipeOptionNames\(/,
      'the tuning field list must not fall back to raw descriptor names');
    assert.match(panel, /tuningKeysForModel\(model, systemInfo, Object\.keys\(serverSavedRecipeOptions\)\)/,
      'saved options must reach the tuning field list');
    assert.match(panel, /tuningKeysForModel\(model, systemInfo, Object\.keys\(saved\)\)/,
      'the draft must be seeded from the key list the fetched saved options produce');
    assert.doesNotMatch(panel, /draftFromResolvedOptions\(recipeKeys, saved[),]/,
      'the seed must not use the key list captured before the fetch, which misses a saved key the fetch reveals');

    console.log('Lemonade App non-configurable recipe option checks passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
