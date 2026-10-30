import { test, expect, type Page } from '@playwright/test';

const PERSIST_KEY = 'lemonade:persist_conversations';
const CONVERSATIONS_KEY = 'lemonade:conversations';

const conversation = (id: string | undefined, title: string) => ({
  ...(id ? { id } : {}),
  title,
  model: null,
  messages: [{ role: 'user', content: `${title} message` }],
  updatedAt: Date.now(),
  schemaVersion: 3,
});

// addInitScript runs on every navigation; the sessionStorage guard seeds once
// so later reloads see what the app itself wrote.
async function openWithSavingOff(page: Page, conversations: unknown[]) {
  await page.addInitScript(data => {
    if (sessionStorage.getItem('chat-history-seeded')) return;
    sessionStorage.setItem('chat-history-seeded', '1');
    localStorage.setItem(data.persistKey, 'false');
    localStorage.setItem(data.convKey, JSON.stringify({ version: 3, conversations: data.conversations }));
  }, { persistKey: PERSIST_KEY, convKey: CONVERSATIONS_KEY, conversations });
  await page.goto('/');
  await page.waitForSelector('.rail__list');
}

// Role locators skip the hidden mobile copy of the rail.
const railRow = (page: Page, title: string) =>
  page.getByRole('listbox', { name: 'Conversations' }).getByRole('option', { name: new RegExp(`^${title},`) });

async function deleteChat(page: Page, title: string) {
  const row = railRow(page, title);
  await row.hover();
  await row.locator(`button[aria-label="Delete conversation: ${title}"]`).click();
  await expect(row).toHaveCount(0);
}

async function storedTitles(page: Page): Promise<string[]> {
  return page.evaluate(key => {
    const parsed = JSON.parse(localStorage.getItem(key) || '{"conversations":[]}');
    return (parsed.conversations as { title: string }[]).map(c => c.title).sort();
  }, CONVERSATIONS_KEY);
}

test.describe('Chat history with saving turned off', () => {
  test('saved chats survive a reload', async ({ page }) => {
    await openWithSavingOff(page, [conversation('a', 'Alpha chat'), conversation('b', 'Beta chat')]);
    await page.reload();
    await expect(railRow(page, 'Alpha chat')).toHaveCount(1);
    await expect(railRow(page, 'Beta chat')).toHaveCount(1);
  });

  test('a deleted chat stays deleted and other chats remain', async ({ page }) => {
    await openWithSavingOff(page, [conversation('a', 'Alpha chat'), conversation('b', 'Beta chat')]);
    await expect(railRow(page, 'Beta chat')).toHaveCount(1);
    await deleteChat(page, 'Beta chat');
    await page.reload();
    await expect(railRow(page, 'Alpha chat')).toHaveCount(1);
    await expect(railRow(page, 'Beta chat')).toHaveCount(0);
  });

  test('deleting a chat keeps a chat another tab saved', async ({ page }) => {
    await openWithSavingOff(page, [conversation('a', 'Alpha chat'), conversation('b', 'Beta chat')]);
    await expect(railRow(page, 'Beta chat')).toHaveCount(1);
    await page.evaluate(({ key, entry }) => {
      const parsed = JSON.parse(localStorage.getItem(key) || '{"version":3,"conversations":[]}');
      parsed.conversations.push(entry);
      localStorage.setItem(key, JSON.stringify(parsed));
    }, { key: CONVERSATIONS_KEY, entry: conversation('x', 'Other tab chat') });
    await deleteChat(page, 'Beta chat');
    expect(await storedTitles(page)).toEqual(['Alpha chat', 'Other tab chat']);
  });

  test('a deleted legacy chat without an id stays deleted', async ({ page }) => {
    await openWithSavingOff(page, [conversation('a', 'Alpha chat'), conversation(undefined, 'Legacy chat')]);
    await expect(railRow(page, 'Legacy chat')).toHaveCount(1);
    const legacyStoredWithoutId = await page.evaluate(key => {
      const parsed = JSON.parse(localStorage.getItem(key) || '{"conversations":[]}');
      const entry = (parsed.conversations as { id?: string; title: string }[]).find(c => c.title === 'Legacy chat');
      return entry !== undefined && entry.id === undefined;
    }, CONVERSATIONS_KEY);
    expect(legacyStoredWithoutId, 'loading with saving off must not rewrite storage').toBe(true);
    await deleteChat(page, 'Legacy chat');
    await page.reload();
    await expect(railRow(page, 'Alpha chat')).toHaveCount(1);
    await expect(railRow(page, 'Legacy chat')).toHaveCount(0);
  });

  test('a deletion whose write failed is retried on the next change', async ({ page }) => {
    await openWithSavingOff(page, [
      conversation('a', 'Alpha chat'),
      conversation('b', 'Beta chat'),
      conversation('c', 'Gamma chat'),
    ]);
    await expect(railRow(page, 'Beta chat')).toHaveCount(1);
    await page.evaluate(() => {
      const original = Storage.prototype.setItem;
      (window as unknown as { restoreSetItem: () => void }).restoreSetItem = () => { Storage.prototype.setItem = original; };
      Storage.prototype.setItem = () => { throw new Error('blocked'); };
    });
    await deleteChat(page, 'Beta chat');
    expect(await storedTitles(page)).toEqual(['Alpha chat', 'Beta chat', 'Gamma chat']);
    await page.evaluate(() => (window as unknown as { restoreSetItem: () => void }).restoreSetItem());
    await deleteChat(page, 'Gamma chat');
    expect(await storedTitles(page)).toEqual(['Alpha chat']);
  });

  test('turning saving back on saves the chats on screen', async ({ page }) => {
    await openWithSavingOff(page, [conversation('a', 'Alpha chat'), conversation('b', 'Beta chat')]);
    await expect(railRow(page, 'Beta chat')).toHaveCount(1);
    await deleteChat(page, 'Beta chat');
    await page.evaluate(key => {
      localStorage.setItem(key, 'true');
      window.dispatchEvent(new CustomEvent('lemonade:chat-history-preference-changed', { detail: true }));
    }, PERSIST_KEY);
    await expect.poll(() => storedTitles(page)).toEqual(['Alpha chat']);
    await page.reload();
    await expect(railRow(page, 'Alpha chat')).toHaveCount(1);
    await expect(railRow(page, 'Beta chat')).toHaveCount(0);
  });
});
