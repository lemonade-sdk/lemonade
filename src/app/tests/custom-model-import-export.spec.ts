import { readFile } from 'node:fs/promises';
import { test, expect, type Page } from '@playwright/test';

async function openModelsWithCustomDefinitions(page: Page, options: {
  omitRegisteredFromCatalog?: boolean;
  includeEmptyCollection?: boolean;
} = {}) {
  const models: Array<Record<string, unknown>> = [
    { id: 'PlannerModel', name: 'PlannerModel', labels: ['chat'], recipe: 'llamacpp', downloaded: true },
    { id: 'user.Standalone', name: 'user.Standalone', display_name: 'Standalone', custom: true,
      labels: ['chat', 'custom'], recipe: 'llamacpp', checkpoint: 'standalone.gguf', downloaded: true },
    { id: 'user.SavedCollection', name: 'user.SavedCollection', display_name: 'Saved Collection', custom: true,
      labels: ['omni', 'custom'], recipe: 'collection.omni', type: 'omni', downloaded: true,
      components: ['PlannerModel'], component_roles: { llm: 'PlannerModel' } },
    { id: 'user.OtherCollection', name: 'user.OtherCollection', display_name: 'Other Collection', custom: true,
      labels: ['omni', 'custom'], recipe: 'collection.omni', type: 'omni', downloaded: true,
      components: ['PlannerModel'], component_roles: { llm: 'PlannerModel' } },
  ];
  if (options.includeEmptyCollection) {
    models.push({ id: 'user.EmptyCollection', name: 'user.EmptyCollection', custom: true,
      labels: ['omni', 'custom'], type: 'omni', recipe: 'collection.omni', components: [] });
  }
  const registeredNames: string[] = [];
  await page.route('**/api/v1/health**', route => route.fulfill({
    json: { status: 'ok', version: 'test', all_models_loaded: [] },
  }));
  await page.route('**/api/v1/system-info**', route => route.fulfill({
    json: { recipes: { llamacpp: { default_backend: 'cpu', backends: { cpu: { state: 'installed' } } } } },
  }));
  await page.route(/\/api\/v1\/models(?:\?.*)?$/, route => route.fulfill({ json: { data: models } }));
  await page.route('**/api/v1/models/register', async route => {
    const body = route.request().postDataJSON() as Record<string, unknown>;
    const name = String(body.model_name);
    registeredNames.push(name);
    const publicName = name.replace(/^user\./, '');
    if (!options.omitRegisteredFromCatalog) {
      models.push({
        id: publicName,
        name: publicName,
        display_name: String(body.display_name || publicName),
        custom: true,
        labels: ['chat', 'custom'],
        recipe: String(body.recipe || 'llamacpp'),
        checkpoint: String(body.checkpoint || ''),
        downloaded: true,
      });
    }
    await route.fulfill({ json: { model_name: publicName, canonical_model_name: name } });
  });

  await page.goto('/');
  await page.locator('.titlebar__nav').getByText('Models').click();
  await expect(page.locator('.titlebar__status-dot--brand')).toHaveClass(/titlebar__status-dot--connected/);
  return { registeredNames };
}

async function exportedModelNames(page: Page, button: ReturnType<Page['getByRole']>): Promise<string[]> {
  const [download] = await Promise.all([page.waitForEvent('download'), button.click()]);
  const exported = JSON.parse(await readFile(await download.path(), 'utf8')) as {
    models: Array<{ name: string }>;
  };
  return exported.models.map(model => model.name);
}

