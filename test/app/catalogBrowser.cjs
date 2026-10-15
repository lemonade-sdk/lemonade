const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const stage = process.argv[3];
const origin = `http://127.0.0.1:${manifest.port}`;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let socket, sequence = 0, assertions = 0;
const pending = new Map(), mutations = [], errors = [], responses = [];
function check(value, message) { assert(value, message); assertions++; }
function send(method, params = {}) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error('CDP timeout: ' + method)); }, 8000);
    pending.set(id, { resolve, reject, timer });
    socket.send(JSON.stringify({ id, method, params }));
  });
}
async function evaluate(expression) {
  const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
  if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
  return result.result.value;
}
async function wait(expression, timeout = 10000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    const result = await evaluate(`Boolean(${expression})`);
    if (result) return result;
    await delay(100);
  }
  throw new Error('Timed out: ' + expression);
}
const query = selector => `document.querySelector(${JSON.stringify(selector)})`;
const button = text => `[...document.querySelectorAll('.settings-modal button')].find(e=>e.textContent.trim()===${JSON.stringify(text)})`;
const field = label => `document.getElementById([...document.querySelectorAll('.settings-modal label')].find(e=>e.textContent===${JSON.stringify(label)})?.htmlFor)`;
async function point(expression) {
  return evaluate(`(()=>{const e=${expression}; if(!e) throw Error('Missing element: '+${JSON.stringify(expression)}); e.scrollIntoView({block:'center'}); const r=e.getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2};})()`);
}
async function click(expression) {
  const p = await point(expression);
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...p });
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p, button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p, button: 'left', clickCount: 1 });
}
async function key(key, code, modifiers = 0) {
  const windowsVirtualKeyCode = { KeyA: 65, Escape: 27, Enter: 13, Tab: 9, Backspace: 8, End: 35, ArrowDown: 40 }[code];
  await send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, modifiers, windowsVirtualKeyCode,
    ...(code === 'KeyA' && modifiers === 2 ? { commands: ['selectAll'] } : {}) });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, modifiers, windowsVirtualKeyCode });
}
async function input(expression, value) {
  await click(expression);
  await key('a', 'KeyA', 2);
  await key('Backspace', 'Backspace');
  await send('Input.insertText', { text: value });
}
async function go(base = origin) {
  await send('Page.bringToFront');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `localStorage.setItem('lemonade-settings',JSON.stringify({apiKey:{value:'regular-test-key'},modelManager:{organizationMode:'recipe'}}))` });
  const navigation = await send('Page.navigate', { url: base + '/app' });
  if (navigation.errorText) throw new Error(navigation.errorText);
  await wait(`location.origin === ${JSON.stringify(base)} && document.readyState === 'complete'`, 30000);
  await wait(query('.model-search-input'), 30000);
  await wait(`window.api?.isWebApp === true`);
  check(await evaluate('typeof window.api.onConnectionSettingsUpdated === "function"'), 'real bridge callback available');
}
async function open(model) {
  await input(query('.model-search-input'), model.replace('user.', ''));
  const row = `[...document.querySelectorAll('.model-catalog-item')].find(e=>e.textContent.includes(${JSON.stringify(model.replace('user.', ''))}))`;
  await wait(row);
  const p = await point(row);
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...p });
  await wait(query('button[title="Load model with options"]'));
  await click(query('button[title="Load model with options"]'));
  await wait(`document.querySelector('.settings-modal')?.textContent.includes(${JSON.stringify(model.replace('user.', ''))}) && !document.querySelector('.settings-loading')`);
  check(await evaluate(`document.querySelectorAll('.settings-overlay').length === 1`), 'one live options modal');
}
async function snapshot(model) {
  const response = await fetch(origin + '/v1/models/' + encodeURIComponent(model) + '/options', { headers: { Authorization: 'Bearer regular-test-key' } });
  check(response.ok, 'model options read'); return response.json();
}
async function screenshot(name) {
  const result = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(manifest.output, name + '.png'), Buffer.from(result.data, 'base64'));
}
async function exercise(model, label, option) {
  await go(); await open(model); await wait(field(label));
  const provenance = `${field(label)}.closest('.form-section').textContent`;
  check(await evaluate(`${field(label)}.value === '2'`), 'schema/effective default 2');
  check(await evaluate(`document.querySelector('.settings-modal').textContent.includes('server default: 2')`), 'default provenance visible');
  check(await evaluate(`${provenance}.includes('Schema default: 2') && ${provenance}.includes('Effective: 2; saved override: none')`), 'schema/effective/unsaved provenance visible');
  await input(field(label), '9');
  await wait(`${button('Save')}.disabled`);
  check(await evaluate(`document.querySelector('.schema-error').textContent.includes('range')`), 'range validation visible');
  await input(field(label), '4');
  await wait(`!${button('Save')}.disabled`);
  check(await evaluate(`${field(label)}.value === '4'`), 'controlled numeric editing replaces its draft');
  await key('Tab', 'Tab');
  check(await evaluate(`document.activeElement.textContent === 'Use inherited value'`), 'keyboard focus reaches inheritance action');
  const before = mutations.length;
  await click(button('Save'));
  await wait(`document.querySelector('.settings-modal').textContent.includes('Options saved.')`);
  check(await evaluate(`${provenance}.includes('Effective: 4; saved override: 4')`), 'saved/effective integer provenance visible');
  const saved = await snapshot(model);
  check(saved.saved[option] === 4 && saved.effective[option] === 4, 'Save persisted exact integer 4');
  check(!mutations.slice(before).some(item => item.url.endsWith('/load')), 'Save did not load');
  check(mutations.slice(before).some(item => JSON.stringify(item.body) === JSON.stringify({ [option]: 4 })), 'Save omitted untouched inherited fields and pinned');
  await click(`[...document.querySelectorAll('.schema-recipe-options .form-section')].find(e=>e.textContent.includes(${JSON.stringify(label)})).querySelector('button')`);
  await click(button('Save')); await wait(`document.querySelector('.settings-modal').textContent.includes('Options saved.')`);
  await wait(`${provenance}.includes('Effective: 2; saved override: none')`);
  const inherited = await snapshot(model);
  check(!(option in inherited.saved) && inherited.effective[option] === 2, 'inherit Save clears saved override');
  if (model === 'user.UiProbe') {
    await input(field('Probe text'), 'temporary'); await input(field('Probe text'), '');
    await input(field('Probe notes'), 'two\nlines');
    await click(field('Probe flag')); await click(field('Probe flag'));
    await click(field('Probe choice')); await key('End', 'End'); await key('Enter', 'Enter');
    await click(button('Save'));
    await wait(`document.querySelector('.settings-modal').textContent.includes('Options saved.')`);
    const typed = await snapshot(model);
    check(typed.saved.ui_probe_text === '' && typed.saved.ui_probe_notes === 'two\nlines' &&
      typed.saved.ui_probe_flag === false && typed.saved.ui_probe_choice === true, 'five controls preserve exact JSON types, false and empty string');
  }
  await input(field(label), '4');
  await screenshot(model.replace('user.', ''));
  await click(button('Load'));
  await wait(`!document.querySelector('.settings-modal')`, 15000);
  check(mutations.some(item => item.url.endsWith('/load') && item.body.save_options === false), 'Load is transient');
  const transient = await snapshot(model);
  check(!(option in transient.saved) && transient.effective[option] === 2, 'unsaved Load override did not persist');
  await wait(`document.querySelector('.model-selector-trigger')?.textContent.includes(${JSON.stringify(model.replace('user.', ''))})`);
  await wait(query('textarea.chat-input'));
  const prompt = 'UI frozen ' + model;
  await input(query('textarea.chat-input'), prompt);
  await click(query('.chat-send-button'));
  await wait(`document.body.textContent.includes('fake batch=4 tag=initial')`, 15000);
  check(mutations.some(item => item.url.endsWith('/chat/completions')), 'existing chat panel sent request');
  await screenshot(model.replace('user.', '') + '-chat');
}
(async () => {
  const targets = await (await fetch(`http://127.0.0.1:${manifest.debugPort}/json/list`)).json();
  const target = targets.find(item => item.type === 'page');
  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.addEventListener('open', resolve, { once: true }); socket.addEventListener('error', reject, { once: true }); });
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const item = pending.get(message.id); if (!item) return;
      pending.delete(message.id); clearTimeout(item.timer);
      if (message.error) item.reject(new Error(JSON.stringify(message.error))); else item.resolve(message.result);
    } else if (message.method === 'Network.requestWillBeSent') {
      const request = message.params.request;
      if (request.method === 'POST') mutations.push({ url: request.url, body: request.postData ? JSON.parse(request.postData) : null });
    } else if (message.method === 'Runtime.exceptionThrown') errors.push(message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text);
    else if (message.method === 'Network.responseReceived') responses.push([message.params.response.url, message.params.response.status]);
    else if (message.method === 'Network.loadingFailed') responses.push(['failed', message.params.errorText]);
    else if (message.method === 'Fetch.requestPaused') {
      const wire = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures/backend-catalog/catalog-fourth.json'), 'utf8'));
      wire.control_version = 2;
      void send('Fetch.fulfillRequest', { requestId: message.params.requestId, responseCode: 200, responseHeaders: [{ name: 'Content-Type', value: 'application/json' }], body: Buffer.from(JSON.stringify(wire)).toString('base64') });
    }
  });
  await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
  await send('Network.setBlockedURLs', { urls: ['https://*'] });
  await send('Emulation.setDeviceMetricsOverride', { width: 1680, height: 1000, deviceScaleFactor: 1, mobile: false });
  if (stage === 'baseline') {
    await go(); check(!(await evaluate('document.body.textContent')).includes('CatalogFourth'), 'fourth absent before server-only placement');
  } else if (stage === 'fourth') await exercise('user.CatalogFourth', 'Batch size', 'catalog_fourth_batch');
  else if (stage === 'probe') await exercise('user.UiProbe', 'Probe batch', 'ui_probe_batch');
  else if (stage === 'legacy') {
    const legacy = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures/backend-catalog/legacy.json'), 'utf8'));
    const system = await (await fetch(origin + '/api/v1/system-info', { headers: { Authorization: 'Bearer regular-test-key' } })).json();
    await go();
    for (const [recipe, keys] of Object.entries(legacy.recipes)) {
      await open(recipe === 'flm' ? 'Legacy-FLM' : 'user.Legacy-' + recipe);
      await wait(`document.querySelector('.model-options-content')`);
      const text = await evaluate(`document.querySelector('.model-options-content').textContent`);
      const backends = Object.entries(system.recipes[recipe].backends).filter(([, info]) => info.state && info.state !== 'unsupported').map(([name]) => name);
      const selector = query('.settings-modal select');
      const hasSelector = keys.some(name => legacy.definitions[name].isBackendOption) && backends.length > 1;
      check(await evaluate(`Boolean(${selector})`) === hasSelector, recipe + ': backend selector visibility');
      let position = -1;
      for (const name of keys) {
        const definition = legacy.definitions[name];
        if (definition.isBackendOption && !hasSelector) continue;
        const label = definition.type === 'boolean' ? definition.label : definition.label.toLowerCase();
        const next = text.indexOf(label, position + 1);
        check(next > position, recipe + ': visible label/order ' + label); position = next;
      }
      if (hasSelector) {
        check(JSON.stringify(await evaluate(`[...${selector}.options].map(e=>e.value)`)) === JSON.stringify(['', ...backends]), recipe + ': Auto and server backend order');
        await click(selector); await key('End', 'End'); await key('Enter', 'Enter');
        check(await evaluate(`${selector}.value`) === backends.at(-1), recipe + ': controlled backend selection');
        await click(button('Reset All'));
        check(await evaluate(`${selector}.value === ''`), recipe + ': reset backend to Auto');
      }
      if (recipe === 'llamacpp') {
        const context = query('.context-size-input');
        check(await evaluate(`${context}.value === '4096'`), 'legacy context fallback 4096');
        await input(context, '6000'); await key('Tab', 'Tab');
        check(await evaluate(`${context}.value === '6000'`), 'legacy context numeric editing');
        await click(query('.context-size-slider')); await key('End', 'End');
        check(await evaluate(`${context}.value === '8192'`), 'legacy context slider reaches model maximum');
        const args = `[...document.querySelectorAll('.settings-modal .form-section')].find(e=>e.querySelector('label')?.textContent==='llamacpp arguments').querySelector('input')`;
        await input(args, '--ui-fixture');
        check(await evaluate(`${args}.value === '--ui-fixture'`), 'legacy string editing');
        const merge = `[...document.querySelectorAll('.settings-modal .settings-section')].find(e=>e.textContent.includes('Merge arguments')).querySelector('input')`;
        check(await evaluate(`${merge}.checked`), 'legacy boolean default');
        await click(merge); check(!await evaluate(`${merge}.checked`), 'legacy boolean editing');
        await click(button('Reset All'));
        check(await evaluate(`${context}.value === '4096' && ${args}.value === '' && ${merge}.checked`), 'legacy reset restores context/string/boolean');
        await screenshot('legacy-controls');
      }
      if (recipe === 'sd-cpp') {
        const steps = `[...document.querySelectorAll('.settings-modal .form-section')].find(e=>e.querySelector('label')?.textContent==='steps').querySelector('input')`;
        await input(steps, '200'); await key('Tab', 'Tab');
        check(await evaluate(`${steps}.value === '150'`), 'legacy numeric upper bound clamps');
        await click(button('Reset All'));
        check(await evaluate(`${steps}.value === '20'`), 'legacy numeric reset');
      }
      await key('Escape', 'Escape'); await wait(`!document.querySelector('.settings-modal')`);
    }
    check(mutations.every(item => !item.url.endsWith('/load')), 'legacy inspection performed no inference');
  } else if (stage === 'dirty') {
    await go(); await open('user.CatalogFourth'); await wait(field('Batch size'));
    await input(field('Batch size'), '5'); check(await evaluate(`${field('Batch size')}.value === '5'`), 'dirty edit retained before restart');
  } else if (stage === 'restart') {
    const before = mutations.length;
    await click(button('Save'));
    await wait(`document.querySelector('.settings-modal').textContent.includes('Server configuration changed; review your edits.')`);
    check(mutations.length === before, 'submission discovering new catalog stops before mutation');
    check(await evaluate(`${field('Batch size')}.value === '5'`), 'dirty edit retained after same-URL restart');
    check(await evaluate(`${button('Save')}.disabled && ${button('Load')}.disabled`), 'new bounds block invalid retained edit');
    await input(field('Batch size'), '4'); await click(button('Save'));
    await wait(`document.querySelector('.settings-modal').textContent.includes('Options saved.')`);
    check((await snapshot('user.CatalogFourth')).saved.catalog_fourth_batch === 4, 'reviewed edit explicitly saved');
    await screenshot('restart-review');
  } else if (stage === 'incompatible') {
    await send('Fetch.enable', { patterns: [{ urlPattern: '*backends/schema*' }] });
    await evaluate(`window.dispatchEvent(new Event('focus'))`);
    await wait(`document.querySelector('.settings-modal').textContent.includes('Incompatible backend schema/control version')`);
    check(await evaluate(`${button('Save')}.disabled && ${button('Load')}.disabled`), 'incompatible schema disables submission');
    await screenshot('incompatible'); await send('Fetch.disable');
  } else if (stage === 'auth') {
    await evaluate(`window.api.getSettings().then(s=>window.api.saveSettings({...s,apiKey:{value:'wrong-test-key'}}))`);
    await wait(`document.querySelector('.settings-modal').textContent.includes('Authentication failed')`);
    check(await evaluate(`${button('Save')}.disabled`), 'auth change invalidates usable form');
    await evaluate(`window.api.getSettings().then(s=>window.api.saveSettings({...s,apiKey:{value:'regular-test-key'}}))`);
    await wait(field('Batch size')); await wait(`!${button('Save')}.disabled`);
    check(await evaluate(`window.api.isWebApp && location.origin === ${JSON.stringify(origin)}`), 'browser identity remains current origin');
  } else if (stage === 'switch') {
    await go(`http://127.0.0.1:${manifest.otherPort}`);
    await wait(query('button[aria-label="Models"]'));
    const other = await evaluate(`fetch('/api/v1/backends/schema',{headers:{Authorization:'Bearer regular-test-key'}}).then(r=>{if(!r.ok)throw Error('Second server schema failed');return r.json()})`);
    check(other.recipes.length === 17 && !other.recipes.some(recipe => recipe.recipe === 'catalog-fourth'), 'second server has its own authenticated builtin-only catalog');
    check(!(await evaluate('document.body.textContent')).includes('CatalogFourth'), 'second server does not reuse first-server model metadata');
    await go(); await open('user.CatalogFourth'); await wait(field('Batch size'));
    check(await evaluate(`${field('Batch size')}.max === '4'`), 'returning server resolves its current catalog metadata');
  } else throw new Error('Unknown stage ' + stage);
  check(errors.length === 0, 'no uncaught browser exceptions: ' + errors.join(', '));
  console.log(JSON.stringify({ stage, assertions, mutations, errors }));
})().catch(async error => {
  let body; try { body = await evaluate('document.body.innerText'); await screenshot(stage + '-failure'); } catch {}
  const document = await evaluate('({url:location.href,ready:document.readyState,html:document.documentElement.outerHTML.slice(0,4000),scripts:[...document.scripts].map(s=>s.src)})').catch(() => null);
  console.error(error.stack, JSON.stringify({errors, responses:responses.slice(-30), document}), body); process.exitCode = 1;
}).finally(() => { for (const item of pending.values()) clearTimeout(item.timer); socket?.close(); });
