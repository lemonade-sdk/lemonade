# Instructions

- Following Playwright test failed.
- Explain why, be concise, respect Playwright best practices.
- Provide a snippet of code with the fix, if possible.

# Test info

- Name: features.spec.ts >> Lemonade UI — Feature Parity >> 23 — loopback API requests resolve to IPv4 127.0.0.1 by default and respect capture setting for session headers
- Location: tests/features.spec.ts:1266:7

# Error details

```
Error: expect(received).toContain(expected) // indexOf

Expected substring: "//127.0.0.1:13305"
Received string:    "http://127.0.0.1:8123/api/v1/downloads"
```

# Page snapshot

```yaml
- generic [ref=e2]:
  - link "Skip to main content" [ref=e3] [cursor=pointer]:
    - /url: "#main-content"
  - generic [ref=e4]:
    - banner [ref=e5]:
      - generic [ref=e6]:
        - generic [ref=e9]: lemonade
        - status "Connected" [ref=e10]
      - navigation "Primary" [ref=e11]:
        - button "Chat" [ref=e12] [cursor=pointer]:
          - img [ref=e13]
          - generic [ref=e15]: Chat
        - button "Models" [ref=e16] [cursor=pointer]:
          - img [ref=e17]
          - generic [ref=e19]: Models
        - button "Backends" [ref=e20] [cursor=pointer]:
          - img [ref=e21]
          - generic [ref=e24]: Backends
        - button "Apps" [ref=e25] [cursor=pointer]:
          - img [ref=e26]
          - generic [ref=e30]: Apps
        - button "Monitor" [ref=e31] [cursor=pointer]:
          - img [ref=e32]
          - generic [ref=e35]: Monitor
        - button "Settings" [ref=e36] [cursor=pointer]:
          - img [ref=e37]
          - generic [ref=e40]: Settings
      - generic "App controls" [ref=e43]:
        - button "Search Lemonade" [ref=e45] [cursor=pointer]:
          - img [ref=e46]
        - button "Toggle theme" [ref=e49] [cursor=pointer]:
          - img [ref=e50]
        - button "Open download manager" [ref=e56] [cursor=pointer]:
          - img [ref=e57]
    - main [ref=e60]:
      - generic [ref=e62]:
        - complementary [ref=e63]:
          - generic [ref=e64]:
            - generic [ref=e65]:
              - img [ref=e66]
              - strong [ref=e69]: History
            - button "Collapse conversation sidebar" [expanded] [ref=e70] [cursor=pointer]:
              - img [ref=e71]
          - button "New chat" [ref=e75] [cursor=pointer]:
            - img [ref=e76]
            - generic [ref=e79]: New chat
          - listbox "Conversations" [ref=e80]
          - paragraph [ref=e81]: No conversations yet
        - generic [ref=e83]:
          - generic [ref=e84]:
            - heading "Get to know Lemonade" [level=1] [ref=e85]
            - paragraph [ref=e86]: 1 model ready. Ask a question or explore what Lemonade can do.
            - list [ref=e87]:
              - listitem [ref=e88] [cursor=pointer]:
                - img [ref=e90]
                - text: How do I use Lemonade?
              - listitem [ref=e92] [cursor=pointer]:
                - img [ref=e94]
                - text: How do I add a model?
              - listitem [ref=e97] [cursor=pointer]:
                - img [ref=e99]
                - text: What are Lemonade tools?
              - listitem [ref=e101] [cursor=pointer]:
                - img [ref=e103]
                - text: What can my hardware run?
          - generic [ref=e107]: Loaded right now
          - article [ref=e110]:
            - generic [ref=e111]:
              - generic [ref=e112]:
                - button "builtin.Qwen3-0.6B-GGUF" [ref=e114] [cursor=pointer]
                - generic [ref=e115]: llamacpp · unsloth/Qwen3-0.6B-GGUF:Q4_0
              - generic [ref=e116]: gpu
            - generic [ref=e118]:
              - img [ref=e120]
              - text: Chat
            - generic [ref=e123]: ● Active Chat mode
            - generic [ref=e124]:
              - button "View details" [ref=e125] [cursor=pointer]
              - button "Unload" [ref=e126] [cursor=pointer]
        - generic [ref=e128]:
          - generic [ref=e129]:
            - generic [ref=e130]:
              - generic [ref=e131]: Model
              - button "Chat builtin.Qwen3-0.6B-GGUF (1) ▾" [ref=e132] [cursor=pointer]:
                - generic [ref=e133]:
                  - img [ref=e135]
                  - generic [ref=e137]: Chat
                - generic [ref=e138]: builtin.Qwen3-0.6B-GGUF
                - generic [ref=e139]: (1)
                - generic [ref=e140]: ▾
            - button "Effective settings" [ref=e141] [cursor=pointer]:
              - img [ref=e142]
            - button "Logs" [ref=e143] [cursor=pointer]:
              - img [ref=e144]
              - text: Logs
          - generic [ref=e146]:
            - button "Add files, photos, or tools" [ref=e148] [cursor=pointer]:
              - img [ref=e149]
            - textbox "Message" [ref=e150]:
              - /placeholder: Message builtin.Qwen3-0.6B-GGUF…
            - button "Send" [disabled] [ref=e151]:
              - img [ref=e152]
          - generic [ref=e154]: Enter to send · Shift+Enter for newline · Paste or drop images
```

