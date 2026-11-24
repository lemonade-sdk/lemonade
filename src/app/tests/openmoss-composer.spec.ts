import { expect, test, type Page } from '@playwright/test';

const SPEECH_MODEL = 'OpenMOSS-TTS';
const VOICEGEN_MODEL = 'MOSS-VoiceGen';
const SFX_MODEL = 'MOSS-SoundEffect';

const MODELS = [
  { name: SPEECH_MODEL, labels: ['tts'], type: 'tts' },
  { name: VOICEGEN_MODEL, labels: ['tts', 'voice-design'], type: 'tts' },
  { name: SFX_MODEL, labels: ['audio-generation'], type: 'audio-generation' },
];

function wavBytes(): Buffer {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0, 'ascii');
  header.writeUInt32LE(36, 4);
  header.write('WAVE', 8, 'ascii');
  header.write('fmt ', 12, 'ascii');
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(24000, 24);
  header.writeUInt32LE(48000, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36, 'ascii');
  header.writeUInt32LE(0, 40);
  return header;
}

async function mockOpenMossServer(
  page: Page,
  loadedModel: string,
  { holdModelOptions = false, lateOptions = {} as Record<string, unknown> } = {},
) {
  let releaseModelOptions = () => { };
  const modelOptionsReleased = holdModelOptions
    ? new Promise<void>(resolve => { releaseModelOptions = resolve; })
    : Promise.resolve();
  const speechRequests: Record<string, unknown>[] = [];
  const generationRequests: Record<string, unknown>[] = [];
  const loadRequests: string[] = [];
  const loaded = MODELS.find(model => model.name === loadedModel)!;

  await page.addInitScript(() => {
    localStorage.setItem('lemonade:mcp_enabled', 'false');
  });
  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => route.fulfill({
    json: {
      status: 'ok',
      version: 'test',
      all_models_loaded: [{ model_name: loaded.name, type: loaded.type, recipe: 'openmoss', device: 'gpu' }],
    },
  }));
  await page.route(/\/api\/v1\/system-info(?:\?.*)?$/, route => route.fulfill({
    json: {
      recipes: {
        openmoss: {
          default_backend: 'vulkan',
          selectable_backend: true,
          modality: 'Text-to-speech',
          options: [],
          backends: { vulkan: { state: 'installed', version: 'test' } },
        },
      },
    },
  }));
  await page.route(/\/api\/v1\/models(?:\/[^?]*)?(?:\?.*)?$/, async route => {
    const optionsMatch = new URL(route.request().url()).pathname.match(/\/api\/v1\/models\/([^/]+)\/options$/);
    if (optionsMatch) {
      await modelOptionsReleased;
      const name = decodeURIComponent(optionsMatch[1]);
      return route.fulfill({
        json: {
          model_name: name,
          recipe: 'openmoss',
          saved: {},
          effective: { model_name: name, ...lateOptions },
          defaults: { model_name: name },
        },
      });
    }
    return route.fulfill({
      json: {
        data: MODELS.map(model => ({
          id: model.name, name: model.name, labels: model.labels, recipe: 'openmoss', downloaded: true, suggested: true,
        })),
      },
    });
  });
  await page.route(/\/api\/v1\/load$/, route => {
    loadRequests.push(String((route.request().postDataJSON() as Record<string, unknown>).model_name));
    return route.fulfill({ json: { status: 'success' } });
  });
  await page.route(/\/api\/v1\/audio\/speech$/, route => {
    speechRequests.push(route.request().postDataJSON() as Record<string, unknown>);
    return route.fulfill({ status: 200, contentType: 'audio/wav', body: wavBytes() });
  });
  await page.route(/\/api\/v1\/audio\/generations$/, route => {
    generationRequests.push(route.request().postDataJSON() as Record<string, unknown>);
    return route.fulfill({ status: 200, contentType: 'audio/wav', body: wavBytes() });
  });

  return { speechRequests, generationRequests, loadRequests, releaseModelOptions };
}

