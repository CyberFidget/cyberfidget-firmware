// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The transcription engine facade (src/js/engine.js) against a fake Worker:
// a worker that fails to load must be diagnosed (incomplete card copy vs lost
// connection), must be dropped so the next load starts a fresh one, a load
// that goes quiet must time out instead of hanging - but a long, legitimate
// session build must not - overlapping loads must not answer each other, and
// removing the download must not strand anything waiting on the worker.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';

// ---- browser stand-ins (just enough for engine.js + db.js) ----

const workers = [];
class FakeWorker {
  constructor(url, opts) {
    this.url = url;
    this.opts = opts;
    this.posted = [];
    this.terminated = false;
    this.onmessage = null;
    this.onerror = null;
    workers.push(this);
  }
  postMessage(msg) { this.posted.push(msg); }
  terminate() { this.terminated = true; }
  // Test drivers.
  emit(data) { this.onmessage && this.onmessage({ data }); }
  fail(event) { this.onerror && this.onerror(event); }
  loads() { return this.posted.filter((m) => m.type === 'load'); }
  lastLoad() { const l = this.loads(); return l[l.length - 1]; }
  // Answer the most recent load request (the worker echoes its id).
  loaded() { this.emit({ type: 'loaded', id: this.lastLoad().id, device: 'wasm' }); }
}

// Minimal IndexedDB: every open succeeds, every transaction completes. Uses
// microtasks (not timers) so it keeps working while timers are mocked.
const later = (fn) => Promise.resolve().then(fn);
const fakeDb = {
  version: 1,
  objectStoreNames: { contains: () => true },
  close() {},
  transaction() {
    const t = {
      objectStore: () => ({
        put: () => ({ result: undefined }),
        delete: () => ({ result: undefined }),
        clear: () => ({ result: undefined }),
        openCursor: () => ({}),
        get: () => { const r = {}; later(() => r.onsuccess && r.onsuccess()); return r; },
      }),
    };
    later(() => t.oncomplete && t.oncomplete());
    return t;
  },
};
globalThis.indexedDB = {
  open() {
    const req = {};
    later(() => { req.result = fakeDb; req.onsuccess && req.onsuccess(); });
    return req;
  },
};
globalThis.window = {};
globalThis.Worker = FakeWorker;

// How the device answers a probe of a companion file.
let deviceAnswer = 'missing';
const probed = [];
globalThis.fetch = async (url) => {
  probed.push(url);
  if (deviceAnswer === 'offline') throw new TypeError('Failed to fetch');
  if (deviceAnswer === 'missing') return new Response('not found', { status: 404, headers: { 'content-type': 'text/plain' } });
  return new Response('', { status: 200, headers: { 'content-type': 'text/javascript' } });
};

const engine = await import('../src/js/engine.js');
const [MODEL_A, MODEL_B] = engine.listModels().map((m) => m.id);

// Track a promise's state without awaiting it.
function watch(p) {
  const s = { state: 'pending', value: undefined };
  p.then((v) => { s.state = 'resolved'; s.value = v; },
    (e) => { s.state = 'rejected'; s.value = e; });
  return s;
}
const flush = async () => { for (let i = 0; i < 50; i++) await Promise.resolve(); };
const settle = () => new Promise((r) => setImmediate(r));   // lets a fake fetch finish
const MIN = 60 * 1000;

test('a worker script the device does not have reports the card copy as incomplete', async () => {
  deviceAnswer = 'missing';
  const p = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 1);
  assert.equal(workers[0].lastLoad().type, 'load');

  // A script load failure fires a bare error event: no message.
  workers[0].fail({ type: 'error' });
  assert.equal(workers[0].terminated, true, 'the dead worker is dropped at once');
  await settle(); await flush();

  assert.deepEqual(probed, ['/web/engine.worker.js'], 'the device was asked about the worker file');
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'card');
  assert.match(p.value.message, /memory card/);
  assert.match(p.value.message, /engine\.worker\.js/);
  assert.doesNotMatch(p.value.message, /unknown|internet/i);
  assert.equal(engine.loaded(), false);
});

test('retrying after a failed worker starts a fresh worker and settles', async () => {
  const p = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 2, 'a new worker is created, not the dead one reused');
  assert.equal(workers[1].lastLoad().type, 'load');

  // A late event from the dropped worker must not touch the new attempt.
  workers[0].fail({ type: 'error' });
  workers[0].emit({ type: 'error', error: 'stale' });
  await settle(); await flush();
  assert.equal(p.state, 'pending');

  workers[1].loaded();
  await flush();
  assert.equal(p.state, 'resolved');
  assert.equal(engine.loaded(), true);
  assert.equal(engine.backend(), 'wasm');
});

test('a worker script the device cannot be reached for reports a lost connection', async () => {
  deviceAnswer = 'offline';
  const p = watch(engine.load(MODEL_B));
  await flush();
  workers[1].fail({ type: 'error' });
  await settle(); await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'connection');
  assert.match(p.value.message, /connection to your Cyber Fidget/);
  assert.doesNotMatch(p.value.message, /memory card|internet/i);
});

test('a worker that fails although the device has its file says to reload', async () => {
  deviceAnswer = 'ok';
  const p = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(workers.length, 3);
  workers[2].fail({ type: 'error' });
  await settle(); await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'engine');
  assert.doesNotMatch(p.value.message, /memory card|internet/i);
});

