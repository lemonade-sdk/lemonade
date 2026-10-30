import { expect, test, type Page } from '@playwright/test';

const model = 'Qwen3.8-27B-GGUF';
const tinyPng = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAFElEQVR4nGPUCNjCAANMDEgANwcAOxABNO6ch9MAAAAASUVORK5CYII=',
  'base64',
);
const PENDING_KEY = 'lemonade:pending_browser_prompt';

const event = (delta: Record<string, unknown>, finishReason: string | null = null) =>
  `data: ${JSON.stringify({ id: 'mock-browser-prompt', choices: [{ index: 0, delta, finish_reason: finishReason }] })}\n\n`;
const complete = (text: string) => event({ content: text }) + event({}, 'stop') + 'data: [DONE]\n\n';

type MockServer = { loaded: boolean; healthChecks: number; requests: any[] };

async function mockServer(page: Page, loaded: boolean): Promise<MockServer> {
  const mock: MockServer = { loaded, healthChecks: 0, requests: [] };
  await page.addInitScript(() => localStorage.setItem('lemonade:mcp_enabled', 'false'));
  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => {
    mock.healthChecks++;
    return route.fulfill({
      json: {
        status: 'ok', version: 'test', model_loaded: mock.loaded ? model : null,
        all_models_loaded: mock.loaded ? [{
          model_name: model, type: 'llm', recipe: 'llamacpp', device: 'vulkan',
          input_modalities: ['text', 'image'], output_modalities: ['text'],
        }] : [],
      },
    });
  });
  await page.route(/\/api\/v1\/models(?:\?.*)?$/, route => route.fulfill({
    json: { data: [{
      id: model, name: model, type: 'llm', recipe: 'llamacpp', downloaded: true,
      labels: ['chat', 'vision', 'tool-calling'],
      input_modalities: ['text', 'image'], output_modalities: ['text'],
    }] },
  }));
  await page.route(/\/api\/v1\/load$/, route => {
    mock.loaded = true;
    return route.fulfill({ json: { status: 'success' } });
  });
  await page.route(/\/api\/v1\/chat\/completions$/, route => {
    mock.requests.push(JSON.parse(route.request().postData() || '{}'));
    return route.fulfill({ status: 200, contentType: 'text/event-stream', body: complete('ok') });
  });
  return mock;
}

function seedExistingConversation(page: Page) {
  return page.addInitScript(() => {
    if (sessionStorage.getItem('browser-prompt-seeded')) return;
    sessionStorage.setItem('browser-prompt-seeded', '1');
    localStorage.setItem('lemonade:conversations', JSON.stringify({
      version: 3,
      conversations: [{
        id: 'existing', title: 'Existing chat', model: { name: 'Qwen3.8-27B-GGUF' }, updatedAt: Date.now(), schemaVersion: 3,
        messages: [
          { role: 'user', content: 'earlier question' },
          { role: 'assistant', content: 'earlier answer' },
        ],
      }],
    }));
    localStorage.setItem('lemonade:active_conversation', 'existing');
  });
}

async function openWithPendingPrompt(page: Page, mock: MockServer, prompt = 'Webpage prompt') {
  await page.goto(`/?q=${encodeURIComponent(prompt)}`);
  await expect(page.locator('.composer__input')).toHaveValue(prompt);
  await expect.poll(() => mock.healthChecks).toBeGreaterThan(0);
  await expect(page.getByText(/Imported webpage content/)).toBeVisible();
}

const requestText = (request: any) => JSON.stringify(request.messages);
const storedConversations = (page: Page) => page.evaluate(() => (
  JSON.parse(localStorage.getItem('lemonade:conversations') || '{"conversations":[]}').conversations
));
const pendingDraft = (page: Page) => page.evaluate(key => sessionStorage.getItem(key), PENDING_KEY);

