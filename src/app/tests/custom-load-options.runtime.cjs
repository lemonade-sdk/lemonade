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
    const {
      customLoadOptions, isStandaloneImageUpscaler, customImageCompanionCheckpoints,
      customImagePurpose, customImageLabelsForPurpose,
      buildCustomModelRecord, customModelToModelInfo, customRegistrationOptions,
    } = require(modulePath);
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

    // Both image recipes support one-shot pixel upscalers using only main.
    // TheNoise image generation still requires the text encoder and VAE.
    assert.equal(isStandaloneImageUpscaler('thenoise', ['UPSCALING']), true);
    assert.equal(isStandaloneImageUpscaler('thenoise', ['not-upscaling']), false);
    assert.equal(isStandaloneImageUpscaler('sd-cpp', ['upscaling']), true);
    assert.equal(isStandaloneImageUpscaler('sd-cpp', ['UPSCALE']), false);
    assert.equal(isStandaloneImageUpscaler('llamacpp', ['upscaling']), false);
    assert.deepEqual(customImageCompanionCheckpoints('thenoise', ['upscaling'], '', ''), {});
    assert.deepEqual(customImageCompanionCheckpoints('thenoise', ['upscaling'], 'unused', 'unused'), {},
      'a standalone upscaler must not register stale companion paths');
    assert.deepEqual(customImageCompanionCheckpoints('thenoise', [], ' encoder ', ' vae '),
      { text_encoder: 'encoder', vae: 'vae' });
    assert.throws(() => customImageCompanionCheckpoints('thenoise', [], '', 'vae'), /text encoder and VAE/);
    assert.throws(() => customImageCompanionCheckpoints('thenoise', [], 'encoder', ''), /text encoder and VAE/);
    assert.deepEqual(customImageCompanionCheckpoints('sd-cpp', [], '', ''), {});
    assert.deepEqual(customImageCompanionCheckpoints('sd-cpp', [], ' encoder ', ' vae '),
      { text_encoder: 'encoder', vae: 'vae' }, 'SD.cpp diffusion checkpoints remain optional');
    assert.deepEqual(customImageCompanionCheckpoints('sd-cpp', ['upscaling'], 'stale', 'stale'), {},
      'SD.cpp upscalers must not register stale diffusion checkpoints');

    const remacri = buildCustomModelRecord({
      name: 'Remacri-4x-Custom',
      displayName: 'Remacri custom',
      checkpoint: 'mikkoph/upscalers:esrgan/4x-remacri.safetensors',
      recipe: 'thenoise',
      capability: 'image',
      labels: ['upscaling'],
    });
    const registration = customRegistrationOptions(customModelToModelInfo(remacri));
    assert.equal(registration.recipe, 'thenoise');
    assert.deepEqual(registration.labels, ['image', 'upscaling']);
    assert.equal(registration.checkpoint, 'mikkoph/upscalers:esrgan/4x-remacri.safetensors');
    assert.equal(registration.checkpoints, undefined);

    const realesrgan = buildCustomModelRecord({
      name: 'RealESRGAN-4x-Custom',
      displayName: 'RealESRGAN custom',
      checkpoint: 'amd/realesrgan-x4plus:RealESRGAN_x4plus.pth',
      recipe: 'sd-cpp',
      capability: 'image',
      labels: ['upscaling'],
    });
    const sdRegistration = customRegistrationOptions(customModelToModelInfo(realesrgan));
    assert.equal(sdRegistration.recipe, 'sd-cpp');
    assert.deepEqual(sdRegistration.labels, ['image', 'upscaling']);
    assert.equal(sdRegistration.checkpoint, 'amd/realesrgan-x4plus:RealESRGAN_x4plus.pth');
    assert.equal(sdRegistration.checkpoints, undefined);

    // Image purpose adds/removes only the feature label; the deployment remains `image`.
    assert.equal(customImagePurpose('sd-cpp', []), 'image');
    assert.equal(customImagePurpose('thenoise', ['EDIT']), 'edit');
    assert.equal(customImagePurpose('sd-cpp', ['edit']), 'edit');
    assert.equal(customImagePurpose('thenoise', ['edit', 'upscaling']), 'upscaling');
    assert.deepEqual(customImageLabelsForPurpose(['tool-calling', 'EDIT'], 'upscaling'),
      ['tool-calling', 'upscaling']);
    assert.deepEqual(customImageLabelsForPurpose(['upscaling', 'edit', 'custom-tag'], 'image'),
      ['custom-tag']);
    assert.deepEqual(customImageLabelsForPurpose(['custom-tag', 'edit'], 'edit'),
      ['custom-tag', 'edit'], 'switching to Edit must not duplicate its label');

    const editModel = buildCustomModelRecord({
      name: 'Custom-Image-Edit',
      displayName: 'Custom image editor',
      checkpoint: 'my-org/image-edit:model.safetensors',
      recipe: 'thenoise',
      capability: 'image',
      labels: customImageLabelsForPurpose([], 'edit'),
    });
    const editRegistration = customRegistrationOptions(customModelToModelInfo(editModel));
    assert.deepEqual(editRegistration.labels, ['image', 'edit']);
    assert.equal(editRegistration.recipe, 'thenoise');
    assert.deepEqual(customImageCompanionCheckpoints('thenoise', editModel.labels, 'text', 'vae'),
      { text_encoder: 'text', vae: 'vae' }, 'image editing retains TheNoise diffusion checkpoints');

    console.log('Lemonade App custom load options regression checks passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
