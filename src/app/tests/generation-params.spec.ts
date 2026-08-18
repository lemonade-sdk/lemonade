import { expect, test } from '@playwright/test';

/*
 * The composer renders whatever generation parameters the server declares. Both
 * recipes below are invented for this test and appear nowhere in the client, so
 * a control that shows up here proves the declaration reached the UI on its own
 * rather than a backend being recognised by name.
 */

const AUDIO_MODEL = 'Invented-Audio-Model';
const SPEECH_MODEL = 'Invented-Speech-Model';

const SYSTEM_INFO = {
  recipes: {
    inventedaudio: {
      default_backend: 'cpu',
      modality: 'Audio generation',
      options: [],
      backends: { cpu: { state: 'installed', version: 'test' } },
      generation_params: {
        'audio-generation': [
          { name: 'seconds', label: 'Duration', type_name: 'NUMBER', default: 7, min: 1, max: 300, step: 1 },
          { name: 'wobble', label: 'Wobble', type_name: 'NUMBER', default: 3.5, min: 0, max: 20, step: 0.5 },
          { name: 'negative_prompt', label: 'Negative prompt', type_name: 'TEXT' },
          { name: 'seed', label: 'Seed', type_name: 'SEED', min: 0, max: 4294967295, step: 1 },
        ],
      },
    },
    inventedspeech: {
      default_backend: 'cpu',
      modality: 'Text-to-speech',
      options: [],
      backends: { cpu: { state: 'installed', version: 'test' } },
      generation_params: {
        tts: [
          { name: 'described', label: 'Describe voice', type_name: 'TEXT', exclusive_group: 'voice_mode' },
          { name: 'sample_b64', label: 'Clone WAV sample', type_name: 'AUDIO_B64', exclusive_group: 'voice_mode', accept: '.wav' },
          { name: 'warble', label: 'Warble', type_name: 'NUMBER', default: 1.25, min: 0, max: 3, step: 0.05, group: 'advanced' },
        ],
      },
    },
  },
};

const MODELS = [
  {
    id: AUDIO_MODEL, name: AUDIO_MODEL, display_name: AUDIO_MODEL,
    labels: ['audio-generation'], recipe: 'inventedaudio',
    registry_source: 'huggingface', downloaded: true, size: 1,
    audio_defaults: { seconds: 42 },
  },
  {
    id: SPEECH_MODEL, name: SPEECH_MODEL, display_name: SPEECH_MODEL,
    labels: ['tts'], recipe: 'inventedspeech',
    registry_source: 'huggingface', downloaded: true, size: 1,
  },
];

async function mockServer(page: import('@playwright/test').Page, loaded: string) {
  await page.route('**/api/v1/health**', route => route.fulfill({
    json: {
      status: 'ok', version: 'test',
      all_models_loaded: [{
        model_name: loaded,
        model_type: loaded === AUDIO_MODEL ? 'audio-generation' : 'tts',
        recipe: loaded === AUDIO_MODEL ? 'inventedaudio' : 'inventedspeech',
      }],
    },
  }));
  await page.route('**/api/v1/system-info**', route => route.fulfill({ json: SYSTEM_INFO }));
  await page.route('**/api/v1/models**', route => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({ data: MODELS }),
  }));
}

test.describe('Server-declared generation parameters', () => {
  test('audio-generation controls come from the declaration, defaults from the model', async ({ page }) => {
    await mockServer(page, AUDIO_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__audio-generation-settings');
    await expect(settings).toBeVisible();

    // Declared for a recipe the client has never heard of.
    await expect(settings.getByRole('spinbutton', { name: /Wobble/ })).toHaveValue('3.5');
    await expect(settings.getByRole('textbox', { name: /Negative prompt/ })).toBeVisible();

    // audio_defaults overrides the recipe-level default of 7.
    await expect(settings.getByRole('spinbutton', { name: /Duration/ })).toHaveValue('42');

    // Nothing the client was not told about.
    await expect(settings.getByRole('spinbutton', { name: /CFG/ })).toHaveCount(0);
    await expect(settings.getByRole('textbox', { name: /Lyrics/ })).toHaveCount(0);
  });

  test('an exclusive group becomes a selector that drives the attachment', async ({ page }) => {
    await mockServer(page, SPEECH_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__generation-settings');
    await expect(settings).toBeVisible();

    // The group id becomes the selector label; members become its options.
    const selector = settings.locator('label', { hasText: 'Voice mode' }).locator('select');
    await expect(selector).toBeVisible();
    await expect(selector.locator('option')).toHaveText(['Describe voice', 'Clone WAV sample']);

    // The first member is a text input, and no attachment is asked for.
    await expect(settings.getByRole('textbox', { name: /Describe voice/ })).toBeVisible();
    await expect(settings.locator('.composer__generation-status'))
      .not.toContainText('Attach one');

    // Switching to the AUDIO_B64 member turns on the attachment affordance.
    await selector.selectOption('sample_b64');
    await expect(settings.locator('.composer__generation-status')).toContainText('Attach one .wav');

    // Advanced params stay behind the disclosure.
    await expect(settings.locator('.composer__generation-params > summary')).toBeVisible();
    await settings.locator('.composer__generation-params > summary').click();
    await expect(settings.getByRole('spinbutton', { name: /Warble/ })).toHaveValue('1.25');
  });
});
