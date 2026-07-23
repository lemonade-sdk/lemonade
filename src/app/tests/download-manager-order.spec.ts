import { expect, test } from '@playwright/test';

test('download manager keeps creation order while concurrent progress updates arrive', async ({ page }) => {
  const timestamp = Date.now();
  const stored = [
    {
      id: 'model:Older-Model',
      downloadType: 'model',
      modelName: 'Older-Model',
      fileName: 'older.gguf',
      fileIndex: 1,
      totalFiles: 1,
      bytesDownloaded: 100,
      bytesTotal: 1000,
      percent: 10,
      status: 'downloading',
      createdAt: timestamp - 2000,
      startTime: timestamp - 2000,
      bytesResumed: 0,
      running: true,
      updatedAt: timestamp + 5000,
    },
    {
      id: 'model:Newer-Model',
      downloadType: 'model',
      modelName: 'Newer-Model',
      fileName: 'newer.gguf',
      fileIndex: 1,
      totalFiles: 1,
      bytesDownloaded: 200,
      bytesTotal: 1000,
      percent: 20,
      status: 'downloading',
      createdAt: timestamp - 1000,
      startTime: timestamp - 1000,
      bytesResumed: 0,
      running: true,
      updatedAt: timestamp,
    },
  ];

  await page.addInitScript((items: unknown[]) => {
    localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify(items));
  }, stored);

  let serverDownloads = [
    {
      id: 'model:Older-Model', type: 'model', model_name: 'Older-Model', status: 'downloading', running: true,
      file: 'older.gguf', file_index: 1, total_files: 1, bytes_downloaded: 150, bytes_total: 1000, percent: 15,
    },
    {
      id: 'model:Newer-Model', type: 'model', model_name: 'Newer-Model', status: 'downloading', running: true,
      file: 'newer.gguf', file_index: 1, total_files: 1, bytes_downloaded: 250, bytes_total: 1000, percent: 25,
    },
  ];

  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: { downloads: serverDownloads } }));

  await page.goto('/');
  await page.locator('.titlebar__download-toggle').click();

  const names = page.locator('.download-item__names strong');
  await expect(names).toHaveText(['Newer-Model', 'Older-Model']);

  // Reverse the server update order. A list sorted by updatedAt would now jump;
  // creation-order sorting must keep the visual positions unchanged.
  serverDownloads = [
    {
      id: 'model:Newer-Model', type: 'model', model_name: 'Newer-Model', status: 'downloading', running: true,
      file: 'newer.gguf', file_index: 1, total_files: 1, bytes_downloaded: 350, bytes_total: 1000, percent: 35,
    },
    {
      id: 'model:Older-Model', type: 'model', model_name: 'Older-Model', status: 'downloading', running: true,
      file: 'older.gguf', file_index: 1, total_files: 1, bytes_downloaded: 450, bytes_total: 1000, percent: 45,
    },
  ];

  await page.waitForTimeout(1300);
  await expect(names).toHaveText(['Newer-Model', 'Older-Model']);
});

test('terminal-looking server download stays locked until the worker stops', async ({ page }) => {
  let serverDownloads = [
    {
      id: 'model:Finalizing-Model', type: 'model', model_name: 'Finalizing-Model',
      status: 'completed', complete: true, running: true,
      file: 'model.gguf', file_index: 1, total_files: 1,
      bytes_downloaded: 1000, bytes_total: 1000, percent: 100,
    },
  ];

  await page.addInitScript((item: unknown) => {
    localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify([item]));
  }, {
    id: 'model:Finalizing-Model',
    downloadType: 'model',
    modelName: 'Finalizing-Model',
    fileName: 'model.gguf',
    fileIndex: 1,
    totalFiles: 1,
    bytesDownloaded: 1000,
    bytesTotal: 1000,
    percent: 100,
    status: 'completed',
    running: true,
    createdAt: Date.now(),
    startTime: Date.now(),
    bytesResumed: 0,
    updatedAt: Date.now(),
  });

  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: { downloads: serverDownloads } }));

  await page.goto('/');
  await page.locator('.titlebar__download-toggle').click();

  const item = page.locator('.download-item').filter({ hasText: 'Finalizing-Model' });
  await expect(item).toContainText('Finalizing');
  await expect(item.getByRole('button', { name: 'Remove from list' })).toHaveCount(0);

  // A terminal status is not terminal while the server still owns the worker.
  // Once running=false arrives, the row becomes removable.
  serverDownloads = [{ ...serverDownloads[0], running: false }];
  await page.waitForTimeout(1300);
  await expect(item).toContainText('Completed');
  await expect(item.getByRole('button', { name: 'Remove from list' })).toBeEnabled();
});

test('authoritative server snapshot removes stale paused renderer state', async ({ page }) => {
  const timestamp = Date.now() - 60_000;
  await page.addInitScript((item: unknown) => {
    localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify([item]));
  }, {
    id: 'model:Stale-Paused-Model',
    downloadType: 'model',
    modelName: 'Stale-Paused-Model',
    fileName: 'model.gguf',
    fileIndex: 1,
    totalFiles: 1,
    bytesDownloaded: 100,
    bytesTotal: 1000,
    percent: 10,
    status: 'paused',
    createdAt: timestamp,
    startTime: timestamp,
    bytesResumed: 0,
    running: false,
    updatedAt: timestamp,
  });

  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: { downloads: [] } }));

  await page.goto('/');
  await page.locator('.titlebar__download-toggle').click();
  await expect(page.locator('.download-item')).toHaveCount(0);
  await expect(page.locator('.download-manager__empty')).toContainText('No downloads yet');
});
