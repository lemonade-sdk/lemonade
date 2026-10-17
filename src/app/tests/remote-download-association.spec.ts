import { expect, test, type Page } from '@playwright/test';

const activeRepository = 'unsloth/Llama-3.2-3B-Instruct-GGUF';
const collidingRepository = 'example/Llama-3.2-3B-Instruct-GGUF';
const localModelName = 'Llama-3.2-3B-Instruct-GGUF-Q4_K_M';

type CatalogState = {
  registerCheckpoint: boolean;
  registrationSource: 'huggingface' | 'modelscope';
  downloadActive: boolean;
};

async function mockCatalogWithCollidingNames(
  page: Page,
  options: Partial<CatalogState> = {},
): Promise<{ state: CatalogState; modelRequests: () => number }> {
  const state: CatalogState = {
    registerCheckpoint: true,
    registrationSource: 'huggingface',
    downloadActive: true,
    ...options,
  };
  let modelRequestCount = 0;
  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', version: 'test', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/system-info**', route => route.fulfill({ json: {} }));
  await page.route('**/api/v1/models**', route => {
    modelRequestCount += 1;
    return route.fulfill({
      json: {
        data: state.registerCheckpoint ? [{
          id: localModelName,
          name: localModelName,
          checkpoint: `${activeRepository}:Q4_K_M`,
          source: state.registrationSource,
          registry_source: state.registrationSource,
          labels: ['chat'],
          recipe: 'llamacpp',
          downloaded: false,
        }] : [],
      },
    });
  });
  await page.route('**/api/v1/downloads**', route =>
    route.fulfill({
      json: {
        downloads: state.downloadActive ? [{
          id: `model:user.${localModelName}`,
          type: 'model',
          model_name: `user.${localModelName}`,
          status: 'downloading',
          running: true,
          file: 'model.gguf',
          file_index: 1,
          total_files: 1,
          bytes_downloaded: 420,
          bytes_total: 1000,
          percent: 42,
        }] : [],
      },
    }),
  );
  await page.route('**huggingface.co/api/models**', route =>
    route.fulfill({
      json: [activeRepository, collidingRepository].map((id, index) => ({
        id,
        modelId: id,
        likes: 10 - index,
        downloads: 100 - index,
        tags: ['gguf', 'text-generation'],
        pipeline_tag: 'text-generation',
      })),
    }),
  );
  await page.route('**/api/v1/pull/variants?**', route => {
    const checkpoint = new URL(route.request().url()).searchParams.get('checkpoint') || '';
    return route.fulfill({
      json: {
        checkpoint,
        source: 'huggingface',
        recipe: 'llamacpp',
        repo_kind: 'gguf',
        suggested_name: 'Llama-3.2-3B-Instruct-GGUF',
        suggested_labels: ['chat'],
        mmproj_files: [],
        variants: [{
          name: 'Q4_K_M',
          primary_file: 'model-Q4_K_M.gguf',
          files: ['model-Q4_K_M.gguf'],
          sharded: false,
          size_bytes: 1000,
        }],
      },
    });
  });
  return { state, modelRequests: () => modelRequestCount };
}

async function openCollidingSearch(page: Page): Promise<void> {
  await page.goto('/');
  await page.locator('.titlebar__nav').getByText('Models', { exact: true }).click();
  await page.locator('#model-list-search').fill('llama 3.2');
  await expect(page.locator('.zone--hf .workspace-list-row')).toHaveCount(2);
}

test('remote progress belongs only to the repository registered for the download', async ({ page }) => {
  await mockCatalogWithCollidingNames(page);
  await openCollidingSearch(page);

  const activeRow = page.locator('.zone--hf .workspace-list-row').filter({ hasText: activeRepository });
  const collidingRow = page.locator('.zone--hf .workspace-list-row').filter({ hasText: collidingRepository });

  await expect(activeRow).toContainText('Downloading 42%');
  await expect(activeRow.getByRole('button', { name: `Cancel download of ${activeRepository}` })).toBeVisible();
  await expect(collidingRow).not.toContainText('Downloading');
  await expect(collidingRow.getByRole('button', { name: `Cancel download of ${collidingRepository}` })).toHaveCount(0);

  // The association must also survive a reload, when component-local pull state is gone
  // and the UI reconstructs progress solely from the server snapshots.
  await page.reload();
  await page.locator('.titlebar__nav').getByText('Models', { exact: true }).click();
  await page.locator('#model-list-search').fill('llama 3.2');

  await expect(activeRow).toContainText('Downloading 42%');
  await expect(collidingRow).not.toContainText('Downloading');
});

test('remote progress is not guessed from a generated name without a checkpoint association', async ({ page }) => {
  await mockCatalogWithCollidingNames(page, { registerCheckpoint: false });
  await openCollidingSearch(page);

  const rows = page.locator('.zone--hf .workspace-list-row');
  await expect(rows.filter({ hasText: activeRepository })).not.toContainText('Downloading');
  await expect(rows.filter({ hasText: collidingRepository })).not.toContainText('Downloading');
  await expect(rows.getByRole('button', { name: /Cancel download of/ })).toHaveCount(0);
});

test('remote progress ignores a checkpoint registered by another provider', async ({ page }) => {
  await mockCatalogWithCollidingNames(page, { registrationSource: 'modelscope' });
  await openCollidingSearch(page);

  const rows = page.locator('.zone--hf .workspace-list-row');
  await expect(rows.filter({ hasText: activeRepository })).not.toContainText('Downloading');
  await expect(rows.filter({ hasText: collidingRepository })).not.toContainText('Downloading');
});

test('a newly discovered external download refreshes a stale model registry', async ({ page }) => {
  const catalog = await mockCatalogWithCollidingNames(page, {
    registerCheckpoint: false,
    downloadActive: false,
  });
  await openCollidingSearch(page);
  const modelRequestsBeforeDownload = catalog.modelRequests();

  catalog.state.registerCheckpoint = true;
  catalog.state.downloadActive = true;
  await page.evaluate(() => window.dispatchEvent(new Event('focus')));

  const activeRow = page.locator('.zone--hf .workspace-list-row').filter({ hasText: activeRepository });
  const collidingRow = page.locator('.zone--hf .workspace-list-row').filter({ hasText: collidingRepository });
  await expect.poll(catalog.modelRequests).toBeGreaterThan(modelRequestsBeforeDownload);
  await expect(activeRow).toContainText('Downloading 42%');
  await expect(collidingRow).not.toContainText('Downloading');
});
