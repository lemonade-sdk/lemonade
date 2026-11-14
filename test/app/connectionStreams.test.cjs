const { assert, test, run, bridge, tick } = require('./helpers/catalogHarness.cjs');
const { api, connection: connect } = bridge();
const calls = [];
let reply = () => new Response('{}');
global.fetch = async (url, options = {}) => { calls.push({ url, options }); return reply(url, options); };
global.performance = global.performance || { now: () => Date.now() };
const encoder = new TextEncoder();
const event = (name, data) => encoder.encode(`event: ${name}\ndata: ${JSON.stringify(data)}\n\n`);
const sse = start => new Response(new ReadableStream({ start }), { headers: { 'content-type': 'text/event-stream' } });
const json = value => new Response(JSON.stringify(value), { headers: { 'content-type': 'application/json' } });
let server = 0;
const fresh = () => { server += 1; connect(`http://stream-${server}.test`, `private-key-${server}`); calls.length = 0; };
const notifications = () => { let count = 0; const stop = api.onModelsChanged(() => { count += 1; }); return { stop, count: () => count }; };

const sockets = [];
class FakeSocket extends EventTarget {
  constructor(url) { super(); this.url = url; this.readyState = FakeSocket.CONNECTING; this.sent = []; sockets.push(this); }
  send(text) { this.sent.push(JSON.parse(text)); }
  close(code = 1005) {
    if (this.readyState === FakeSocket.CLOSED) return;
    this.readyState = FakeSocket.CLOSED;
    const closed = new Event('close'); closed.code = code; this.dispatchEvent(closed);
  }
  open() { this.readyState = FakeSocket.OPEN; this.dispatchEvent(new Event('open')); }
  message(data) { const message = new Event('message'); message.data = JSON.stringify(data); this.dispatchEvent(message); }
}
FakeSocket.CONNECTING = 0; FakeSocket.OPEN = 1; FakeSocket.CLOSED = 3;
global.WebSocket = FakeSocket;
global.window.setTimeout = setTimeout;
global.window.clearTimeout = clearTimeout;