test('a URL prompt is sent automatically without tools when the model is loaded', async ({ page }) => {
  const mock = await mockServer(page, true);
  await page.goto('/?q=Summarize%20this');
  await expect.poll(() => mock.requests.length).toBe(1);
  expect(requestText(mock.requests[0])).toContain('Summarize this');
  expect(mock.requests[0].tools).toBeUndefined();
  await expect(page).toHaveURL(/\/#\/chat$/);
});

test('opening an existing chat releases the waiting prompt so follow-ups stay in that chat', async ({ page }) => {
  const mock = await mockServer(page, false);
  await seedExistingConversation(page);
  await openWithPendingPrompt(page, mock);

  await page.getByText('Existing chat', { exact: true }).first().click();
  await expect(page.locator('.message--user')).toContainText('earlier question');
  await expect(page.locator('.composer__input')).toHaveValue('');
  await expect(page.getByText(/Imported webpage content/)).toHaveCount(0);
  expect(await pendingDraft(page)).toBeNull();

  await page.locator('.composer__input').fill('my follow-up');
  await page.locator('.composer__input').press('Enter');
  await expect.poll(() => mock.requests.length).toBe(1);
  expect(requestText(mock.requests[0])).toContain('earlier question');
  const conversations = await storedConversations(page);
  expect(conversations).toHaveLength(1);
  expect(conversations[0].messages.some((message: any) => message.fromBrowserPrompt)).toBe(false);
});

test('a released prompt is not sent later when the user starts a new chat', async ({ page }) => {
  test.setTimeout(90000);
  const mock = await mockServer(page, false);
  await seedExistingConversation(page);
  await openWithPendingPrompt(page, mock);
  await page.getByText('Existing chat', { exact: true }).first().click();

  // Another client loads the model; the app notices on its next health poll.
  mock.loaded = true;
  const checks = mock.healthChecks;
  await expect.poll(() => mock.healthChecks, { timeout: 30000 }).toBeGreaterThan(checks);
  await page.getByRole('button', { name: /New chat/ }).first().click();
  await page.waitForTimeout(1500);
  expect(mock.requests).toHaveLength(0);
});

test('images attached to the waiting prompt are sent with it', async ({ page }) => {
  const mock = await mockServer(page, false);
  await openWithPendingPrompt(page, mock, 'Describe this');
  await page.locator('.composer input[type="file"]').setInputFiles({ name: 'tiny.png', mimeType: 'image/png', buffer: tinyPng });
  await expect(page.locator('.composer__image-thumb')).toHaveCount(1);
  await page.locator('.composer__input').press('Enter');

  await expect.poll(() => mock.requests.length).toBe(1);
  expect(requestText(mock.requests[0])).toContain('image_url');
  expect(mock.requests[0].tools).toBeUndefined();
  await expect(page.locator('.composer__image-thumb')).toHaveCount(0);
});

test('a starter prompt replaces the waiting prompt and keeps tools available', async ({ page }) => {
  const mock = await mockServer(page, false);
  await openWithPendingPrompt(page, mock);
  await page.getByRole('listitem').filter({ hasText: 'What are Lemonade tools?' }).click();
  await expect(page.getByText(/Imported webpage content/)).toHaveCount(0);
  expect(await pendingDraft(page)).toBeNull();

  await page.reload();
  await expect(page.locator('.composer__input')).not.toHaveValue('Webpage prompt');
});

test('refresh keeps the chat the user opened after releasing the prompt', async ({ page }) => {
  const mock = await mockServer(page, false);
  await seedExistingConversation(page);
  await openWithPendingPrompt(page, mock);
  await page.getByText('Existing chat', { exact: true }).first().click();
  await expect(page.locator('.message--user')).toContainText('earlier question');

  await page.reload();
  await expect(page.locator('.message--user')).toContainText('earlier question');
  await expect(page.locator('.composer__input')).toHaveValue('');
});

test('edits to the waiting prompt survive a reload and are what gets sent', async ({ page }) => {
  const mock = await mockServer(page, false);
  await openWithPendingPrompt(page, mock);
  await page.locator('.composer__input').fill('Edited webpage prompt');
  await expect.poll(() => pendingDraft(page)).toBe('Edited webpage prompt');

  const checks = mock.healthChecks;
  await page.reload();
  await expect(page.locator('.composer__input')).toHaveValue('Edited webpage prompt');
  await expect.poll(() => mock.healthChecks).toBeGreaterThan(checks);
  await page.locator('.composer__input').press('Enter');
  await expect.poll(() => mock.requests.length).toBe(1);
  expect(requestText(mock.requests[0])).toContain('Edited webpage prompt');
  expect(mock.requests[0].tools).toBeUndefined();
  expect(await pendingDraft(page)).toBeNull();
});