test('Export all separates ordinary custom models and Omni collections (#4017)', async ({ page }) => {
  const { registeredNames } = await openModelsWithCustomDefinitions(page);
  await expect(page.getByRole('button', { name: 'Export all custom models' })).toHaveCount(0);

  await page.getByRole('button', { name: 'Open custom models' }).click();
  const editor = page.locator('.custom-model-editor');
  await expect(editor).toBeVisible();
  const exportModels = editor.getByRole('button', { name: 'Export all custom models' });
  await expect(exportModels).toBeVisible();
  expect(await exportedModelNames(page, exportModels)).toEqual(['user.Standalone']);

  await editor.getByRole('button', { name: 'Omni Collection' }).click();
  const exportCollections = editor.getByRole('button', { name: 'Export all custom collections' });
  await expect(exportCollections).toBeVisible();
  expect((await exportedModelNames(page, exportCollections)).sort()).toEqual([
    'user.SavedCollection', 'user.OtherCollection',
  ].sort());
  const search = page.getByRole('searchbox', { name: 'Search models' });
  await search.fill('no-matching-imported-model');
  await editor.locator('input[type="file"]').setInputFiles({
    name: 'custom-models.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify({
      models: [
        { name: 'ImportedAlpha', display_name: 'Imported Alpha', checkpoint: 'alpha.gguf', recipe: 'llamacpp', type: 'chat' },
        { name: 'ImportedBeta', display_name: 'Imported Beta', checkpoint: 'beta.gguf', recipe: 'llamacpp', type: 'chat' },
      ],
    })),
  });
  await expect(editor).toHaveCount(0);
  await expect(search).toHaveValue('');
  await expect(page.locator('.model-detail-panel')).toContainText('Imported Beta');
  await expect(page.locator('.model-list-panel__list')).toContainText('Imported Alpha');
  await expect(page.locator('.model-list-panel__list')).toContainText('Imported Beta');
  expect(registeredNames).toEqual(['user.ImportedAlpha', 'user.ImportedBeta']);
});

test('saved Omni collection exports only itself and its editor scrolls (#4017)', async ({ page }) => {
  await openModelsWithCustomDefinitions(page);
  const list = page.locator('.model-list-panel__list');
  await list.locator('.workspace-list-row').filter({ hasText: 'Standalone' }).click();
  await expect(page.getByRole('tab', { name: 'Configuration' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Edit collection settings' })).toHaveCount(0);

  await list.locator('.workspace-list-row').filter({ hasText: 'Saved Collection' }).click();
  await page.getByRole('button', { name: 'Edit collection settings' }).click();
  const editor = page.locator('.custom-model-editor');
  await expect(editor).toBeVisible();
  const exportOne = editor.getByRole('button', { name: 'Export this collection' });
  await expect(exportOne).toBeVisible();
  await expect(editor.getByRole('button', { name: /Export all custom/ })).toHaveCount(0);
  expect(await exportedModelNames(page, exportOne)).toEqual(['user.SavedCollection']);

  for (const viewport of [{ width: 1280, height: 540 }, { width: 700, height: 540 }]) {
    await page.setViewportSize(viewport);
    const scroller = editor.locator('.custom-model-form__body');
    await expect(scroller).toBeVisible();
    await expect.poll(() => scroller.evaluate(el => el.scrollHeight - el.clientHeight)).toBeGreaterThan(0);
    await scroller.evaluate(el => { el.scrollTop = 0; el.scrollTop = el.scrollHeight; });
    await expect.poll(() => scroller.evaluate(el => el.scrollTop)).toBeGreaterThan(0);
  }
});

test('ordinary export ignores incomplete Omni collections (#4017)', async ({ page }) => {
  await openModelsWithCustomDefinitions(page, { includeEmptyCollection: true });
  await page.getByRole('button', { name: 'Open custom models' }).click();
  const exportModels = page.locator('.custom-model-editor').getByRole('button', { name: 'Export all custom models' });
  expect(await exportedModelNames(page, exportModels)).toEqual(['user.Standalone']);
});

test('import keeps editor open if the catalog has not updated (#4018)', async ({ page }) => {
  await openModelsWithCustomDefinitions(page, { omitRegisteredFromCatalog: true });
  await page.getByRole('button', { name: 'Open custom models' }).click();
  const editor = page.locator('.custom-model-editor');
  await editor.locator('input[type="file"]').setInputFiles({
    name: 'one-model.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify({ name: 'UnlistedModel', checkpoint: 'unlisted.gguf', recipe: 'llamacpp', type: 'chat' })),
  });
  await expect(editor).toBeVisible();
  await expect(editor.locator('.custom-model-form__error')).toContainText('Import succeeded, but the model is not visible yet.');
});
