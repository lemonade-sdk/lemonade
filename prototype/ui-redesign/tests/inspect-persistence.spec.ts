import { test, expect } from '@playwright/test';

test('Session Inspector - Local Storage Persistence', async ({ page }) => {
  const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS, PUT, PATCH, DELETE',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Client-Session-Id, X-Account-Session-Id',
  };

  // Intercept all API requests to handle CORS preflight OPTIONS
  await page.route(/\/api\/v1\//, async route => {
    if (route.request().method() === 'OPTIONS') {
      await route.fulfill({
        status: 204,
        headers: corsHeaders
      });
    } else {
      await route.continue();
    }
  });

  // Mock health check
  await page.route(/\/api\/v1\/health/, async route => {
    if (route.request().method() === 'OPTIONS') {
      await route.fulfill({ status: 204, headers: corsHeaders });
      return;
    }
    await route.fulfill({
      status: 200,
      headers: corsHeaders,
      contentType: 'application/json',
      body: JSON.stringify({
        status: 'ok',
        version: '1.0.0',
        websocket_port: 9000,
        all_models_loaded: []
      })
    });
  });

  // Mock models list
  await page.route(/\/api\/v1\/models/, async route => {
    if (route.request().method() === 'OPTIONS') {
      await route.fulfill({ status: 204, headers: corsHeaders });
      return;
    }
    await route.fulfill({
      status: 200,
      headers: corsHeaders,
      contentType: 'application/json',
      body: JSON.stringify({
        data: [
          { id: 'mock-model-1', name: 'mock-model-1', model_name: 'mock-model-1' }
        ]
      })
    });
  });

  // Mock chat completions
  await page.route(/\/api\/v1\/chat\/completions/, async route => {
    if (route.request().method() === 'OPTIONS') {
      await route.fulfill({ status: 204, headers: corsHeaders });
      return;
    }
    await route.fulfill({
      status: 200,
      headers: corsHeaders,
      contentType: 'application/json',
      body: JSON.stringify({
        choices: [
          { message: { role: 'assistant', content: 'Mocked completion response.' } }
        ]
      })
    });
  });

  // 1. Navigate to the inspect view directly
  await page.goto('/#/inspect');
  await page.waitForTimeout(1000);

  // 2. Verify that capturing is off by default
  const captureSwitch = page.locator('.switch-control');
  await expect(captureSwitch).toBeVisible();

  // Checking aria-checked attribute on the switch
  let isCapturing = await captureSwitch.getAttribute('aria-checked');
  expect(isCapturing).toBe('false');

  // 3. Toggle capturing ON
  await captureSwitch.click();
  await page.waitForTimeout(200);
  isCapturing = await captureSwitch.getAttribute('aria-checked');
  expect(isCapturing).toBe('true');

  // 4. Reload page and check if capturing state is persisted
  await page.reload();
  await page.waitForTimeout(1000);
  const captureSwitchReloaded = page.locator('.switch-control');
  const isCapturingReloaded = await captureSwitchReloaded.getAttribute('aria-checked');
  expect(isCapturingReloaded).toBe('true');

  // 5. Generate a dummy trace by opening "+ Create" composer modal and submitting
  await page.click('button:has-text("+ Create")');
  await page.waitForTimeout(500);

  // Click the model search box inside the modal
  const modelSearch = page.locator('input[placeholder="Search model..."]').first();
  await modelSearch.click();
  await page.waitForTimeout(200);

  // Select the mocked model option
  const firstItem = page.locator('.model-search-item').first();
  await expect(firstItem).toBeVisible();
  await firstItem.click();
  await page.waitForTimeout(200);

  // Submit the request in modal
  await page.click('.inspect-modal-footer button.primary-simulate');
  await page.evaluate(() => {
    const trace = {
      id: 'mock-trace-id-1',
      traceId: 'mocktrace123',
      spanId: 'mockspan123',
      kind: 'LLM',
      operation: 'chat.completions',
      status: 'ok',
      model: 'mock-model-1',
      timestamp: new Date().toLocaleTimeString(),
      startTimeMs: Date.now(),
      dur: 1500,
      messages: [
        { role: 'user', content: 'Tell me a joke about compiler optimizations.' },
        { role: 'assistant', content: 'Mocked completion response.' }
      ],
      output: 'Mocked completion response.'
    };
    (window as any).inspectStore.setState({
      traces: [trace],
      selectedTraceId: trace.id
    });
  });
  await page.waitForTimeout(1000);

  // Verify a trace was captured in the left rail list
  const traceItems = page.locator('.trace-row');
  await expect(traceItems).toHaveCount(1);
  const originalTraceId = await traceItems.first().getAttribute('data-trace-id');

  // 6. Reload page and check if the trace survived the reload
  await page.reload();
  await page.waitForTimeout(1000);

  const traceItemsReloaded = page.locator('.trace-row');
  await expect(traceItemsReloaded).toHaveCount(1);
  const reloadedTraceId = await traceItemsReloaded.first().getAttribute('data-trace-id');
  expect(reloadedTraceId).toBe(originalTraceId);

  // 7. Click "Clear" specifically in the inspect rail footer
  await page.click('.inspect-rail__footer button:has-text("Clear")');
  await page.waitForTimeout(500);
  await expect(page.locator('.trace-row')).toHaveCount(0);

  // 8. Reload again and check if it's still cleared
  await page.reload();
  await page.waitForTimeout(1000);
  await expect(page.locator('.trace-row')).toHaveCount(0);
});