# Test source

```ts
  1231 |     expect(hasHuggingFace).toBeTruthy();
  1232 |
  1233 |     console.log(`Loaded Models: ${hasLoadedModels}, Downloaded: ${hasDownloaded}, Registry: ${hasRegistry}, HF: ${hasHuggingFace}`);
  1234 |
  1235 |     await page.screenshot({ path: 'screenshots/21-models-zone-labels.png', fullPage: true });
  1236 |   });
  1237 |
  1238 |   test('22 — Backends update button says "updated" not "installed"', async ({ page }) => {
  1239 |     await page.goto('/');
  1240 |     await page.waitForSelector('.titlebar__nav');
  1241 |
  1242 |     // Navigate to Backends
  1243 |     await page.locator('.titlebar__nav').getByText('Backends').click();
  1244 |     await page.waitForSelector('[data-view="backends"]');
  1245 |
  1246 |     // Check if any Update buttons exist in the matrix (filter by text)
  1247 |     const updateBtns = page.locator('.cell__swap', { hasText: /^Update$|^Updating/ });
  1248 |     const installBtns = page.locator('.cell__swap', { hasText: /^Install$|^Installing/ });
  1249 |     const updateCount = await updateBtns.count();
  1250 |     const installCount = await installBtns.count();
  1251 |     console.log(`Update buttons: ${updateCount}, Install buttons: ${installCount}`);
  1252 |
  1253 |     // If update buttons exist, they should say "Update" not "Install"
  1254 |     if (updateCount > 0) {
  1255 |       const firstUpdate = updateBtns.first();
  1256 |       await expect(firstUpdate).toContainText(/Update/);
  1257 |     }
  1258 |
  1259 |     // Verify the matrix table renders
  1260 |     const matrix = page.locator('[data-backends-matrix] table');
  1261 |     await expect(matrix).toBeVisible();
  1262 |
  1263 |     await page.screenshot({ path: 'screenshots/22-backends-update.png', fullPage: true });
  1264 |   });
  1265 |
  1266 |   test('23 — loopback API requests resolve to IPv4 127.0.0.1 by default and respect capture setting for session headers', async ({ page }) => {
  1267 |     // Mock WebSocket to immediately connect and send auth.ok
  1268 |     await page.addInitScript(() => {
  1269 |       class MockWebSocket {
  1270 |         static CONNECTING = 0;
  1271 |         static OPEN = 1;
  1272 |         static CLOSING = 2;
  1273 |         static CLOSED = 3;
  1274 |
  1275 |         readyState = MockWebSocket.CONNECTING;
  1276 |         onopen: (() => void) | null = null;
  1277 |         onmessage: ((ev: any) => void) | null = null;
  1278 |         onclose: (() => void) | null = null;
  1279 |         onerror: (() => void) | null = null;
  1280 |
  1281 |         constructor() {
  1282 |           setTimeout(() => {
  1283 |             this.readyState = MockWebSocket.OPEN;
  1284 |             if (this.onopen) this.onopen();
  1285 |           }, 10);
  1286 |         }
  1287 |
  1288 |         send(data: string) {
  1289 |           const parsed = JSON.parse(data);
  1290 |           if (parsed.type === 'auth') {
  1291 |             setTimeout(() => {
  1292 |               if (this.onmessage) {
  1293 |                 this.onmessage({ data: JSON.stringify({ type: 'auth.ok' }) });
  1294 |               }
  1295 |             }, 10);
  1296 |           }
  1297 |         }
  1298 |
  1299 |         close() {
  1300 |           this.readyState = MockWebSocket.CLOSED;
  1301 |           if (this.onclose) this.onclose();
  1302 |         }
  1303 |       }
  1304 |       (window as any).WebSocket = MockWebSocket as any;
  1305 |     });
  1306 |
  1307 |     // Mock chat completion
  1308 |     await page.route(/\/api\/v1\/chat\/completions/, async route => {
  1309 |       await route.fulfill({
  1310 |         status: 200,
  1311 |         contentType: 'application/json',
  1312 |         body: JSON.stringify({ choices: [{ message: { role: 'assistant', content: 'hi' } }] })
  1313 |       });
  1314 |     });
  1315 |
  1316 |     const requests: Array<{ url: string; headers: Record<string, string> }> = [];
  1317 |     page.on('request', req => {
  1318 |       if (req.url().includes('/api/v1/')) {
  1319 |         requests.push({ url: req.url(), headers: req.headers() });
  1320 |       }
  1321 |     });
  1322 |
  1323 |     await page.goto('/');
  1324 |     await page.waitForSelector('.titlebar__nav');
  1325 |     await page.waitForTimeout(1000);
  1326 |
  1327 |     // Initial requests (capturing is OFF by default)
  1328 |     expect(requests.length).toBeGreaterThan(0);
  1329 |     for (const req of requests) {
  1330 |       expect(req.url).not.toContain('//localhost:13305');
> 1331 |       expect(req.url).toContain('//127.0.0.1:13305');
       |                       ^ Error: expect(received).toContain(expected) // indexOf
  1332 |     }
  1333 |
  1334 |     // Trigger a chat completion while capturing is OFF
  1335 |     await page.evaluate(async () => {
  1336 |       try {
  1337 |         await (window as any).apiClient.chatCompletionOnce('mock-model', []);
  1338 |       } catch (e) {}
  1339 |     });
  1340 |
  1341 |     // Verify last request (chat completion) has NO session headers
  1342 |     const chatReqOff = requests.find(r => r.url.includes('/chat/completions'));
  1343 |     expect(chatReqOff).toBeDefined();
  1344 |     expect(chatReqOff!.headers['x-client-session-id']).toBeUndefined();
  1345 |
  1346 |     // Now toggle capturing ON
  1347 |     await page.evaluate(() => {
  1348 |       (window as any).inspectStore.setState({ capturing: true });
  1349 |     });
  1350 |
  1351 |     // Wait briefly for WebSocket connection to authenticate and enable headers
  1352 |     await page.waitForTimeout(200);
  1353 |
  1354 |     // Trigger a chat completion while capturing is ON
  1355 |     const requestsAfterToggle: Array<{ url: string; headers: Record<string, string> }> = [];
  1356 |     page.on('request', req => {
  1357 |       if (req.url().includes('/api/v1/')) {
  1358 |         requestsAfterToggle.push({ url: req.url(), headers: req.headers() });
  1359 |       }
  1360 |     });
  1361 |
  1362 |     await page.evaluate(async () => {
  1363 |       try {
  1364 |         await (window as any).apiClient.chatCompletionOnce('mock-model', []);
  1365 |       } catch (e) {}
  1366 |     });
  1367 |
  1368 |     const chatReqOn = requestsAfterToggle.find(r => r.url.includes('/chat/completions'));
  1369 |     expect(chatReqOn).toBeDefined();
  1370 |     expect(chatReqOn!.headers['x-client-session-id']).toBeDefined();
  1371 |   });
  1372 |
  1373 |   test('24 — fallback retry: retries without session headers on fetch preflight/network failure and disables them', async ({ page }) => {
  1374 |     // Mock WebSocket to immediately connect and send auth.ok
  1375 |     await page.addInitScript(() => {
  1376 |       class MockWebSocket {
  1377 |         static CONNECTING = 0;
  1378 |         static OPEN = 1;
  1379 |         static CLOSING = 2;
  1380 |         static CLOSED = 3;
  1381 |
  1382 |         readyState = MockWebSocket.CONNECTING;
  1383 |         onopen: (() => void) | null = null;
  1384 |         onmessage: ((ev: any) => void) | null = null;
  1385 |         onclose: (() => void) | null = null;
  1386 |         onerror: (() => void) | null = null;
  1387 |
  1388 |         constructor() {
  1389 |           setTimeout(() => {
  1390 |             this.readyState = MockWebSocket.OPEN;
  1391 |             if (this.onopen) this.onopen();
  1392 |           }, 10);
  1393 |         }
  1394 |
  1395 |         send(data: string) {
  1396 |           const parsed = JSON.parse(data);
  1397 |           if (parsed.type === 'auth') {
  1398 |             setTimeout(() => {
  1399 |               if (this.onmessage) {
  1400 |                 this.onmessage({ data: JSON.stringify({ type: 'auth.ok' }) });
  1401 |               }
  1402 |             }, 10);
  1403 |           }
  1404 |         }
  1405 |
  1406 |         close() {
  1407 |           this.readyState = MockWebSocket.CLOSED;
  1408 |           if (this.onclose) this.onclose();
  1409 |         }
  1410 |       }
  1411 |       (window as any).WebSocket = MockWebSocket as any;
  1412 |     });
  1413 |
  1414 |     // Mock chat completion: reject requests with session headers, succeed without them
  1415 |     await page.route(/\/api\/v1\/chat\/completions/, async route => {
  1416 |       const headers = route.request().headers();
  1417 |       if (headers['x-client-session-id']) {
  1418 |         await route.abort('failed');
  1419 |       } else {
  1420 |         await route.fulfill({
  1421 |           status: 200,
  1422 |           contentType: 'application/json',
  1423 |           body: JSON.stringify({ choices: [{ message: { role: 'assistant', content: 'fallback-success' } }] })
  1424 |         });
  1425 |       }
  1426 |     });
  1427 |
  1428 |     await page.goto('/');
  1429 |     await page.waitForSelector('.titlebar__nav');
  1430 |
  1431 |     // Turn capturing ON
```
