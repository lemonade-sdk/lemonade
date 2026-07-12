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

  await page.route('/api/v1/health', route =>
    route.fulfill({ json: { status: 'ok', all_models_loaded: [] } }),
  );
  await page.route('/api/v1/downloads**', route => route.fulfill({ json: { downloads: serverDownloads } }));

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
