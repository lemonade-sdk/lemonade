import { expect, test, type Page } from '@playwright/test';

const model = 'Qwen3.8-27B-GGUF';
const tinyPng = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAFElEQVR4nGPUCNjCAANMDEgANwcAOxABNO6ch9MAAAAASUVORK5CYII=',
  'base64',
);

const event = (delta: Record<string, unknown>, finishReason: string | null = null) =>
  `data: ${JSON.stringify({ id: 'mock-vision', choices: [{ index: 0, delta, finish_reason: finishReason }] })}\n\n`;
const complete = (delta: Record<string, unknown>, finishReason = 'stop') =>
  event(delta) + event({}, finishReason) + 'data: [DONE]\n\n';

async function mockVisionModel(page: Page, toolsEnabled = false) {
  if (!toolsEnabled) {
    await page.addInitScript(() => localStorage.setItem('lemonade:mcp_enabled', 'false'));
  }
  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => route.fulfill({
    json: {
      status: 'ok', version: 'test', model_loaded: model,
      all_models_loaded: [{
        model_name: model, type: 'llm', recipe: 'llamacpp', device: 'vulkan',
        input_modalities: ['text', 'image'], output_modalities: ['text'],
      }],
    },
  }));
  await page.route(/\/api\/v1\/models(?:\?.*)?$/, route => route.fulfill({
    json: { data: [{
      id: model, name: model, type: 'llm', recipe: 'llamacpp',
      labels: ['chat', 'vision', 'tool-calling'],
      input_modalities: ['text', 'image'], output_modalities: ['text'],
    }] },
  }));
}

async function sendImage(page: Page) {
  await page.goto('/');
  await expect(page.locator('.composer__input')).toHaveAttribute('placeholder', /Message Qwen3\.8-27B-GGUF/);
  await page.locator('.composer input[type="file"]').setInputFiles({
    name: 'tiny.png', mimeType: 'image/png', buffer: tinyPng,
  });
  await expect(page.locator('.composer__image-thumb')).toHaveCount(1);
  await page.locator('.composer__input').fill('what do you see');
  await page.locator('.composer__send').click();
}

test('vision length termination is reported as an error, not a blank reply', async ({ page }) => {
  await mockVisionModel(page);
  let requests = 0;
  await page.route(/\/api\/v1\/chat\/completions$/, async route => {
    requests++;
    await route.fulfill({
      status: 200, contentType: 'text/event-stream',
      body: complete({ reasoning_content: 'Analyzing the image...' }, 'length'),
    });
  });

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('finish_reason=length');
  expect(requests).toBe(1);
});

test('reasoning-only completions cannot silently succeed without a finish reason', async ({ page }) => {
  await mockVisionModel(page);
  await page.route(/\/api\/v1\/chat\/completions$/, route => route.fulfill({
    status: 200, contentType: 'text/event-stream',
    body: event({ reasoning_content: '...' }) + 'data: [DONE]\n\n',
  }));

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('no visible answer');
});

test('normal vision completions still work without a retry', async ({ page }) => {
  await mockVisionModel(page);
  await page.route(/\/api\/v1\/chat\/completions$/, route => route.fulfill({
    status: 200, contentType: 'text/event-stream',
    body: complete({ content: 'A screen beside a building.' }),
  }));

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('A screen beside a building.');
});

test('an explicit DONE without a trailing newline still completes successfully', async ({ page }) => {
  await mockVisionModel(page);
  await page.route(/\/api\/v1\/chat\/completions$/, route => route.fulfill({
    status: 200, contentType: 'text/event-stream',
    body: event({ content: 'A complete answer.' }) + 'data: [DONE]',
  }));

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('A complete answer.');
  await expect(page.locator('.message--error')).toHaveCount(0);
});

test('a stream without DONE is reported as an incomplete response', async ({ page }) => {
  await mockVisionModel(page);
  await page.route(/\/api\/v1\/chat\/completions$/, route => route.fulfill({
    status: 200, contentType: 'text/event-stream',
    body: event({ content: 'An unfinished reply.' }),
  }));

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('ended before [DONE]');
});

test('a length-terminated partial answer is not reported as a complete answer', async ({ page }) => {
  await mockVisionModel(page);
  await page.route(/\/api\/v1\/chat\/completions$/, route => route.fulfill({
    status: 200, contentType: 'text/event-stream',
    body: complete({ content: 'Partial text' }, 'length'),
  }));

  await sendImage(page);
  await expect(page.locator('.message--assistant').last()).toContainText('finish_reason=length');
});