test.describe('OpenMOSS composer', () => {
  test('describe mode asks the speech model itself to design the voice', async ({ page }) => {
    const server = await mockOpenMossServer(page, SPEECH_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__openmoss-settings');
    await expect(settings).toBeVisible();
    const mode = settings.locator('select');
    await expect(mode.locator('option')).toHaveText(['Plain', 'Describe voice', 'Clone WAV sample']);
    await mode.selectOption('describe');

    const send = page.locator('.composer__send');
    await page.locator('.composer__input').fill('Hello from the composer.');
    await expect(send).toBeDisabled();
    await settings.getByRole('textbox').fill('a warm low voice with a British accent');
    await expect(settings.locator('.composer__openmoss-status')).toContainText(`${SPEECH_MODEL} designs a reference voice`);

    await settings.locator('.composer__openmoss-params > summary').click();
    await settings.getByRole('spinbutton', { name: 'Audio temp' }).fill('1.2');

    await send.click();
    await expect(page.locator('.message--assistant').last()).toContainText('Designed a voice from your description');

    expect(server.speechRequests).toHaveLength(1);
    const [request] = server.speechRequests;
    expect(request.model).toBe(SPEECH_MODEL);
    expect(request.input).toBe('Hello from the composer.');
    expect(request.voice_design_description).toBe('a warm low voice with a British accent');
    expect(request).not.toHaveProperty('voice');
    expect(request).not.toHaveProperty('reference_wav_b64');
    expect(request.audio_temperature).toBe(1.2);
    expect(request).not.toHaveProperty('audio_top_k');
    expect(server.loadRequests).not.toContain(VOICEGEN_MODEL);
  });

  test('clone mode sends the WAV sample with the style note as voice', async ({ page }) => {
    const server = await mockOpenMossServer(page, SPEECH_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__openmoss-settings');
    await settings.locator('select').selectOption('clone');
    await settings.getByRole('textbox').fill('calm, conversational');
    await page.locator('input[type="file"]').setInputFiles({ name: 'sample.wav', mimeType: 'audio/wav', buffer: wavBytes() });
    await expect(settings.locator('.composer__openmoss-status')).toContainText('Voice sample ready: sample.wav');

    await page.locator('.composer__input').fill('Read this in my voice.');
    await page.locator('.composer__send').click();
    await expect(page.locator('.message--assistant').last()).toContainText('Generated speech using the attached voice sample.');

    expect(server.speechRequests).toHaveLength(1);
    const [request] = server.speechRequests;
    expect(request.model).toBe(SPEECH_MODEL);
    expect(request.voice).toBe('calm, conversational');
    expect(request.reference_wav_b64).toBe(wavBytes().toString('base64'));
    expect(request).not.toHaveProperty('voice_design_description');
  });

  test('a voice-design model only describes, and takes the description as voice', async ({ page }) => {
    const server = await mockOpenMossServer(page, VOICEGEN_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__openmoss-settings');
    await expect(settings.locator('select option')).toHaveText(['Describe voice']);
    await settings.getByRole('textbox').fill('an excited young narrator');
    await page.locator('.composer__input').fill('Welcome back.');
    await page.locator('.composer__send').click();
    await expect(page.locator('.message--assistant').last()).toContainText('Designed a voice from your description');

    expect(server.speechRequests).toHaveLength(1);
    const [request] = server.speechRequests;
    expect(request.model).toBe(VOICEGEN_MODEL);
    expect(request.voice).toBe('an excited young narrator');
    expect(request).not.toHaveProperty('voice_design_description');
  });

  test('voice settings entered before the model options load survive the load', async ({ page }) => {
    const server = await mockOpenMossServer(page, SPEECH_MODEL, {
      holdModelOptions: true,
      lateOptions: { voice: 'a gravelly old voice' },
    });
    await page.goto('/');

    const settings = page.locator('.composer__openmoss-settings');
    await settings.locator('select').selectOption('describe');
    await settings.locator('.composer__openmoss-params > summary').click();
    await settings.getByRole('spinbutton', { name: 'Audio top-k' }).fill('40');

    server.releaseModelOptions();
    await expect(settings.getByRole('textbox')).toHaveValue('a gravelly old voice');
    await expect(settings.locator('select')).toHaveValue('describe');
    await expect(settings.getByRole('spinbutton', { name: 'Audio top-k' })).toHaveValue('40');

    await page.locator('.composer__input').fill('Still here after the options loaded.');
    await page.locator('.composer__send').click();
    await expect(page.locator('.message--assistant').last()).toContainText('Designed a voice from your description');

    expect(server.speechRequests).toHaveLength(1);
    const [request] = server.speechRequests;
    expect(request.voice_design_description).toBe('a gravelly old voice');
    expect(request.audio_top_k).toBe(40);
  });

  test('sound-effect edits survive a late options load that still updates untouched settings', async ({ page }) => {
    const server = await mockOpenMossServer(page, SFX_MODEL, {
      holdModelOptions: true,
      lateOptions: { cfg_scale: 6.5 },
    });
    await page.goto('/');

    const settings = page.locator('.composer__audio-generation-settings');
    await settings.getByRole('spinbutton', { name: 'Steps' }).fill('30');
    await settings.getByRole('textbox', { name: /Negative prompt/ }).fill('music, speech');

    server.releaseModelOptions();
    await expect(settings.getByRole('spinbutton', { name: 'CFG' })).toHaveValue('6.5');
    await expect(settings.getByRole('spinbutton', { name: 'Steps' })).toHaveValue('30');
    await expect(settings.getByRole('textbox', { name: /Negative prompt/ })).toHaveValue('music, speech');

    await page.locator('.composer__input').fill('rain on a tin roof');
    await page.locator('.composer__send').click();
    await expect(page.locator('.message--assistant').last()).toContainText('Generated a sound effect from your prompt.');

    expect(server.generationRequests).toHaveLength(1);
    const [request] = server.generationRequests;
    expect(request.steps).toBe(30);
    expect(request.cfg).toBe(6.5);
    expect(request.negative_prompt).toBe('music, speech');
  });

  test('MOSS-SoundEffect starts from engine defaults and draws a real seed', async ({ page }) => {
    const server = await mockOpenMossServer(page, SFX_MODEL);
    await page.goto('/');

    const settings = page.locator('.composer__audio-generation-settings');
    await expect(settings).toBeVisible();
    await expect(settings.getByRole('spinbutton', { name: 'Steps' })).toHaveValue('100');
    await expect(settings.getByRole('spinbutton', { name: 'CFG' })).toHaveValue('4');
    await expect(settings.getByRole('spinbutton', { name: 'Sigma shift' })).toHaveValue('5');
    await settings.getByRole('textbox', { name: /Negative prompt/ }).fill('music, speech');

    await page.locator('.composer__input').fill('a heavy wooden door creaking open');
    await page.locator('.composer__send').click();
    await expect(page.locator('.message--assistant').last()).toContainText('Generated a sound effect from your prompt.');

    expect(server.generationRequests).toHaveLength(1);
    const [request] = server.generationRequests;
    expect(request.model).toBe(SFX_MODEL);
    expect(request.steps).toBe(100);
    expect(request.cfg).toBe(4);
    expect(request.sigma_shift).toBe(5);
    expect(request.negative_prompt).toBe('music, speech');
    expect(Number.isInteger(request.seed)).toBe(true);
    expect(request.seed as number).toBeGreaterThanOrEqual(0);
    expect(request.seed as number).toBeLessThan(2 ** 32);
  });
});