test('an install stream from the previous server stops applying progress and completes nothing', async () => {
  fresh(); let feed;
  reply = () => sse(controller => { feed = controller; controller.enqueue(event('progress', { percent: 10 })); });
  const seen = []; let error; let completed = false; const models = notifications();
  await api.installBackend('constructor', 'cpu', {
    onProgress: data => {
      seen.push(data.percent);
      if (seen.length > 2) return;
      if (seen.length === 2) {
        connect('http://stream-other.test', 'private-other-key');
        feed.enqueue(event('complete', { percent: 100 }));
        feed.close();
      }
    },
    onComplete: () => { completed = true; }, onError: caught => { error = caught; },
  });
  models.stop();
  assert.deepEqual(seen, [10, 10]);
  assert.equal(completed, false);
  assert.match(error.message, /connection changed/);
  assert.equal(models.count(), 0);
  assert.equal(calls.length, 1);
});
test('an install answer that arrives after a connection change is dropped', async () => {
  fresh(); let release;
  reply = () => new Promise(resolve => { release = resolve; });
  let error; let completed = false; const seen = [];
  const pending = api.installBackend('constructor', 'cpu', {
    onProgress: data => seen.push(data), onComplete: () => { completed = true; }, onError: caught => { error = caught; },
  });
  await tick(); connect('http://stream-late.test', 'private-late-key');
  release(json({ status: 'completed', complete: true }));
  await pending;
  assert.match(error.message, /connection changed/);
  assert.equal(completed, false);
  assert.deepEqual(seen, []);
});
test('an install whose download list is read from another server is not reported complete', async () => {
  fresh();
  reply = url => {
    if (url.endsWith('/install')) return json({ status: 'started' });
    connect('http://stream-list.test', 'private-list-key');
    return json({ downloads: [{ id: 'backend:constructor:cpu', status: 'completed', complete: true, running: false }] });
  };
  let error; let completed = false;
  await api.installBackend('constructor', 'cpu', { onComplete: () => { completed = true; }, onError: caught => { error = caught; } });
  assert.equal(completed, false);
  assert.match(error.message, /connection changed/);
});
test('a pull stream from the previous server stops applying progress and completes nothing', async () => {
  fresh(); let feed;
  reply = () => sse(controller => { feed = controller; controller.enqueue(event('progress', { percent: 5 })); });
  const seen = []; let error; let completed = false; const models = notifications();
  await api.pullModel('m', {
    onProgress: data => {
      seen.push(data.percent);
      connect('http://pull-other.test', 'private-pull-key');
      feed.enqueue(event('complete', { percent: 100, status: 'completed' }));
      feed.close();
    },
    onComplete: () => { completed = true; }, onError: caught => { error = caught; },
  });
  models.stop();
  assert.deepEqual(seen, [5]);
  assert.equal(completed, false);
  assert.match(error.message, /connection changed/);
  assert.equal(models.count(), 0);
  assert.equal(calls.length, 1);
});
test('a pull never polls another server for its download', async () => {
  fresh(); const origin = api.connectionSnapshot().apiUrl;
  reply = url => {
    if (url.endsWith('/pull')) return json({ status: 'downloading', running: true });
    connect('http://pull-poll.test', 'private-poll-key');
    return json({ downloads: [{ model_name: 'm', type: 'model', status: 'completed', complete: true, running: false }] });
  };
  let error; let completed = false;
  await api.pullModel('m', { onComplete: () => { completed = true; }, onError: caught => { error = caught; } });
  assert.equal(completed, false);
  assert.match(error.message, /connection changed/);
  assert.deepEqual(calls.map(call => call.url), [origin + '/pull', origin + '/downloads']);
});
test('a pull stream that ends before a connection change does not finish on the new server', async () => {
  fresh(); const origin = api.connectionSnapshot().apiUrl;
  reply = url => {
    if (url.endsWith('/pull')) return sse(controller => { controller.enqueue(event('complete', { status: 'completed', complete: true, running: false })); controller.close(); });
    connect('http://pull-terminal.test', 'private-terminal-key');
    throw new TypeError('network changed');
  };
  let error; let completed = false;
  await api.pullModel('m', { onComplete: () => { completed = true; }, onError: caught => { error = caught; } });
  assert.equal(completed, false);
  assert.match(error.message, /connection changed/);
  assert.deepEqual(calls.map(call => call.url), [origin + '/pull', origin + '/downloads']);
});
test('generated files that arrive after a connection change are dropped', async () => {
  const created = [];
  const original = URL.createObjectURL;
  URL.createObjectURL = blob => { created.push(blob); return 'blob:test'; };
  try {
    for (const request of [
      () => api.audioGeneration('m', 'rain'),
      () => api.model3dGeneration('m', 'data:image/png;base64,AA=='),
      () => api.textToSpeech('m', 'hello'),
    ]) {
      fresh(); let release;
      reply = () => new Promise(resolve => { release = resolve; });
      const pending = request();
      await tick(); connect('http://file-late.test', 'private-file-key');
      release(new Response(new Uint8Array([1, 2, 3])));
      await assert.rejects(pending, /connection changed/);
    }
    assert.equal(created.length, 0);
    fresh(); reply = () => new Response(new Uint8Array([1, 2, 3]));
    assert.equal((await api.textToSpeech('m', 'hello')).url, 'blob:test');
    assert.equal(created.length, 1);
  } finally { URL.createObjectURL = original; }
});
test('a log stream is closed and silenced when the connection changes', async () => {
  fresh(); sockets.length = 0;
  const entries = []; let disconnected = 0; const errors = [];
  const handle = api.connectLogStream({
    onEntry: entry => entries.push(entry.line), onDisconnected: () => { disconnected += 1; }, onError: message => errors.push(message),
  });
  const socket = sockets[0];
  assert(socket.url.startsWith(`ws://stream-${server}.test/logs/stream`));
  socket.open();
  socket.message({ type: 'logs.entry', entry: { seq: 1, line: 'first' } });
  let late;
  const stop = api.onConnectionChange(() => { late = () => socket.dispatchEvent(Object.assign(new Event('message'), { data: JSON.stringify({ type: 'logs.entry', entry: { seq: 2, line: 'stale' } }) })); });
  connect('http://log-other.test', 'private-log-key');
  stop(); late();
  assert.equal(socket.readyState, FakeSocket.CLOSED);
  assert.deepEqual(entries, ['first']);
  assert.equal(disconnected, 1);
  assert.deepEqual(errors, []);
  assert.equal(sockets.length, 1);
  handle.close();
});
test('a log stream that fails before opening does not fall back onto another server', async () => {
  fresh(); sockets.length = 0;
  let disconnected = 0; const errors = [];
  const handle = api.connectLogStream({ onDisconnected: () => { disconnected += 1; }, onError: message => errors.push(message) });
  connect('http://log-early.test', 'private-early-key');
  await tick();
  assert.equal(sockets.length, 1);
  assert.equal(sockets[0].readyState, FakeSocket.CLOSED);
  assert.equal(disconnected, 1);
  assert.deepEqual(errors, []);
  handle.close();
});
test('a log stream on an unchanged connection keeps delivering and releases its listener on close', async () => {
  fresh(); sockets.length = 0;
  const entries = [];
  const handle = api.connectLogStream({ onEntry: entry => entries.push(entry.line) });
  sockets[0].open();
  sockets[0].message({ type: 'logs.entry', entry: { seq: 1, line: 'one' } });
  sockets[0].message({ type: 'logs.entry', entry: { seq: 2, line: 'two' } });
  assert.deepEqual(entries, ['one', 'two']);
  assert.deepEqual(sockets[0].sent, [{ type: 'logs.subscribe', after_seq: null }]);
  await tick();
  handle.close();
  assert.equal(sockets[0].readyState, FakeSocket.CLOSED);
  let closes = 0;
  sockets[0].close = () => { closes += 1; };
  connect('http://log-after.test', 'private-after-key');
  assert.equal(closes, 0);
});
test('a realtime transcription socket is closed and silenced when the connection changes', async () => {
  fresh(); sockets.length = 0;
  const text = []; let disconnected = 0; const errors = [];
  const pending = api.connectRealtimeTranscription('whisper', {
    onTranscription: (value, final) => text.push([value, final]), onDisconnected: () => { disconnected += 1; }, onError: message => errors.push(message),
  });
  await tick();
  const socket = sockets[0];
  assert(socket.url.includes('/v1/realtime?'));
  socket.open();
  const handle = await pending;
  socket.message({ type: 'conversation.item.input_audio_transcription.delta', delta: 'hel' });
  let late;
  const stop = api.onConnectionChange(() => { late = () => socket.dispatchEvent(Object.assign(new Event('message'), { data: JSON.stringify({ type: 'conversation.item.input_audio_transcription.completed', transcript: 'stale' }) })); });
  connect('http://realtime-other.test', 'private-realtime-key');
  stop(); late();
  assert.equal(handle.isConnected(), false);
  assert.deepEqual(text, [['hel', false]]);
  assert.equal(disconnected, 1);
  assert.deepEqual(errors, []);
});
test('a realtime socket that fails before opening does not retry on another server', async () => {
  fresh(); sockets.length = 0;
  const pending = api.connectRealtimeTranscription('whisper', {});
  await tick();
  connect('http://realtime-early.test', 'private-realtime-early-key');
  await assert.rejects(pending, /connection changed/);
  assert.equal(sockets.length, 1);
  assert.equal(calls.length, 0);
});

run();
