import { test, expect } from '@playwright/test';

const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS, PUT, PATCH, DELETE',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Client-Session-Id',
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
          {
            id: 'mock-model-1',
            name: 'mock-model-1',
            model_name: 'mock-model-1'
          },
          {
            id: 'mock-reasoning-model',
            name: 'mock-reasoning-model',
            model_name: 'mock-reasoning-model',
            labels: ['reasoning'],
            downloaded: true
          },
          {
            id: 'mock-tool-hot',
            name: 'mock-tool-hot',
            model_name: 'mock-tool-hot',
            labels: ['tool-calling', 'hot'],
            downloaded: true
          },
          {
            id: 'mock-tool-downloaded',
            name: 'mock-tool-downloaded',
            model_name: 'mock-tool-downloaded',
            labels: ['tool-calling'],
            downloaded: true
          },
          {
            id: 'mock-tool-remote',
            name: 'mock-tool-remote',
            model_name: 'mock-tool-remote',
            labels: ['tool-calling'],
            downloaded: false
          },
          {
            id: 'ACE-Step-music',
            name: 'ACE-Step-music',
            model_name: 'ACE-Step-music',
            labels: ['audio', 'music-generation']
          }
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
  test('Improve optimizer model selector lists downloaded tool-calling models', async ({ page }) => {
    await page.addInitScript(() => {
      localStorage.setItem('lemonade:chat_last_ready_model', 'mock-tool-downloaded');
    });
    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    await page.evaluate(() => {
      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-improve-trace',
          traceId: 'trace-improve',
          spanId: 'span-improve',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-reasoning-model',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          messages: [{ role: 'user', content: 'Summarize this report.' }],
          output: 'A response that needs improvement.',
          improveData: {
            critique: [],
            parameter_diff: {
              temperature: { suggested: 0.3, rationale: 'Improve consistency.' },
              system_vs_user_split: false
            },
            optimized_prompt: {
              system_instructions: null,
              user_prompt: 'Summarize the report concisely.'
            },
            key_improvements: ['Reduced verbosity.']
          }
        }],
        selectedTraceId: 'mock-improve-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    const optimizerSelector = page.getByRole('combobox', { name: 'Select LLM Optimizer' });
    await expect(optimizerSelector).toHaveValue('mock-tool-downloaded');
    await optimizerSelector.click();

    // Opening the optimizer picker must immediately expose every downloaded
    // tool-calling model, without requiring the user to clear the current value.
    await expect(page.getByRole('option', { name: 'mock-tool-hot', exact: true })).toBeVisible();
    await expect(page.getByRole('option', { name: 'mock-tool-downloaded', exact: true })).toBeVisible();
    await expect(page.getByRole('option', { name: 'mock-tool-remote', exact: true })).toHaveCount(0);
    await expect(page.getByRole('option', { name: 'mock-reasoning-model', exact: true })).toHaveCount(0);
    await expect(page.getByRole('option', { name: 'ACE-Step-music', exact: true })).toHaveCount(0);

    await page.getByRole('option', { name: 'mock-tool-hot', exact: true }).click();
    await expect(optimizerSelector).toHaveValue('mock-tool-hot');

    await page.getByRole('button', { name: 'Test', exact: true }).click();
    const testModelSelector = page.getByRole('combobox', { name: 'Select Test Model' });
    await testModelSelector.click();
    await testModelSelector.press('Control+A');
    await testModelSelector.press('Backspace');
    await expect(page.getByRole('option', { name: 'ACE-Step-music', exact: true })).toBeVisible();
  });

  test('Closing optimization progress aborts the request and releases the GUI', async ({ page }) => {
    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    await page.evaluate(() => {
      const originalFetch = window.fetch.bind(window);
      (window as any).__optimizerAborted = false;
      window.fetch = ((input: RequestInfo | URL, init?: RequestInit) => {
        const url = typeof input === 'string' ? input : input instanceof Request ? input.url : String(input);
        if (url.includes('/api/v1/chat/completions')) {
          return new Promise<Response>((_resolve, reject) => {
            const abort = () => {
              (window as any).__optimizerAborted = true;
              reject(new DOMException('Aborted', 'AbortError'));
            };
            if (init?.signal?.aborted) {
              abort();
              return;
            }
            init?.signal?.addEventListener('abort', abort, { once: true });
          });
        }
        return originalFetch(input, init);
      }) as typeof window.fetch;

      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-cancel-trace',
          traceId: 'trace-cancel',
          spanId: 'span-cancel',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-tool-hot',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          messages: [{ role: 'user', content: 'Summarize this report.' }],
          output: 'A response that needs improvement.'
        }],
        selectedTraceId: 'mock-cancel-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    await page.getByRole('button', { name: 'Analyze and Optimize Prompt' }).click();

    const progressDialog = page.getByRole('dialog', { name: 'Analyzing & Optimizing Prompt' });
    await expect(progressDialog).toBeVisible();
    await progressDialog.getByRole('button', { name: 'Close modal' }).click();

    await expect(progressDialog).toHaveCount(0);
    await expect.poll(() => page.evaluate(() => (window as any).__optimizerAborted)).toBe(true);
    await expect(page.getByRole('dialog', { name: 'Prompt Optimization Failed' })).toHaveCount(0);
  });

  test('Improve optimizer rejects an echoed worked-example response', async ({ page }) => {
    // Stream a response that reproduces the meta-prompt worked example verbatim instead
    // of generating real content. The optimizer must reject it as placeholder output.
    const exampleEcho = JSON.stringify({
      critique: [
        {
          category: 'constraints',
          severity: 'high',
          finding: 'The user prompt never specified an output format, so the model returned prose instead of a list.',
          rationale: 'The task implies enumeration but no format was requested; an explicit constraint closes that gap.'
        }
      ],
      parameter_diff: { temperature: { suggested: 0.2, rationale: 'A deterministic formatting task benefits from low temperature.' }, system_vs_user_split: false },
      optimized_prompt: {
        system_instructions: 'When asked to list steps, answer as a numbered list.',
        user_prompt: 'List the steps to bake a loaf of sourdough bread.\n\nAnswer as a numbered list, one step per line.'
      },
      key_improvements: ['Added an explicit output format constraint.', 'Lowered temperature for determinism.']
    });
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      if (route.request().method() === 'OPTIONS') {
        await route.fulfill({ status: 204, headers: corsHeaders });
        return;
      }
      await route.fulfill({
        status: 200,
        headers: { ...corsHeaders, 'Content-Type': 'text/event-stream' },
        body: [
          `data: ${JSON.stringify({ choices: [{ delta: { content: exampleEcho } }], id: 'x' })}`,
          'data: [DONE]'
        ].join('\n\n')
      });
    });

    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();
    await page.evaluate(() => {
      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-echo-trace',
          traceId: 'trace-echo',
          spanId: 'span-echo',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-reasoning-model',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          messages: [{ role: 'user', content: 'Summarize this report.' }],
          output: 'A response that needs improvement.'
        }],
        selectedTraceId: 'mock-echo-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    await page.getByRole('button', { name: 'Analyze and Optimize Prompt' }).click();

    // The example-echo must be rejected: show the failure modal, not accept the example as output.
    await expect(page.getByRole('heading', { name: 'Prompt Optimization Failed' })).toBeVisible();
  });

  test('Improve optimizer rejects a partial worked-example echo in user_prompt', async ({ page }) => {
    // The model echoes only the example's optimized user_prompt while the rest of the
    // response is genuine. Partial echoes must still be detected, not accepted.
    const partialEcho = JSON.stringify({
      critique: [
        {
          category: 'clarity',
          severity: 'high',
          finding: 'The request was ambiguous and the response drifted from the intent.',
          rationale: 'Missing a clear directive caused the divergence.'
        }
      ],
      parameter_diff: { temperature: { suggested: 0.4, rationale: 'Reduce variance in the rewrite.' }, system_vs_user_split: false },
      optimized_prompt: {
        system_instructions: null,
        user_prompt: 'List the steps to bake a loaf of sourdough bread.\n\nAnswer as a numbered list, one step per line.'
      },
      key_improvements: ['Clarified the directive.']
    });
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      if (route.request().method() === 'OPTIONS') {
        await route.fulfill({ status: 204, headers: corsHeaders });
        return;
      }
      await route.fulfill({
        status: 200,
        headers: { ...corsHeaders, 'Content-Type': 'text/event-stream' },
        body: [
          `data: ${JSON.stringify({ choices: [{ delta: { content: partialEcho } }], id: 'x' })}`,
          'data: [DONE]'
        ].join('\n\n')
      });
    });

    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();
    await page.evaluate(() => {
      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-echo-trace',
          traceId: 'trace-echo',
          spanId: 'span-echo',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-reasoning-model',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          messages: [{ role: 'user', content: 'Summarize this report.' }],
          output: 'A response that needs improvement.'
        }],
        selectedTraceId: 'mock-echo-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    await page.getByRole('button', { name: 'Analyze and Optimize Prompt' }).click();

    await expect(page.getByRole('heading', { name: 'Prompt Optimization Failed' })).toBeVisible();
  });

  test('Improve optimizer accepts a legitimate prompt that mentions sourdough', async ({ page }) => {
    // The optimized prompt legitimately mentions "sourdough" but is NOT a verbatim echo
    // of the worked example. It must be accepted as real output, not rejected as a
    // placeholder.
    const legit = JSON.stringify({
      critique: [
        {
          category: 'clarity',
          severity: 'medium',
          finding: 'The response rambled without a clear structure.',
          rationale: 'An explicit structure would constrain the generation.'
        }
      ],
      parameter_diff: { temperature: { suggested: 0.3, rationale: 'Improve determinism.' }, system_vs_user_split: false },
      optimized_prompt: {
        system_instructions: 'Answer concisely and in a structured format when the task allows.',
        user_prompt: 'Please bake a sourdough bread for me and give me the full recipe.'
      },
      key_improvements: ['Added a structure directive.']
    });
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      if (route.request().method() === 'OPTIONS') {
        await route.fulfill({ status: 204, headers: corsHeaders });
        return;
      }
      await route.fulfill({
        status: 200,
        headers: { ...corsHeaders, 'Content-Type': 'text/event-stream' },
        body: [
          `data: ${JSON.stringify({ choices: [{ delta: { content: legit } }], id: 'x' })}`,
          'data: [DONE]'
        ].join('\n\n')
      });
    });

    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();
    await page.evaluate(() => {
      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-echo-trace',
          traceId: 'trace-echo',
          spanId: 'span-echo',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-reasoning-model',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          messages: [{ role: 'user', content: 'Summarize this report.' }],
          output: 'A response that needs improvement.'
        }],
        selectedTraceId: 'mock-echo-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    await page.getByRole('button', { name: 'Analyze and Optimize Prompt' }).click();

    // Accepted: the optimization delta modal opens, not the failure modal.
    await expect(page.getByRole('heading', { name: 'Prompt Optimization Delta' })).toBeVisible();
  });

  test('Improve optimizer accepts a workable response that preserves example wording from the original prompt', async ({ page }) => {
    // The ORIGINAL prompt itself contains worked-example wording ("bake a loaf of
    // sourdough bread"). Since the guardrail requires preserving task content verbatim,
    // re-using that wording in the optimized prompt is preservation, not an echo — it must
    // be accepted even though it matches the worked-example literal.
    const preserved = JSON.stringify({
      critique: [
        {
          category: 'clarity',
          severity: 'medium',
          finding: 'The request lacked explicit steps.',
          rationale: 'Adding a numbered structure would guide the outcome.'
        }
      ],
      parameter_diff: { temperature: { suggested: 0.3, rationale: 'Improve determinism.' }, system_vs_user_split: false },
      optimized_prompt: {
        system_instructions: 'Answer as a numbered list, one step per line.',
        user_prompt: 'Bake a loaf of sourdough bread.\n\nList all the steps in order.'
      },
      key_improvements: ['Added a numbered structure directive.']
    });
    await page.route(/\/api\/v1\/chat\/completions/, async route => {
      if (route.request().method() === 'OPTIONS') {
        await route.fulfill({ status: 204, headers: corsHeaders });
        return;
      }
      await route.fulfill({
        status: 200,
        headers: { ...corsHeaders, 'Content-Type': 'text/event-stream' },
        body: [
          `data: ${JSON.stringify({ choices: [{ delta: { content: preserved } }], id: 'x' })}`,
          'data: [DONE]'
        ].join('\n\n')
      });
    });

    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();
    await page.evaluate(() => {
      (window as any).inspectStore.setState({
        traces: [{
          id: 'mock-preserve-trace',
          traceId: 'trace-preserve',
          spanId: 'span-preserve',
          kind: 'LLM',
          operation: 'chat.completions',
          status: 'ok',
          model: 'mock-reasoning-model',
          timestamp: '12:00:00 PM',
          startTimeMs: Date.now(),
          dur: 1500,
          // The original prompt contains the worked-example wording ("bake a loaf of
          // sourdough bread", "answer as a numbered list, one step per line") verbatim;
          // preserving it must not be flagged as an echo.
          messages: [{ role: 'user', content: 'Bake a loaf of sourdough bread.\n\nAnswer as a numbered list, one step per line.' }],
          output: 'A response that needs improvement.'
        }],
        selectedTraceId: 'mock-preserve-trace'
      });
    });

    await page.getByRole('tab', { name: 'Improve' }).click();
    await page.getByRole('button', { name: 'Analyze and Optimize Prompt' }).click();

    // Accepted: preserving original wording is not an echo, so the delta modal opens.
    await expect(page.getByRole('heading', { name: 'Prompt Optimization Delta' })).toBeVisible();
  });

  test('Decoupled Keyboard Selection in Trace List', async ({ page }) => {
    // 1. Navigate to the inspect view directly
    await page.goto('/#/dashboard/telemetry');
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
    const traceRows = page.locator('.inspect-rail__list .workspace-list-row');
    await expect(traceRows).toHaveCount(3);

    // Get the first item (which should have tabIndex=0 and be mock-trace-3)
    const firstOption = page.locator('[role="option"][data-trace-id="mock-trace-3"]');
    await expect(firstOption).toBeVisible();
    await firstOption.focus();
    await expect(firstOption).toBeFocused();

    // Verify initial selected state
    let selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-3');

    // ArrowDown should move active focus to mock-trace-2, but NOT select it
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(100);

    const secondOption = page.locator('[role="option"][data-trace-id="mock-trace-2"]');
    await expect(secondOption).toBeFocused();

    // Verify selectedTraceId is STILL mock-trace-3 (decoupled)
    selectedId = await page.evaluate(() => (window as any).inspectStore.getState().selectedTraceId);
    expect(selectedId).toBe('mock-trace-3');
    await expect(firstOption).toHaveAttribute('aria-selected', 'true');
    await expect(secondOption).toHaveAttribute('aria-selected', 'false');

    // ArrowDown again to mock-trace-1
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(100);

    const thirdOption = page.locator('[role="option"][data-trace-id="mock-trace-1"]');
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
    await page.goto('/#/dashboard/telemetry');
    await expect(page.locator('.inspect-rail')).toBeVisible();

    // Open Sim Create Modal
    await page.getByRole('button', { name: 'Create', exact: true }).click();
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
    await page.goto('/#/dashboard/telemetry');
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
    const traceRows = page.locator('.inspect-rail__list .workspace-list-row');
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
    await page.goto('/#/dashboard/telemetry');
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
    await page.locator('.inspect-rail__list .workspace-list-row').last().click(); // newest is first, so trace 1 is last
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
    await page.locator('.inspect-rail__list .workspace-list-row').first().click(); // Trace B is the first row
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
