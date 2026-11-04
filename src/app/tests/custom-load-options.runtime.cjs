const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundleStore() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-custom-load-options-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/features/customModels/customModelStore.ts'),
    output: { path: outputPath, filename: 'customModelStore.cjs', library: { type: 'commonjs2' } },
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
  return { outputPath, modulePath: path.join(outputPath, 'customModelStore.cjs') };
}

(async () => {
  const { outputPath, modulePath } = await bundleStore();
  try {
    const { customLoadOptions } = require(modulePath);
    const base = { custom: true, type: 'embedding', max_context_window: 40960 };

    // A saved ctx_size must win over the model's max context window — the GUI
    // load request must not force the full window for custom models.
    assert.deepEqual(
      customLoadOptions({ ...base, recipe_options: { ctx_size: 12288, pinned: true } }),
      { save_options: false, ctx_size: 12288, pinned: true },
      'saved ctx_size must survive the max_context_window default',
    );
    assert.equal(
      customLoadOptions({ ...base, recipe_options: { ctx_size: -1 } }).ctx_size,
      -1,
      'the -1 "auto" sentinel must survive so lemond can auto-tune',
    );

    // Without a saved ctx_size the max window remains the default.
    assert.deepEqual(
      customLoadOptions({ ...base, recipe_options: { pinned: true } }),
      { save_options: false, pinned: true, ctx_size: 40960 },
    );
    assert.equal(customLoadOptions({ ...base }).ctx_size, 40960);

    // Values the load route's integer schema rejects must not be forwarded;
    // they fall back to the max-window default instead of 400-ing the load.
    for (const bad of ['12288', '-1', 'Infinity', true, {}, 0, null, 12288.5]) {
      assert.equal(
        customLoadOptions({ ...base, recipe_options: { ctx_size: bad } }).ctx_size,
        40960,
        `saved ctx_size ${JSON.stringify(bad)} must fall back to the max window`,
      );
    }

    // Pre-existing behavior is unchanged.
    assert.equal(
      customLoadOptions({ ...base, type: 'omni', recipe_options: {} }).ctx_size,
      undefined,
      'omni models must not get a forced ctx_size',
    );
    assert.equal(
      customLoadOptions({ type: 'embedding', max_context_window: 40960, recipe_options: { ctx_size: 4096 } }),
      undefined,
      'non-custom models must keep the previous no-op',
    );
    assert.equal(
      customLoadOptions({ custom: true, type: 'embedding', recipe_options: {} }).ctx_size,
      undefined,
      'no max_context_window and no saved value must leave ctx_size unset',
    );
    assert.equal(
      customLoadOptions({ custom: true, type: 'embedding', recipe_options: { ctx_size: 8192 } }).ctx_size,
      8192,
      'a saved ctx_size without a max window passes through',
    );

    console.log('Lemonade App custom load options regression checks passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
