const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/features/videoGeneration/videoJob.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  fileName: filename,
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
const mod = { exports: {} };
Function('exports', 'module', compiled.outputText)(mod.exports, mod);
const { runVideoJob, isVideoJobCanceled, videoFileExtension } = mod.exports;

function fakeServer(statuses, final = {}) {
  const calls = { create: [], get: 0, content: 0, cancel: [] };
  let index = 0;
  return {
    calls,
    transport: {
      async create(body) {
        calls.create.push(body);
        return { id: 'job-1', status: 'queued' };
      },
      async get(id) {
        assert.equal(id, 'job-1');
        calls.get += 1;
        const status = statuses[Math.min(index, statuses.length - 1)];
        index += 1;
        return { id, status, ...(status === statuses[statuses.length - 1] ? final : {}) };
      },
      async content(id) {
        assert.equal(id, 'job-1');
        calls.content += 1;
        return new Blob(['clip'], { type: 'video/webm' });
      },
      async cancel(id) {
        calls.cancel.push(id);
      },
    },
  };
}

const noSleep = async () => {};

(async () => {
  {
    const server = fakeServer(['running', 'running', 'completed'], { output_format: 'webm', duration: 2.06 });
    const seen = [];
    const result = await runVideoJob(server.transport, { model: 'Wan', prompt: 'a cat' }, {
      sleep: noSleep,
      onStatus: record => seen.push(record.status),
    });
    assert.deepEqual(server.calls.create, [{ model: 'Wan', prompt: 'a cat' }]);
    assert.deepEqual(seen, ['queued', 'running', 'running', 'completed']);
    assert.equal(server.calls.content, 1);
    assert.equal(result.blob.size, 4);
    assert.equal(result.record.duration, 2.06);
    assert.deepEqual(server.calls.cancel, []);
  }

  {
    const server = fakeServer(['failed'], { error: 'out of memory' });
    await assert.rejects(
      runVideoJob(server.transport, {}, { sleep: noSleep }),
      /Video generation failed: out of memory/,
    );
    assert.equal(server.calls.content, 0);
  }

  {
    const server = fakeServer(['canceled']);
    await assert.rejects(runVideoJob(server.transport, {}, { sleep: noSleep }), err => isVideoJobCanceled(err));
  }

  {
    // Stop while the clip is generating must reach /cancel, or the GPU stays busy.
    const server = fakeServer(['running']);
    const controller = new AbortController();
    const job = runVideoJob(server.transport, {}, {
      signal: controller.signal,
      onStatus: record => { if (record.status === 'running') controller.abort(); },
      sleep: noSleep,
    });
    await assert.rejects(job, err => isVideoJobCanceled(err));
    assert.deepEqual(server.calls.cancel, ['job-1']);
    assert.equal(server.calls.content, 0);
  }

  {
    // Stop only settles once the server reports the job ended, so the model
    // is not reloaded while its backend is still shutting down.
    const statuses = ['running', 'running', 'running', 'canceled'];
    let index = 0;
    const server = fakeServer(['running']);
    server.transport.get = async id => ({ id, status: statuses[Math.min(index++, statuses.length - 1)] });
    const controller = new AbortController();
    const seen = [];
    const job = runVideoJob(server.transport, {}, {
      signal: controller.signal,
      onStatus: record => {
        seen.push(record.status);
        if (record.status === 'running') controller.abort();
      },
      sleep: noSleep,
    });
    await assert.rejects(job, err => isVideoJobCanceled(err));
    assert.deepEqual(seen, ['queued', 'running', 'canceling']);
    assert.equal(index, 4, 'polls until the server reports canceled');
  }

  {
    // A server that never reports the end must not hang Stop forever.
    const server = fakeServer(['running']);
    const controller = new AbortController();
    const job = runVideoJob(server.transport, {}, {
      signal: controller.signal,
      onStatus: record => { if (record.status === 'running') controller.abort(); },
      sleep: noSleep,
      maxCancelPolls: 5,
    });
    await assert.rejects(job, err => isVideoJobCanceled(err));
    assert.equal(server.calls.get, 1 + 5);
  }

  {
    const server = fakeServer(['running']);
    server.transport.cancel = async () => { throw new Error('server gone'); };
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(runVideoJob(server.transport, {}, { signal: controller.signal, sleep: noSleep }), err => isVideoJobCanceled(err));
    assert.deepEqual(server.calls.create, [], 'an already stopped request must not start a job');
  }

  {
    // A short network blip must not lose a clip that keeps generating.
    const server = fakeServer(['completed'], { output_format: 'webm' });
    const get = server.transport.get;
    let failures = 2;
    server.transport.get = async id => {
      if (failures > 0) { failures -= 1; throw new Error('blip'); }
      return get(id);
    };
    const result = await runVideoJob(server.transport, {}, { sleep: noSleep });
    assert.equal(result.blob.size, 4);
  }

  {
    const server = fakeServer(['running']);
    server.transport.get = async () => { throw new Error('down'); };
    await assert.rejects(runVideoJob(server.transport, {}, { sleep: noSleep, maxPollFailures: 3 }), /down/);
  }

  assert.equal(videoFileExtension({ id: 'x', status: 'completed', output_format: 'webp' }), 'webp');
  assert.equal(videoFileExtension({ id: 'x', status: 'completed', mime_type: 'video/x-msvideo' }), 'avi');
  assert.equal(videoFileExtension({ id: 'x', status: 'completed' }), 'webm');

  console.log('Video job regression checks passed.');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
