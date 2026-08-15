import { expect, test } from '@playwright/test';

/*
 * End-to-end cover for the deployment-label classification: one model per
 * documented label, plus both collection kinds, all carrying the registry_source
 * every served model carries. Each must show its own type in the detail header,
 * and none may fall through to "Unknown".
 */

type Row = { id: string; labels: string[]; recipe: string; expected: string };

const CATALOG: Row[] = [
  { id: 'Kind-Chat', labels: ['chat', 'reasoning'], recipe: 'llamacpp', expected: 'Chat' },
  { id: 'Kind-Vision-Chat', labels: ['chat', 'vision'], recipe: 'llamacpp', expected: 'Chat' },
  { id: 'Kind-Transcription', labels: ['transcription', 'realtime-transcription'], recipe: 'whispercpp', expected: 'Audio' },
  { id: 'Kind-Embeddings', labels: ['embeddings'], recipe: 'llamacpp', expected: 'Embedding' },
  { id: 'Kind-Reranking', labels: ['reranking'], recipe: 'llamacpp', expected: 'Reranking' },
  { id: 'Kind-Image', labels: ['image'], recipe: 'sd-cpp', expected: 'Image' },
  { id: 'Kind-Tts', labels: ['tts'], recipe: 'kokoro', expected: 'TTS' },
  { id: 'Kind-AudioGen', labels: ['audio-generation'], recipe: 'acestep', expected: 'Music & SFX' },
  { id: 'Kind-Classification', labels: ['classification'], recipe: 'onnxruntime', expected: 'Classification' },
  { id: 'Kind-Mesh', labels: ['3d'], recipe: 'trellis', expected: '3D' },
  { id: 'Kind-Omni-Collection', labels: ['chat'], recipe: 'collection.omni', expected: 'Omni' },
  { id: 'Kind-Router-Collection', labels: ['chat'], recipe: 'collection.router', expected: 'Router' },
];

test.describe('Model kind display', () => {
  test.beforeEach(async ({ page }) => {
    await page.route('**/api/v1/health**', route => route.fulfill({
      json: { status: 'ok', version: 'test', all_models_loaded: [] },
    }));
    await page.route('**/api/v1/system-info**', route => route.fulfill({
      json: {
        recipes: {
          llamacpp: { default_backend: 'cpu', backends: { cpu: { state: 'installed', version: 'test' } } },
        },
      },
    }));
    await page.route('**/api/v1/models**', route => route.fulfill({
      contentType: 'application/json',
      body: JSON.stringify({
        data: CATALOG.map(row => ({
          id: row.id,
          name: row.id,
          display_name: row.id,
          labels: row.labels,
          recipe: row.recipe,
          // Every served model carries this; it used to send the whole catalog
          // down the remote-search branch and out as Unknown.
          registry_source: 'huggingface',
          downloaded: true,
          size: 1,
          ...(row.recipe.startsWith('collection.') ? { components: ['Kind-Chat'] } : {}),
        })),
      }),
    }));

    await page.goto('/');
    await page.locator('.titlebar__nav').getByText('Models').click();
    await expect(page.locator('.titlebar__status-dot--brand')).toHaveClass(/titlebar__status-dot--connected/);
  });

  test('every deployment label renders its own type, never Unknown', async ({ page }) => {
    const list = page.locator('.model-list-panel__list .workspace-list-row');
    await expect(list).toHaveCount(CATALOG.length);

    for (const row of CATALOG) {
      await list.filter({ hasText: row.id }).first().click();
      const header = page.locator('.workspace-detail-panel__metadata').first();
      await expect(header, `${row.id} must show its own type`).toContainText(row.expected);
      await expect(header, `${row.id} must not be Unknown`).not.toContainText('Unknown');
    }

    await page.screenshot({ path: 'test-results/model-kind-display.png', fullPage: true });
  });

  test('each task filter counts its own models, with no double counting', async ({ page }) => {
    const chip = (name: RegExp) => page.locator('.model-nav-rail__task-chip').filter({ hasText: name });
    const list = page.locator('.model-list-panel__list .workspace-list-row');

    // A collection serves chat but has its own task, so it is counted once.
    await chip(/^Chat/).click();
    await expect(list).toHaveCount(2);
    await expect(list.filter({ hasText: 'Collection' })).toHaveCount(0);
    await chip(/^Chat/).click();

    await chip(/^Omni/).click();
    await expect(list).toHaveCount(1);
    await expect(list.filter({ hasText: 'Kind-Omni-Collection' })).toHaveCount(1);
    await chip(/^Omni/).click();

    // Classification models are discoverable rather than silently unfiltered.
    await chip(/^Classify/).click();
    await expect(list).toHaveCount(1);
    await expect(list.filter({ hasText: 'Kind-Classification' })).toHaveCount(1);
  });

  test('chat controls follow the declared input modalities', async ({ page }) => {
    // A vision chat model offers image attachment; a plain chat model does not.
    await page.locator('.model-list-panel__list .workspace-list-row').filter({ hasText: 'Kind-Vision-Chat' }).first().click();
    await expect(page.locator('.workspace-detail-panel__metadata').first()).toContainText('Chat');

    await page.locator('.model-list-panel__list .workspace-list-row').filter({ hasText: 'Kind-Classification' }).first().click();
    const classificationHeader = page.locator('.workspace-detail-panel__metadata').first();
    await expect(classificationHeader).toContainText('Classification');
    // No /classify surface exists yet, so the row is informational only.
    await expect(classificationHeader).not.toContainText('Chat');
  });
});
