import { test, expect } from '@playwright/test';

const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS, PUT, PATCH, DELETE',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Client-Session-Id, X-Account-Session-Id',
};

test.beforeEach(async ({ page }) => {
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
});

test.describe('Session Inspector - Recent Improvements', () => {

  test('Decoupled Keyboard Selection in Trace List', async ({ page }) => {
    // 1. Navigate to the inspect view directly
    await page.goto('/#/inspect');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    // 2. Put initial traces in the inspectStore
    await page.evaluate(() => {
      const trace1 = {
        id: 'mock-trace-1',
        traceId: 'trace-1',
        spanId: 'span-1',
        kind: 'LLM' as const,
        operation: 'chat.completions',
        status: 'ok' as const,
        model: 'mock-model-1',
        timestamp: '12:00:00 PM',
        startTimeMs: Date.now() - 10000,
        dur: 1500,
        messages: [{ role: 'user' as const, content: 'First message' }],
        output: 'First output'
      };

      const trace2 = {
        id: 'mock-trace-2',
        traceId: 'trace-2',
        spanId: 'span-2',
        kind: 'LLM' as const,
        operation: 'chat.completions',
        status: 'ok' as const,
        model: 'mock-model-1',
        timestamp: '12:01:00 PM',
        startTimeMs: Date.now() - 5000,
        dur: 2000,
        messages: [{ role: 'user' as const, content: 'Second message' }],
        output: 'Second output'
      };

      const trace3 = {
        id: 'mock-trace-3',
        traceId: 'trace-3',
        spanId: 'span-3',
        kind: 'LLM' as const,
        operation: 'chat.completions',
        status: 'error' as const,
        model: 'mock-model-1',
        timestamp: '12:02:00 PM',
        startTimeMs: Date.now(),
        dur: 500,
        messages: [{ role: 'user' as const, content: 'Third message' }],
        output: 'Third output'
      };

      (window as any).inspectStore.setState({
        traces: [trace3, trace2, trace1], // newest first
        selectedTraceId: 'mock-trace-3'
      });
    });

    await page.waitForTimeout(200);

    // Check trace rows are visible
    const traceRows = page.locator('.trace-row');
    await expect(traceRows).toHaveCount(3);

    // Get the first item (which should have tabIndex=0 and be mock-trace-3)
    const firstOption = page.locator('button[role="option"][data-trace-id="mock-trace-3"]');
    await expect(firstOption).toBeVisible();
    await firstOption.focus();
    await expect(firstOption).toBeFocused();

    // Verify initial selected state
    let selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-3');

    // ArrowDown should move active focus to mock-trace-2, but NOT select it
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(100);

    const secondOption = page.locator('button[role="option"][data-trace-id="mock-trace-2"]');
    await expect(secondOption).toBeFocused();

    // Verify selectedTraceId is STILL mock-trace-3 (decoupled)
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-3');
    await expect(firstOption).toHaveAttribute('aria-selected', 'true');
    await expect(secondOption).toHaveAttribute('aria-selected', 'false');

    // ArrowDown again to mock-trace-1
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(100);

    const thirdOption = page.locator('button[role="option"][data-trace-id="mock-trace-1"]');
    await expect(thirdOption).toBeFocused();

    // Verify selectedTraceId is STILL mock-trace-3
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-3');

    // Press Enter to trigger selection on the focused mock-trace-1
    await page.keyboard.press('Enter');
    await page.waitForTimeout(100);

    // Verify selectedTraceId is now mock-trace-1
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-1');
    await expect(firstOption).toHaveAttribute('aria-selected', 'false');
    await expect(thirdOption).toHaveAttribute('aria-selected', 'true');

    // ArrowUp to mock-trace-2
    await page.keyboard.press('ArrowUp');
    await page.waitForTimeout(100);
    await expect(secondOption).toBeFocused();

    // Verify selectedTraceId is STILL mock-trace-1
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-1');

    // Press Space to trigger selection on the focused mock-trace-2
    await page.keyboard.press(' ');
    await page.waitForTimeout(100);

    // Verify selectedTraceId is now mock-trace-2
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-2');
    await expect(thirdOption).toHaveAttribute('aria-selected', 'false');
    await expect(secondOption).toHaveAttribute('aria-selected', 'true');
  });

  test('Combobox Input Focus Highlight', async ({ page }) => {
    // 1. Navigate to the inspect view directly
    await page.goto('/#/inspect');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    // Open Sim Create Modal
    await page.click('button.primary-simulate');
    await page.waitForTimeout(500);

    // Locate the search input
    const input = page.locator('input[placeholder="Search model..."]').first();
    await expect(input).toBeVisible();

    // Select the model
    await input.click();
    await page.waitForTimeout(200);

    const firstItem = page.locator('.model-search-item').first();
    await expect(firstItem).toBeVisible();
    await firstItem.click();
    await page.waitForTimeout(200);

    // Verify value is populated
    await expect(input).toHaveValue('mock-model-1');

    // Blur the input
    await input.blur();
    await page.waitForTimeout(100);

    // Focus input and verify highlight text selection + value retained
    await input.focus();
    await page.waitForTimeout(150); // let setTimeout(..., 0) run

    const selectionInfo = await input.evaluate((el: HTMLInputElement) => ({
      value: el.value,
      start: el.selectionStart,
      end: el.selectionEnd
    }));

    expect(selectionInfo.value).toBe('mock-model-1');
    expect(selectionInfo.start).toBe(0);
    expect(selectionInfo.end).toBe(12); // 'mock-model-1'.length is 12
  });

  test('WebSocket Reconnection Security', async ({ page }) => {
    // Add init script to mock WebSocket before page load
    await page.addInitScript(() => {
      const instances: any[] = [];
      class MockWebSocket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;

        url: string;
        readyState: number;
        onopen: any = null;
        onclose: any = null;
        onmessage: any = null;
        onerror: any = null;

        constructor(url: string) {
          this.url = url;
          this.readyState = MockWebSocket.CONNECTING;
          instances.push(this);

          setTimeout(() => {
            if (this.readyState === MockWebSocket.CONNECTING) {
              this.readyState = MockWebSocket.OPEN;
              if (this.onopen) {
                this.onopen();
              }
            }
          }, 10);
        }

        send(data: string) {
          // Dummy send
        }

        close() {
          if (this.readyState !== MockWebSocket.CLOSED) {
            this.readyState = MockWebSocket.CLOSED;
            if (this.onclose) {
              this.onclose();
            }
          }
        }
      }

      (window as any).wsInstances = instances;
      (window as any).WebSocket = MockWebSocket as any;
    });

    // Handle and verify page errors
    const errors: Error[] = [];
    page.on('pageerror', (err) => {
      errors.push(err);
    });

    // Navigate to inspect view
    await page.goto('/#/inspect');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    // Make sure capturing is set to true on store
    await page.evaluate(() => {
      (window as any).inspectStore.setState({ capturing: true });
    });
    await page.waitForTimeout(200);

    // Verify we have active WS instance
    let initialCount = await page.evaluate(() => (window as any).wsInstances.length);
    expect(initialCount).toBeGreaterThan(0);

    // Force close active WebSocket multiple times
    await page.evaluate(() => {
      const instances = (window as any).wsInstances;
      if (instances.length > 0) {
        instances[instances.length - 1].close();
      }
    });

    await page.waitForTimeout(200);

    // Trigger explicit reconnects and check for robustness
    for (let i = 0; i < 5; i++) {
      await page.evaluate(() => {
        (window as any).inspectStore.reconnect();
      });
    }

    await page.waitForTimeout(500);

    // Push simulated Span via websocket messages to make sure it handles incoming messages on new WS
    await page.evaluate(() => {
      const instances = (window as any).wsInstances;
      const ws = instances[instances.length - 1];
      if (ws && ws.onmessage) {
        ws.onmessage({
          data: JSON.stringify({
            traceId: 'ws-trace-id-123',
            spanId: 'ws-span-id-123',
            startTimeUnixNano: '1720311234000000000',
            endTimeUnixNano: '1720311235000000000',
            attributes: [
              { key: 'openinference.span.kind', value: { stringValue: 'LLM' } },
              { key: 'llm.model_name', value: { stringValue: 'mock-model-1' } },
              { key: 'input.value', value: { stringValue: 'WS Hello' } },
              { key: 'output.value', value: { stringValue: 'WS World' } }
            ]
          })
        });
      }
    });

    await page.waitForTimeout(500);

    // Ensure trace was captured and added from WebSocket
    const traceRows = page.locator('.trace-row');
    const count = await traceRows.count();
    expect(count).toBeGreaterThan(0);

    // Confirm no errors were thrown
    expect(errors).toHaveLength(0);
  });

  test('Telemetry Thinking Parser, Collapse, Copy, and Scroll Constraints', async ({ page, context }) => {
    // Grant clipboard permissions for copy assertion
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);

    // Setup WebSocket Mock in this test
    await page.addInitScript(() => {
      const instances: any[] = [];
      class MockWebSocket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;

        url: string;
        readyState: number;
        onopen: any = null;
        onclose: any = null;
        onmessage: any = null;
        onerror: any = null;

        constructor(url: string) {
          this.url = url;
          this.readyState = MockWebSocket.CONNECTING;
          instances.push(this);

          setTimeout(() => {
            if (this.readyState === MockWebSocket.CONNECTING) {
              this.readyState = MockWebSocket.OPEN;
              if (this.onopen) this.onopen();
            }
          }, 10);
        }

        send(data: string) {}
        close() {
          if (this.readyState !== MockWebSocket.CLOSED) {
            this.readyState = MockWebSocket.CLOSED;
            if (this.onclose) this.onclose();
          }
        }
      }
      (window as any).wsInstances = instances;
      (window as any).WebSocket = MockWebSocket as any;
    });

    // 1. Navigate to the inspect view
    await page.goto('/#/inspect');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    // Make sure capturing is set to true on store
    await page.evaluate(() => {
      (window as any).inspectStore.setState({ capturing: true });
    });
    await page.waitForTimeout(200);

    // 2. Push raw OTel span containing leading <think> tags and nested <think> blocks
    await page.evaluate(() => {
      const rawContent = `<think>
This is the leading thinking block.
</think>
Here is how you do it:
\`\`\`xml
<think>
literal inside code
</think>
\`\`\`
Followed by some normal text.`;

      const instances = (window as any).wsInstances;
      const ws = instances[instances.length - 1];
      if (ws && ws.onmessage) {
        ws.onmessage({
          data: JSON.stringify({
            traceId: 'trace-thinking-spec-123',
            spanId: 'span-thinking-spec-123',
            startTimeUnixNano: '1720311234000000000',
            endTimeUnixNano: '1720311235000000000',
            attributes: [
              { key: 'openinference.span.kind', value: { stringValue: 'LLM' } },
              { key: 'llm.model_name', value: { stringValue: 'mock-model-thinking-spec' } },
              { key: 'llm.input_messages.0.message.role', value: { stringValue: 'system' } },
              { key: 'llm.input_messages.0.message.content', value: { stringValue: 'You are a helpful assistant.' } },
              { key: 'llm.input_messages.1.message.role', value: { stringValue: 'user' } },
              { key: 'llm.input_messages.1.message.content', value: { stringValue: 'How do you represent literal <think> in XML?' } },
              { key: 'output.value', value: { stringValue: rawContent } }
            ]
          })
        });
      }
    });

    await page.waitForTimeout(500);

    // Push Trace B (second trace to verify render state is not shared/reused)
    await page.evaluate(() => {
      const instances = (window as any).wsInstances;
      const ws = instances[instances.length - 1];
      if (ws && ws.onmessage) {
        ws.onmessage({
          data: JSON.stringify({
            traceId: 'trace-thinking-spec-abc',
            spanId: 'span-thinking-spec-abc',
            startTimeUnixNano: '1720311244000000000',
            endTimeUnixNano: '1720311245000000000',
            attributes: [
              { key: 'openinference.span.kind', value: { stringValue: 'LLM' } },
              { key: 'llm.model_name', value: { stringValue: 'mock-model-thinking-spec' } },
              { key: 'input.value', value: { stringValue: 'Second Trace Prompt' } },
              { key: 'output.value', value: { stringValue: 'Second Trace Output content.' } }
            ]
          })
        });
      }
    });

    await page.waitForTimeout(500);

    // Select the first trace
    await page.locator('.trace-row').last().click(); // newest is first, so trace 1 is last
    await page.waitForTimeout(200);

    // Click on the Messages tab
    const messagesTabButton = page.locator('button[role="tab"]:has-text("Messages")');
    await expect(messagesTabButton).toBeVisible();
    await messagesTabButton.click();
    await page.waitForTimeout(200);

    // A. Verify Secure-by-Default card posture: Card defaults to Raw, while global is Rendered
    const globalRenderedBtn = page.locator('.messages-toolbar__left button:has-text("Rendered")');
    await expect(globalRenderedBtn).toHaveAttribute('aria-pressed', 'true');

    const assistantCard = page.locator('.message-card').last();
    const cardToggleSegmented = assistantCard.locator('.message-card__toggle-segmented');
    await expect(cardToggleSegmented.locator('button:has-text("Raw")')).toHaveClass(/active/);
    await expect(assistantCard.locator('pre.raw-text-body')).toBeVisible();

    // B. Verify strict parsing doesn't strip literal <think> inside code blocks
    const rawContentText = await assistantCard.locator('pre.raw-text-body').innerText();
    expect(rawContentText).toContain('<think>\nThis is the leading thinking block.\n</think>');
    expect(rawContentText).toContain('<think>\nliteral inside code\n</think>');

    // C. Click "Rendered" on the card to check collapsible reasoning blocks
    await cardToggleSegmented.locator('button:has-text("Rendered")').click();
    await page.waitForTimeout(200);

    // Verification of collapsible reasoning header (aria-expanded = false by default)
    const reasoningHeader = assistantCard.locator('.reasoning-block__header');
    await expect(reasoningHeader).toBeVisible();
    await expect(reasoningHeader).toHaveAttribute('aria-expanded', 'false');

    const reasoningBody = assistantCard.locator('.reasoning-block__body');
    await expect(reasoningBody).not.toBeVisible();

    // Toggle reasoning open
    await reasoningHeader.click();
    await page.waitForTimeout(100);
    await expect(reasoningHeader).toHaveAttribute('aria-expanded', 'true');
    await expect(reasoningBody).toBeVisible();
    await expect(reasoningBody).toHaveText('This is the leading thinking block.');

    // Body text in Rendered view mode should NOT show the leading thinking block, but MUST retain the literal code block think tag
    const renderedBody = assistantCard.locator('.message-card__body .fade-in').first();
    await expect(renderedBody).toBeVisible();
    const renderedText = await renderedBody.innerText();
    expect(renderedText).not.toContain('This is the leading thinking block.');
    expect(renderedText).toContain('<think>\nliteral inside code\n</think>');

    // E. Verify copy/export functionality includes raw unstripped content & explicit thinking
    const copyButton = page.locator('.split-button__action');
    await expect(copyButton).toBeVisible();
    await copyButton.click();
    await page.waitForTimeout(100);

    // Read clipboard content in browser context
    const clipboardText = await page.evaluate(async () => navigator.clipboard.readText());
    const parsedClipboard = JSON.parse(clipboardText);
    expect(parsedClipboard).toHaveLength(3);
    expect(parsedClipboard[2].role).toBe('assistant');
    // Ensure raw unstripped content is exported
    expect(parsedClipboard[2].content).toContain('<think>\nThis is the leading thinking block.\n</think>');
    // Ensure thinking property is explicitly exported
    expect(parsedClipboard[2].thinking).toBe('This is the leading thinking block.');

    // F. REGRESSION TEST: Select the second trace and verify that the card rendering state resets to default 'raw'
    await page.locator('.trace-row').first().click(); // Trace B is the first row
    await page.waitForTimeout(200);

    // Click on the Messages tab again (as changing selectedTraceId resets activeTab to 'overview')
    await page.locator('button[role="tab"]:has-text("Messages")').click();
    await page.waitForTimeout(200);

    // Card in Trace B should default back to Raw
    const assistantCardB = page.locator('.message-card').last();
    const cardToggleSegmentedB = assistantCardB.locator('.message-card__toggle-segmented');
    await expect(cardToggleSegmentedB.locator('button:has-text("Raw")')).toHaveClass(/active/);
    await expect(assistantCardB.locator('pre.raw-text-body')).toBeVisible();

    // D. Verify height constraints on the layout
    const vcHeight = await page.locator('.view-container').evaluate(el => el.getBoundingClientRect().height);
    const layoutHeight = await page.locator('.inspect-layout').evaluate(el => el.getBoundingClientRect().height);
    const railHeight = await page.locator('.inspect-rail').evaluate(el => el.getBoundingClientRect().height);
    const detailHeight = await page.locator('.inspect-detail').evaluate(el => el.getBoundingClientRect().height);

    expect(layoutHeight).toBeLessThanOrEqual(vcHeight + 2);
    expect(railHeight).toBeLessThanOrEqual(vcHeight + 2);
    expect(detailHeight).toBeLessThanOrEqual(vcHeight + 2);
  });
});
