import { expect, test, type Page } from '@playwright/test';

const oldModel = 'handoff-old-model';
const newModel = 'handoff-new-model';

const snapshot = (name: string) => ({ name, type: 'llm', capability: 'chat', recipe: 'llamacpp' });
const conversation = (id: string, title: string, updatedAt: number) => ({
  id,
  title,
  model: snapshot(oldModel),
  messages: [
    { role: 'user', content: `${title} question`, model: snapshot(oldModel) },
    { role: 'assistant', content: `${title} answer`, model: snapshot(oldModel) },
  ],
  updatedAt,
});

const sse = (content: string) => [
  `data: ${JSON.stringify({ id: 'handoff', choices: [{ index: 0, delta: { content }, finish_reason: null }] })}\n\n`,
  `data: ${JSON.stringify({ id: 'handoff', choices: [{ index: 0, delta: {}, finish_reason: 'stop' }] })}\n\n`,
  'data: [DONE]\n\n',
].join('');

// A restarted server has nothing resident, while the browser still holds a
// conversation that last answered on oldModel and reopens it on startup.
async function mockRestartedServer(page: Page) {
  const loaded = new Set<string>();
  const loadRequests: string[] = [];
  const pullRequests: string[] = [];
  const completionModels: string[] = [];

  await page.addInitScript(({ conversations, activeId }) => {
    if (sessionStorage.getItem('handoff-seeded')) return;
    sessionStorage.setItem('handoff-seeded', '1');
    localStorage.setItem('lemonade:mcp_enabled', 'false');
    localStorage.setItem('lemonade:persist_conversations', 'true');
    localStorage.setItem('lemonade:conversations', JSON.stringify({ version: 3, conversations }));
    localStorage.setItem('lemonade:active_conversation', activeId);
  }, {
    conversations: [
      conversation('current-thread', 'Current thread', Date.now()),
      conversation('older-thread', 'Older thread', Date.now() - 60_000),
    ],
    activeId: 'current-thread',
  });

  await page.route(/\/api\/v1\/health(?:\?.*)?$/, route => route.fulfill({
    json: {
      status: 'ok',
      version: 'test',
      all_models_loaded: [...loaded].map(name => ({
        model_name: name, type: 'llm', recipe: 'llamacpp', device: 'cpu',
      })),
    },
  }));
  await page.route(/\/api\/v1\/system-info(?:\?.*)?$/, route => route.fulfill({
    json: {
      recipes: {
        llamacpp: {
          default_backend: 'cpu',
          selectable_backend: true,
          uses_ctx_size: true,
          options: [],
          backends: { cpu: { state: 'installed', version: 'test' } },
        },
      },
    },
  }));
  await page.route(/\/api\/v1\/models(?:\/[^?]*)?(?:\?.*)?$/, route => {
    const optionsMatch = new URL(route.request().url()).pathname.match(/\/api\/v1\/models\/([^/]+)\/options$/);
    if (optionsMatch) {
      const name = decodeURIComponent(optionsMatch[1]);
      return route.fulfill({
        json: { model_name: name, recipe: 'llamacpp', saved: {}, effective: { model_name: name }, defaults: { model_name: name } },
      });
    }
    return route.fulfill({
      json: {
        data: [oldModel, newModel].map(name => ({
          id: name, name, labels: ['chat'], recipe: 'llamacpp', downloaded: true,
        })),
      },
    });
  });
  await page.route(/\/api\/v1\/load$/, route => {
    const name = String((route.request().postDataJSON() as Record<string, unknown>).model_name);
    loadRequests.push(name);
    loaded.clear();
    loaded.add(name);
    return route.fulfill({ json: { status: 'success', model_name: name } });
  });
  await page.route(/\/api\/v1\/pull$/, route => {
    pullRequests.push(String((route.request().postDataJSON() as Record<string, unknown>).model_name));
    return route.fulfill({ status: 500, json: { error: 'unexpected pull' } });
  });
  await page.route(/\/api\/v1\/chat\/completions$/, route => {
    completionModels.push(String((route.request().postDataJSON() as Record<string, unknown>).model));
    return route.fulfill({ status: 200, contentType: 'text/event-stream', body: sse('Answered by the new model.') });
  });

  return { loadRequests, pullRequests, completionModels };
}

test('a model loaded from Models replaces the restored conversation model in chat', async ({ page }) => {
  const server = await mockRestartedServer(page);
  const composer = page.locator('.composer__input');

  await page.goto('/');
  await expect(page.locator('#rail-conv-current-thread')).toHaveAttribute('aria-selected', 'true');
  await expect(composer).toHaveAttribute('placeholder', new RegExp(`Message ${oldModel}`));

  await page.locator('.titlebar__nav').getByText('Models').click();
  await page.locator('.model-list-panel__list .workspace-list-row').filter({ hasText: newModel }).click();
  await page.getByRole('button', { name: `Load ${newModel}` }).click();

  await expect(composer).toBeVisible();
  await expect(composer).toHaveAttribute('placeholder', new RegExp(`Message ${newModel}`));

  await composer.fill('Which model is this?');
  await page.locator('.composer__send').click();
  await expect(page.locator('.message--assistant').last()).toContainText('Answered by the new model.');

  expect(server.completionModels).toEqual([newModel]);
  expect(server.loadRequests).toEqual([newModel]);
  expect(server.pullRequests).toEqual([]);

  // Reopening a conversation by hand still brings back the model it was using.
  await page.locator('#rail-conv-older-thread').click();
  await expect(composer).toHaveAttribute('placeholder', new RegExp(`Message ${oldModel}`));
});
