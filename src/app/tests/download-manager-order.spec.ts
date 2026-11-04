import { expect, test } from '@playwright/test';

test('download manager keeps creation order while concurrent progress updates arrive', async ({ page }) => {
  const timestamp = Date.now();
  let serverDownloads = [
    {
      id: 'model:Older-Model', type: 'model', model_name: 'Older-Model', status: 'downloading', running: true,
      created_at: timestamp - 2000,
      file: 'older.gguf', file_index: 1, total_files: 1, bytes_downloaded: 150, bytes_total: 1000, percent: 15,
    },
    {
      id: 'model:Newer-Model', type: 'model', model_name: 'Newer-Model', status: 'downloading', running: true,
      created_at: timestamp - 1000,
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
      created_at: timestamp - 1000,
      file: 'newer.gguf', file_index: 1, total_files: 1, bytes_downloaded: 350, bytes_total: 1000, percent: 35,
    },
    {
      id: 'model:Older-Model', type: 'model', model_name: 'Older-Model', status: 'downloading', running: true,
      created_at: timestamp - 2000,
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

test('legacy localStorage download rows are ignored', async ({ page }) => {
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

test('paused remove refreshes the authoritative server snapshot', async ({ page }) => {
  let serverDownloads = [{
    id: 'model:Paused-Model', type: 'model', model_name: 'Paused-Model', status: 'paused', running: false,
    created_at: Date.now() - 1000,
    file: 'paused.gguf', file_index: 1, total_files: 1, bytes_downloaded: 100, bytes_total: 1000, percent: 10,
  }];

  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: { downloads: serverDownloads } }));
  await page.route('**/api/v1/downloads/control', async route => {
    serverDownloads = [];
    await route.fulfill({ json: { ok: true } });
  });

  await page.goto('/');
  await page.locator('.titlebar__download-toggle').click();
  const item = page.locator('.download-item').filter({ hasText: 'Paused-Model' });
  await expect(item).toHaveCount(1);
  await item.getByRole('button', { name: 'Remove from list' }).click();
  await expect(item).toHaveCount(0);
});

test('dismissal stays hidden on remove failure but does not hide a newer terminal attempt', async ({ page }) => {
  const firstCreatedAt = Date.now() - 10_000;
  let serverDownloads = [{
    id: 'model:Retry-Model', type: 'model', model_name: 'Retry-Model', status: 'error', running: false,
    created_at: firstCreatedAt,
    file: 'retry.gguf', file_index: 1, total_files: 1, bytes_downloaded: 100, bytes_total: 1000, percent: 10,
    error: 'first attempt failed',
  }];

  await page.route('**/api/v1/health**', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('**/api/v1/downloads**', route => route.fulfill({ json: { downloads: serverDownloads } }));
  await page.route('**/api/v1/downloads/control', route =>
    route.fulfill({ status: 500, json: { error: 'remove failed' } }),
  );

  await page.goto('/');
  await page.locator('.titlebar__download-toggle').click();
  let item = page.locator('.download-item').filter({ hasText: 'Retry-Model' });
  await expect(item).toHaveCount(1);
  await item.getByRole('button', { name: 'Remove from list' }).click();
  await expect(item).toHaveCount(0);

  serverDownloads = [{
    ...serverDownloads[0],
    created_at: firstCreatedAt + 5000,
    error: 'second attempt failed',
  }];
  await page.evaluate(() => window.dispatchEvent(new Event('focus')));
  item = page.locator('.download-item').filter({ hasText: 'Retry-Model' });
  await expect(item).toHaveCount(1);
  await expect(item).toContainText('second attempt failed');
});