test('a load that hears nothing times out, drops the worker, and the next load recovers', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(workers.length, 4);

  // Download progress keeps it alive: the clock restarts on every message.
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  workers[3].emit({ type: 'progress', pct: 40, label: 'model.onnx' });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  await flush();
  assert.equal(p.state, 'pending', 'still alive while the download keeps reporting');

  // Silence for the full window: the waiter is rejected, not left hanging.
  t.mock.timers.tick(1);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /stopped responding/);
  assert.equal(workers[3].terminated, true);
  assert.equal(engine.loaded(), false);

  const again = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(workers.length, 5, 'the next load starts a fresh worker');
  workers[4].loaded();
  await flush();
  assert.equal(again.state, 'resolved');
});

test('a long silent session build after a status is not cut off', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  await flush();
  // The worker says it is building / preparing, then computes silently for
  // far longer than the download allowance.
  workers[4].emit({ type: 'building' });
  t.mock.timers.tick(10 * MIN);
  workers[4].emit({ type: 'status', phase: 'preparing' });
  t.mock.timers.tick(10 * MIN);
  await flush();
  assert.equal(p.state, 'pending', 'a slow phone building the session is left alone');
  assert.equal(workers[4].terminated, false);
  workers[4].loaded();
  await flush();
  assert.equal(p.state, 'resolved');
});

test('a status-phase silence still ends eventually', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_B));
  await flush();
  workers[4].emit({ type: 'status', phase: 'warming' });
  t.mock.timers.tick(engine.LOAD_BUSY_MS - 1);
  await flush();
  assert.equal(p.state, 'pending');
  t.mock.timers.tick(1);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /stopped responding/);
  assert.equal(workers[4].terminated, true);
});

test('a finished load leaves no timer behind', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 6);
  workers[5].loaded();
  await flush();
  assert.equal(p.state, 'resolved');
  t.mock.timers.tick(engine.LOAD_BUSY_MS * 2);
  await flush();
  assert.equal(workers[5].terminated, false, 'a ready worker is not timed out');
  assert.equal(engine.loaded(), true);
});

test('overlapping loads run one at a time and only their own reply settles them', async () => {
  const w = workers[5];
  const before = w.loads().length;
  const b = watch(engine.load(MODEL_B));
  const a = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(w.loads().length, before + 1, 'the second load waits for the first');
  const bReq = w.lastLoad();
  assert.equal(bReq.modelId, MODEL_B);

  // A reply for some other request settles nothing.
  w.emit({ type: 'loaded', id: bReq.id + 1000, device: 'wasm' });
  await flush();
  assert.equal(b.state, 'pending');

  w.emit({ type: 'loaded', id: bReq.id, device: 'wasm' });
  await flush();
  assert.equal(b.state, 'resolved');
  assert.equal(a.state, 'pending', 'B finishing does not mark A ready');
  const aReq = w.lastLoad();
  assert.equal(aReq.modelId, MODEL_A);
  assert.notEqual(aReq.id, bReq.id);

  w.emit({ type: 'loaded', id: aReq.id, device: 'wasm' });
  await flush();
  assert.equal(a.state, 'resolved');
});

test('a failed load does not block the next queued load', async () => {
  const w = workers[5];
  const b = watch(engine.load(MODEL_B));
  const a = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'Failed to fetch' });
  await flush();
  assert.equal(b.state, 'rejected');
  assert.equal(w.lastLoad().modelId, MODEL_A, 'the queued load went ahead');
  w.loaded();
  await flush();
  assert.equal(a.state, 'resolved');
});

test('removing the download fails whatever was waiting on the worker', async () => {
  const w = workers[5];
  const tr = watch(engine.transcribe(new Float32Array(16000), MODEL_A));
  const ld = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(tr.state, 'pending');
  assert.equal(ld.state, 'pending');

  await engine.dropDownload();
  await flush();
  assert.equal(w.terminated, true);
  assert.equal(tr.state, 'rejected', 'a running transcription is not left hanging');
  assert.equal(ld.state, 'rejected', 'a running load is not left hanging');
  assert.equal(engine.loaded(), false);

  // The dropped worker's late reply can't resolve anything on a new one.
  const again = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(workers.length, 7);
  w.emit({ type: 'loaded', id: workers[6].lastLoad().id, device: 'wasm' });
  await flush();
  assert.equal(again.state, 'pending');
  workers[6].loaded();
  await flush();
  assert.equal(again.state, 'resolved');
});

test('worker errors naming a device file or a lost connection keep that meaning', async () => {
  const w = workers[6];
  let p = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'x', missing: 'vendor/ort/ort-wasm-simd-threaded.wasm' });
  await flush();
  assert.equal(p.value.kind, 'card');
  assert.match(p.value.message, /vendor\/ort\/ort-wasm-simd-threaded\.wasm/);

  p = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'x', lost: true });
  await flush();
  assert.equal(p.value.kind, 'connection');

  p = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'Failed to fetch' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, undefined, 'an internet download failure keeps the internet advice');
  assert.equal(p.value.message, 'Failed to fetch');
});

test('a crash inside a running worker keeps its message and drops the worker', async () => {
  const w = workers[6];
  const p = watch(engine.load(MODEL_A));
  await flush();
  w.fail({ type: 'error', message: 'out of memory' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /out of memory/);
  assert.equal(w.terminated, true);

  const again = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 8);
  workers[7].loaded();
  await flush();
  assert.equal(again.state, 'resolved');
});
